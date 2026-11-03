/*
 * config_store.c: profile journal in main flash. Same record format and
 * journal design as platform/nuvoton-nuc1xx/src/config_store.c (described
 * in config_store.h), on BOARD_CONFIG_PAGES pages of BOARD_FLASH_PAGE bytes
 * from BOARD_CONFIG_BASE (inside the vendor "EEPROM" area, outside the
 * checksummed app range). Words are programmed as two halfwords (the F1
 * flash programs 16 bits at a time); a torn word fails the record CRC.
 *
 * Erases stall the CPU (~20-40 ms per 1 KB page); flash.c kicks the IWDG.
 *
 * SPDX-License-Identifier: MIT
 */
#include "config_store.h"
#include "params.h"
#include "flash.h"
#include "crc32.h"
#include "gsx.h"
#include "board.h"

#define CS_BASE   BOARD_CONFIG_BASE
#define CS_SIZE   (BOARD_CONFIG_PAGES * BOARD_FLASH_PAGE)
#define CS_PAGE   BOARD_FLASH_PAGE

/* Stay inside the vendor EEPROM area: never the checksummed app range
 * (below 0x0801B400) nor the update-request flag page (0x0801FC00). */
_Static_assert(CS_BASE >= 0x0801B400u && CS_BASE + CS_SIZE <= 0x0801FC00u,
               "config pages must lie in 0x0801B400-0x0801FBFF");
_Static_assert(CS_BASE % CS_PAGE == 0, "config base must be page aligned");
#define CS_HDR_WORDS 3u                    /* magic, seq, count  */
#define CS_MAX_ENTRIES ((CS_PAGE / 4u - CS_HDR_WORDS - 1u) / 2u)

typedef struct {
    uint32_t addr;
    uint32_t words;
    uint32_t seq;
} rec_t;

static uint32_t page_end(uint32_t a)
{
    return (a & ~(CS_PAGE - 1u)) + CS_PAGE;
}

static int rec_check(uint32_t a, rec_t *r)
{
    if (flash_read(a) != CONFIG_STORE_MAGIC)
        return 0;
    uint32_t count = flash_read(a + 8u);
    if (count > CS_MAX_ENTRIES)
        return 0;
    uint32_t words = CS_HDR_WORDS + 2u * count + 1u;
    if (a + 4u * words > page_end(a))
        return 0;
    uint32_t crc = CRC32_INIT;
    for (uint32_t i = 0; i < words - 1u; i++)
        crc = crc32_word(crc, flash_read(a + 4u * i));
    if (~crc != flash_read(a + 4u * (words - 1u)))
        return 0;
    r->addr = a;
    r->words = words;
    r->seq = flash_read(a + 4u);
    return 1;
}

static int find_newest(rec_t *best)
{
    int found = 0;
    for (uint32_t pg = CS_BASE; pg < CS_BASE + CS_SIZE; pg += CS_PAGE) {
        uint32_t a = pg;
        rec_t r;
        /* Records are packed from the page start; stop at the first word
         * that doesn't start a valid record (blank, or a torn write). */
        while (a + 16u <= pg + CS_PAGE && rec_check(a, &r)) {
            if (!found || (int32_t)(r.seq - best->seq) > 0) {
                *best = r;
                found = 1;
            }
            a += 4u * r.words;
        }
    }
    return found;
}

static int stored(const param_def_t *p)
{
    return !(p->flags & (PF_VOLATILE | PF_READ_ONLY));
}

int config_store_load(void)
{
    rec_t r;
    if (!find_newest(&r))
        return 0;
    uint32_t count = flash_read(r.addr + 8u);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = r.addr + 4u * (CS_HDR_WORDS + 2u * i);
        uint32_t key = flash_read(a);
        int32_t value = (int32_t)flash_read(a + 4u);
        const param_def_t *p = params_find((uint16_t)(key & 0xFFFFu));
        if (p && stored(p))
            params_set((uint16_t)(key & 0xFFFFu), (uint16_t)(key >> 16), value, 0);
    }
    return 1;
}

static int blank(uint32_t a, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++)
        if (flash_read(a + 4u * i) != 0xFFFFFFFFu)
            return 0;
    return 1;
}

/* Program one word, read it back, fold it into the CRC. */
static int put(uint32_t *a, uint32_t w, uint32_t *crc)
{
    if (flash_program_word(*a, w) != 0 || flash_read(*a) != w)
        return -1;
    *crc = crc32_word(*crc, w);
    *a += 4u;
    return 0;
}

static int commit(void);

int config_store_commit(void)
{
    flash_unlock();
    int st = commit();
    flash_lock();
    return st;
}

static int commit(void)
{
    const param_table_t *t = &app_params;
    uint32_t count = 0;
    for (uint32_t k = 0; k < t->param_count; k++)
        if (stored(&t->params[k]))
            count += t->params[k].count;
    if (count > CS_MAX_ENTRIES)
        return GSX_FLASH_ERR;
    uint32_t words = CS_HDR_WORDS + 2u * count + 1u;

    rec_t last;
    uint32_t a, seq = 1;
    int need_erase = 1;
    if (find_newest(&last)) {
        seq = last.seq + 1u;
        a = last.addr + 4u * last.words;
        if (a + 4u * words <= page_end(last.addr) && blank(a, words)) {
            need_erase = 0;
        } else {
            a = page_end(last.addr);
            if (a >= CS_BASE + CS_SIZE)
                a = CS_BASE;
        }
    } else {
        a = CS_BASE;
    }
    if (need_erase && flash_erase_page(a) != 0)
        return GSX_FLASH_ERR;

    uint32_t start = a, crc = CRC32_INIT;
    if (put(&a, CONFIG_STORE_MAGIC, &crc) || put(&a, seq, &crc) || put(&a, count, &crc))
        return GSX_FLASH_ERR;
    for (uint32_t k = 0; k < t->param_count; k++) {
        const param_def_t *p = &t->params[k];
        if (!stored(p))
            continue;
        for (uint32_t i = 0; i < p->count; i++)
            if (put(&a, (uint32_t)p->id | i << 16, &crc) ||
                put(&a, (uint32_t)p->values[i], &crc))
                return GSX_FLASH_ERR;
    }
    if (flash_program_word(a, ~crc) != 0)
        return GSX_FLASH_ERR;
    rec_t check;
    return rec_check(start, &check) ? GSX_OK : GSX_FLASH_ERR;
}

int config_store_erase(void)
{
    int st = GSX_OK;
    flash_unlock();
    for (uint32_t pg = CS_BASE; pg < CS_BASE + CS_SIZE; pg += CS_PAGE)
        if (flash_erase_page(pg) != 0)
            st = GSX_FLASH_ERR;
    flash_lock();
    return st;
}

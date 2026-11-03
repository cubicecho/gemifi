/*
 * config_store.c: per-slot settings records (core/include/config_store.h) in
 * main flash: BOARD_CONFIG_PAGES (2) pages of BOARD_FLASH_PAGE bytes from
 * BOARD_CONFIG_BASE, inside the vendor "EEPROM" area, outside the
 * checksummed app range. Records use the v2 format from config_store.h.
 * Words are programmed as two halfwords (the F1 flash programs 16 bits at a
 * time); a torn word fails the record CRC.
 *
 * Two pages, one of them active: the page holding the newest record. The
 * active page always holds the newest record of every slot. A write is
 * appended to it; when it is full, the other page is erased, the newest
 * record of every other slot is copied there unchanged (same seq), and the
 * new record goes last. The new record is what makes the other page the
 * active one, so a power cut before it lands leaves the old page active and
 * complete, and the copies on the other page only tie with their originals.
 *
 * This needs every slot's worst-case record to fit one page together; the
 * builder checks it (tools/builder/gemifi_device.py STORE_BUDGETS, stm32f1).
 * On the Code 61 that is about 5 presets. More need a bigger config area
 * and a journal like the NUC's (issue #13).
 * Old v1 ('GCFG') records are not read: no Code 61 has stored any.
 *
 * Erases stall the CPU (~20-40 ms per 1 KB page); flash.c kicks the IWDG.
 *
 * SPDX-License-Identifier: MIT
 */
#include "config_store.h"
#include "flash.h"
#include "crc32.h"
#include "gsx.h"
#include "board.h"

#define CS_BASE   BOARD_CONFIG_BASE
#define CS_PAGE   BOARD_FLASH_PAGE
#define CS_SIZE   (BOARD_CONFIG_PAGES * CS_PAGE)

/* Stay inside the vendor EEPROM area: never the checksummed app range
 * (below 0x0801B400) nor the update-request flag page (0x0801FC00). */
_Static_assert(CS_BASE >= 0x0801B400u && CS_BASE + CS_SIZE <= 0x0801FC00u,
               "config pages must lie in 0x0801B400-0x0801FBFF");
_Static_assert(CS_BASE % CS_PAGE == 0, "config base must be page aligned");
_Static_assert(BOARD_CONFIG_PAGES == 2u, "the store swaps between exactly two pages");

#define HDR_WORDS   4u                     /* magic, seq, slot | flags | revision, count */
#define NAME_WORDS  (CS_NAME_LEN / 4u)
#define PAGE_WORDS  (CS_PAGE / 4u)
#define MAX_NAMED   ((PAGE_WORDS - HDR_WORDS - NAME_WORDS - 1u) / 2u)

typedef struct {
    uint32_t addr;
    uint32_t words;
    uint32_t seq;
    uint32_t hdr;                          /* slot | flags << 8 | revision << 16 */
    uint32_t count;
    uint32_t ent;                          /* address of the first entry */
} rec_t;

#define REC_SLOT(r) ((uint8_t)((r)->hdr & 0xFFu))

static uint32_t page_of(uint32_t a)
{
    return a & ~(CS_PAGE - 1u);
}

static uint32_t other_page(uint32_t pg)
{
    return pg == CS_BASE ? CS_BASE + CS_PAGE : CS_BASE;
}

static int seq_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

/* A valid v2 record at a (inside its page)? */
static int rec_check(uint32_t a, rec_t *r)
{
    uint32_t end = page_of(a) + CS_PAGE;
    if (a + 4u * (HDR_WORDS + 1u) > end || flash_read(a) != CONFIG_STORE_MAGIC_V2)
        return 0;
    uint32_t hdr = flash_read(a + 8u), count = flash_read(a + 12u);
    uint32_t named = (hdr >> 8) & CS_F_NAME;
    if (count > MAX_NAMED)
        return 0;
    uint32_t words = HDR_WORDS + (named ? NAME_WORDS : 0u) + 2u * count + 1u;
    if (a + 4u * words > end)
        return 0;
    uint32_t crc = CRC32_INIT;
    for (uint32_t i = 0; i < words - 1u; i++)
        crc = crc32_word(crc, flash_read(a + 4u * i));
    if (~crc != flash_read(a + 4u * (words - 1u)))
        return 0;
    r->addr = a;
    r->words = words;
    r->seq = flash_read(a + 4u);
    r->hdr = hdr;
    r->count = count;
    r->ent = a + 4u * (HDR_WORDS + (named ? NAME_WORDS : 0u));
    return 1;
}

/* Walk a page's records (packed from its start, up to the first word that
 * doesn't start one). Returns the address after the last one. */
static uint32_t walk(uint32_t pg, void (*fn)(const rec_t *r, void *ctx), void *ctx)
{
    uint32_t a = pg;
    rec_t r;
    while (rec_check(a, &r)) {
        if (fn)
            fn(&r, ctx);
        a += 4u * r.words;
    }
    return a;
}

typedef struct {
    int all;                               /* any slot (find the newest record of all) */
    uint8_t slot;
    int found;
    rec_t best;
} find_t;

static void find_cb(const rec_t *r, void *ctx)
{
    find_t *f = ctx;
    if (!f->all && REC_SLOT(r) != f->slot)
        return;
    if (!f->found || seq_newer(r->seq, f->best.seq)) {
        f->best = *r;
        f->found = 1;
    }
}

/* The newest record of `slot` (all = 0) or of any slot (all = 1). */
static int newest(int all, uint8_t slot, rec_t *out)
{
    find_t f = { all, slot, 0, { 0 } };
    walk(CS_BASE, find_cb, &f);
    walk(CS_BASE + CS_PAGE, find_cb, &f);
    if (f.found)
        *out = f.best;
    return f.found;
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

typedef struct {
    uint32_t hdr;
    uint32_t words;
    const uint32_t *name;                  /* NAME_WORDS words, or NULL */
    const cs_entry_t *e;
    uint32_t n;
} new_rec_t;

static int emit(uint32_t a, uint32_t seq, const new_rec_t *nr)
{
    uint32_t start = a, crc = CRC32_INIT;
    if (put(&a, CONFIG_STORE_MAGIC_V2, &crc) || put(&a, seq, &crc) || put(&a, nr->hdr, &crc) ||
        put(&a, nr->n, &crc))
        return -1;
    for (uint32_t i = 0; nr->name && i < NAME_WORDS; i++)
        if (put(&a, nr->name[i], &crc))
            return -1;
    for (uint32_t i = 0; i < nr->n; i++)
        if (put(&a, (uint32_t)nr->e[i].id | (uint32_t)nr->e[i].index << 16, &crc) ||
            put(&a, (uint32_t)nr->e[i].value, &crc))
            return -1;
    if (flash_program_word(a, ~crc) != 0)
        return -1;
    rec_t check;
    return rec_check(start, &check) ? 0 : -1;
}

typedef struct {
    uint8_t skip;                          /* the slot being written */
    uint32_t to;                           /* next free word on the new page */
    int err;
} copy_t;

/* Copy r to the new page if it is the newest record of its slot. */
static void copy_cb(const rec_t *r, void *ctx)
{
    copy_t *c = ctx;
    rec_t best;
    if (c->err || REC_SLOT(r) == c->skip || !newest(0, REC_SLOT(r), &best) || best.seq != r->seq)
        return;
    if (c->to + 4u * r->words > page_of(c->to) + CS_PAGE) {
        c->err = 1;                        /* the builder's budget rules this out */
        return;
    }
    for (uint32_t i = 0; i < r->words; i++) {
        uint32_t w = flash_read(r->addr + 4u * i);
        if (flash_program_word(c->to, w) != 0 || flash_read(c->to) != w) {
            c->err = 1;
            return;
        }
        c->to += 4u;
    }
}

static int append(uint8_t slot, const new_rec_t *nr)
{
    rec_t last;
    uint32_t seq = 1, act = CS_BASE + CS_PAGE;     /* empty store: start on CS_BASE */
    if (newest(1, 0, &last)) {
        seq = last.seq + 1u;
        act = page_of(last.addr);
        uint32_t a = walk(act, 0, 0);
        if (a + 4u * nr->words <= act + CS_PAGE && blank(a, nr->words))
            return emit(a, seq, nr);
    }
    uint32_t to = other_page(act);
    if (flash_erase_page(to) != 0)
        return -1;
    copy_t c = { slot, to, 0 };
    walk(act, copy_cb, &c);
    if (c.err || c.to + 4u * nr->words > to + CS_PAGE)
        return -1;
    return emit(c.to, seq, nr);
}

static int locked_append(uint8_t slot, const new_rec_t *nr)
{
    flash_unlock();
    int st = append(slot, nr);
    flash_lock();
    return st ? GSX_FLASH_ERR : GSX_OK;
}

int config_store_write(uint8_t slot, uint16_t revision, const char *name,
                       const cs_entry_t *e, uint32_t n)
{
    uint32_t nw[NAME_WORDS] = { 0 };
    new_rec_t nr = { (uint32_t)slot | (uint32_t)revision << 16, 0, 0, e, n };
    if (name && name[0]) {
        for (uint32_t i = 0; i < CS_NAME_LEN && name[i]; i++)
            nw[i / 4u] |= (uint32_t)(uint8_t)name[i] << (8u * (i % 4u));
        nr.hdr |= CS_F_NAME << 8;
        nr.name = nw;
    }
    if (n > MAX_NAMED)
        return GSX_BAD_LEN;
    nr.words = HDR_WORDS + (nr.name ? NAME_WORDS : 0u) + 2u * n + 1u;
    return locked_append(slot, &nr);
}

static int tombstone(const rec_t *r)
{
    return (r->hdr >> 8) & CS_F_TOMBSTONE;
}

int config_store_read(uint8_t slot, cs_slot_info_t *info,
                      void (*cb)(const cs_entry_t *e, void *ctx), void *ctx)
{
    rec_t r;
    if (!newest(0, slot, &r) || tombstone(&r))
        return 0;
    if (info) {
        uint32_t named = (r.hdr >> 8) & CS_F_NAME;
        info->revision = (uint16_t)(r.hdr >> 16);
        info->flags = (uint8_t)(r.hdr >> 8);
        for (uint32_t i = 0; i < CS_NAME_LEN; i++)
            info->name[i] = named ? (char)(flash_read(r.addr + 16u + (i & ~3u)) >> (8u * (i % 4u))) : 0;
        info->name[CS_NAME_LEN] = 0;
    }
    for (uint32_t i = 0; cb && i < r.count; i++) {
        uint32_t key = flash_read(r.ent + 8u * i);
        cs_entry_t e = { (uint16_t)key, (uint16_t)(key >> 16), (int32_t)flash_read(r.ent + 8u * i + 4u) };
        cb(&e, ctx);
    }
    return 1;
}

int config_store_erase_slot(uint8_t slot)
{
    rec_t r;
    if (!newest(0, slot, &r) || tombstone(&r))
        return GSX_OK;                     /* nothing stored: nothing to hide */
    new_rec_t nr = { (uint32_t)slot | CS_F_TOMBSTONE << 8, HDR_WORDS + 1u, 0, 0, 0 };
    return locked_append(slot, &nr);
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

uint32_t config_store_max_entries(void)
{
    return MAX_NAMED;
}

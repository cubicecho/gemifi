/*
 * config_store.c: per-slot record journal in data flash. Format in
 * config_store.h.
 *
 * Layout: the 7 data-flash pages below the device-ID page, used as a ring.
 * Records are appended in the head page (the page holding the record with
 * the highest seq) and never span a page. The scanner steps over anything
 * that isn't a valid record (blank words, torn writes), so a torn record
 * only wastes its space. The newest valid record of each slot wins.
 *
 * Invariant: the page after the head holds no live record (no record that
 * is the newest of its slot). Moving the head on is then:
 *   1. erase the next page N (it holds nothing live);
 *   2. copy every live record of the page after it (N+1) into N, each with
 *      a new seq: N+1 then holds nothing live, so the invariant holds again;
 *   3. append the new record (if it doesn't fit, move on again).
 * A power cut in step 2 leaves N+1 with live records while N is the head.
 * The next write finishes the copy first; if the rest doesn't fit (a torn
 * record wasted space), it erases N again: N holds only copies of records
 * that still exist in N+1, so every slot reads the same before and after.
 * So the newest record of a slot is never erased before a newer copy of
 * it exists.
 *
 * A v1 record (slots 0 and 1 at once) is copied as v1 while it is the
 * newest of both, and as a v2 record of the one slot it still answers for
 * once the other has a newer record (same entries: the slot reads the same).
 *
 * No RAM buffers: everything is read from flash as needed (a write scans
 * the journal a few times per record it moves; erases dominate anyway).
 *
 * SPDX-License-Identifier: MIT
 */
#include "config_store.h"
#include "fmc.h"
#include "crc32.h"
#include "gsx.h"
#include "nuc1xx.h"

#define CS_BASE    FLASH_DATA_BASE
#define CS_END     FLASH_DEVID_ADDR                     /* last page: device ID */
#define CS_PAGE    FLASH_PAGE_SIZE
#define CS_PAGES   ((CS_END - CS_BASE) / CS_PAGE)       /* 7 */
#define PAGE_WORDS (CS_PAGE / 4u)
#define V2_HDR     4u                                   /* magic, seq, slot word, count */
#define NAME_WORDS (CS_NAME_LEN / 4u)
#define MAX_PLAIN  ((PAGE_WORDS - V2_HDR - 1u) / 2u)    /* entries, no name: 61 */
#define MAX_NAMED  ((PAGE_WORDS - V2_HDR - NAME_WORDS - 1u) / 2u)   /* 59 */
#define ANY_SLOT   0x100u
#define NO_SLOT    0x100u

typedef struct {
    uint32_t addr;      /* first word                                 */
    uint32_t words;     /* whole record, CRC included                 */
    uint32_t seq;
    uint32_t hdr;       /* v2: slot | flags << 8 | revision << 16     */
    uint32_t count;
    uint32_t ent;       /* address of the first entry                 */
    uint8_t v1;
} rec_t;

/* A new record being written (config_store_write / erase_slot). */
typedef struct {
    uint32_t hdr;
    uint32_t words;
    const uint32_t *name;                   /* NAME_WORDS words, or NULL */
    const cs_entry_t *e;
} new_rec_t;

static uint32_t next_seq;                   /* set by locate() */

static uint32_t page_of(uint32_t a)
{
    return a & ~(CS_PAGE - 1u);
}

static uint32_t page_next(uint32_t pg)
{
    pg += CS_PAGE;
    return pg >= CS_END ? CS_BASE : pg;
}

static int rec_check(uint32_t a, rec_t *r)
{
    uint32_t end = page_of(a) + CS_PAGE, m = fmc_read(a), hdr_words;
    if (m == CONFIG_STORE_MAGIC_V2) {
        r->v1 = 0;
        hdr_words = V2_HDR;
    } else if (m == CONFIG_STORE_MAGIC) {
        r->v1 = 1;
        hdr_words = 3u;
    } else {
        return 0;
    }
    if (a + 4u * hdr_words > end)
        return 0;
    r->hdr = r->v1 ? 0u : fmc_read(a + 8u);
    r->count = fmc_read(a + 4u * (hdr_words - 1u));
    if (r->count > PAGE_WORDS)
        return 0;
    if (r->hdr & (CS_F_NAME << 8))
        hdr_words += NAME_WORDS;
    uint32_t words = hdr_words + 2u * r->count + 1u;
    if (a + 4u * words > end)
        return 0;
    uint32_t crc = CRC32_INIT;
    for (uint32_t i = 0; i < words - 1u; i++)
        crc = crc32_word(crc, fmc_read(a + 4u * i));
    if (~crc != fmc_read(a + 4u * (words - 1u)))
        return 0;
    r->addr = a;
    r->words = words;
    r->seq = fmc_read(a + 4u);
    r->ent = a + 4u * hdr_words;
    return 1;
}

/* The next valid record at or after *a (in any page); moves *a past it. */
static int scan_next(uint32_t *a, rec_t *r)
{
    while (*a < CS_END) {
        if (rec_check(*a, r)) {
            *a += 4u * r->words;
            return 1;
        }
        *a += 4u;
    }
    return 0;
}

static int answers(const rec_t *r, uint32_t slot)
{
    return slot == ANY_SLOT || (r->v1 ? slot <= 1u : (r->hdr & 0xFFu) == slot);
}

/* The newest record answering `slot` (ANY_SLOT: the newest of all). */
static int newest(uint32_t slot, rec_t *best)
{
    int found = 0;
    rec_t r;
    for (uint32_t a = CS_BASE; scan_next(&a, &r);)
        if (answers(&r, slot) && (!found || (int32_t)(r.seq - best->seq) > 0)) {
            *best = r;
            found = 1;
        }
    return found;
}

static int is_newest(const rec_t *r, uint32_t slot)
{
    rec_t b;
    return newest(slot, &b) && b.addr == r->addr;
}

/* Which slots r is the newest record of, minus `skip`: bit 0 = its slot
 * (v2), bits 0/1 = slots 0/1 (v1). */
static uint32_t live_mask(const rec_t *r, uint32_t skip)
{
    if (!r->v1)
        return (r->hdr & 0xFFu) != skip && is_newest(r, r->hdr & 0xFFu);
    uint32_t m = 0;
    for (uint32_t s = 0; s < 2u; s++)
        if (s != skip && is_newest(r, s))
            m |= 1u << s;
    return m;
}

/* A v1 record still live for one slot only is copied as a v2 record of
 * that slot (same entries; capped to what a page holds). */
static uint32_t conv_count(const rec_t *r)
{
    return r->count < MAX_PLAIN ? r->count : MAX_PLAIN;
}

static uint32_t copy_words(const rec_t *r, uint32_t m)
{
    return (r->v1 && m != 3u) ? V2_HDR + 2u * conv_count(r) + 1u : r->words;
}

/* Program one word, read it back, fold it into the CRC. */
static int put(uint32_t *a, uint32_t w, uint32_t *crc)
{
    if (fmc_program(*a, w) != 0 || fmc_read(*a) != w)
        return -1;
    *crc = crc32_word(*crc, w);
    *a += 4u;
    return 0;
}

static int finish(uint32_t start, uint32_t a, uint32_t crc)
{
    rec_t check;
    if (fmc_program(a, ~crc) != 0)
        return -1;
    return rec_check(start, &check) ? 0 : -1;
}

static int copy_rec(uint32_t dst, const rec_t *r, uint32_t m)
{
    uint32_t a = dst, crc = CRC32_INIT, src = r->addr + 8u, n = r->words - 3u;
    if (r->v1 && m != 3u) {
        n = conv_count(r);
        if (put(&a, CONFIG_STORE_MAGIC_V2, &crc) || put(&a, next_seq++, &crc) ||
            put(&a, m >> 1, &crc) || put(&a, n, &crc))    /* slot 0 or 1 */
            return -1;
        src = r->ent;
        n *= 2u;
    } else if (put(&a, fmc_read(r->addr), &crc) || put(&a, next_seq++, &crc)) {
        return -1;
    }
    for (; n; n--, src += 4u)
        if (put(&a, fmc_read(src), &crc))
            return -1;
    return finish(dst, a, crc);
}

/* Copy the live records of page pg (except `skip`'s) to dst, those that
 * fit in `room` words; with dst 0 only count them all. Returns the words
 * used, or -1. */
static int32_t compact(uint32_t pg, uint32_t dst, uint32_t skip, uint32_t room)
{
    uint32_t used = 0;
    rec_t r;
    for (uint32_t a = pg; scan_next(&a, &r) && r.addr < pg + CS_PAGE;) {
        uint32_t m = live_mask(&r, skip), w = copy_words(&r, m);
        if (!m || (dst && used + w > room))
            continue;
        if (dst && copy_rec(dst + 4u * used, &r, m))
            return -1;
        used += w;
    }
    return (int32_t)used;
}

static int emit(uint32_t dst, const new_rec_t *nr)
{
    uint32_t a = dst, crc = CRC32_INIT;
    uint32_t n = (nr->words - V2_HDR - 1u - (nr->name ? NAME_WORDS : 0u)) / 2u;
    if (put(&a, CONFIG_STORE_MAGIC_V2, &crc) || put(&a, next_seq++, &crc) ||
        put(&a, nr->hdr, &crc) || put(&a, n, &crc))
        return -1;
    for (uint32_t i = 0; nr->name && i < NAME_WORDS; i++)
        if (put(&a, nr->name[i], &crc))
            return -1;
    for (uint32_t i = 0; i < n; i++)
        if (put(&a, (uint32_t)nr->e[i].id | (uint32_t)nr->e[i].index << 16, &crc) ||
            put(&a, (uint32_t)nr->e[i].value, &crc))
            return -1;
    return finish(dst, a, crc);
}

/* The head page and its first free address (*fill); sets next_seq. An
 * empty journal reads as a full last page, so the first write erases
 * page 0. */
static uint32_t locate(uint32_t *fill)
{
    rec_t r;
    if (!newest(ANY_SLOT, &r)) {
        next_seq = 1;
        *fill = CS_END;
        return CS_END - CS_PAGE;
    }
    next_seq = r.seq + 1u;
    uint32_t pg = page_of(r.addr), e = pg + CS_PAGE;
    while (e > pg && fmc_read(e - 4u) == 0xFFFFFFFFu)
        e -= 4u;
    *fill = e > r.addr + 4u * r.words ? e : r.addr + 4u * r.words;
    return pg;
}

static int append(uint32_t slot, const new_rec_t *nr)
{
    for (uint32_t i = 0; i <= CS_PAGES; i++) {
        uint32_t fill, head = locate(&fill), nx = page_next(head);
        /* Repair after a power cut mid-copy: the page after the head must
         * hold nothing live. The head then holds only copies of nx's
         * records, so it may be erased if the rest doesn't fit (its
         * originals in nx are then live again and all get copied). */
        int32_t need = compact(nx, 0, NO_SLOT, 0);
        if (need) {
            if (fill + 4u * (uint32_t)need > head + CS_PAGE) {
                if (fmc_erase_page(head))
                    return -1;
                fill = head;
            }
            need = compact(nx, fill, NO_SLOT, PAGE_WORDS);
            if (need < 0)
                return -1;
            fill += 4u * (uint32_t)need;
        }
        if (fill + 4u * nr->words <= head + CS_PAGE)
            return emit(fill, nr);
        /* Move on: erase nx, pull the live records of the page after it
         * into nx. The slot being written stays behind when the new record
         * fits after the others (it is written before that page is erased). */
        uint32_t nn = page_next(nx);
        if (fmc_erase_page(nx))
            return -1;
        need = compact(nn, 0, slot, 0);
        if ((uint32_t)need + nr->words <= PAGE_WORDS) {
            need = compact(nn, nx, slot, PAGE_WORDS);
            if (need < 0)
                return -1;
            return emit(nx + 4u * (uint32_t)need, nr);
        }
        need = compact(nn, nx, NO_SLOT, PAGE_WORDS);
        if (need < 0)
            return -1;
        /* Still no room: fill nx up with live records of the page after
         * nn, so the live records pack together over the rounds. */
        if (compact(page_next(nn), nx + 4u * (uint32_t)need, slot, PAGE_WORDS - (uint32_t)need) < 0)
            return -1;
    }
    return -1;                              /* journal full */
}

int config_store_write(uint8_t slot, uint16_t revision, const char *name,
                       const cs_entry_t *e, uint32_t n)
{
    uint32_t nw[NAME_WORDS] = { 0 };
    new_rec_t nr = { (uint32_t)slot | (uint32_t)revision << 16, 0, 0, e };
    if (name && name[0]) {
        for (uint32_t i = 0; i < CS_NAME_LEN && name[i]; i++)
            nw[i / 4u] |= (uint32_t)(uint8_t)name[i] << (8u * (i % 4u));
        nr.hdr |= CS_F_NAME << 8;
        nr.name = nw;
    }
    if (n > (nr.name ? MAX_NAMED : MAX_PLAIN))
        return GSX_BAD_LEN;
    nr.words = V2_HDR + (nr.name ? NAME_WORDS : 0u) + 2u * n + 1u;
    return append(slot, &nr) ? GSX_FLASH_ERR : GSX_OK;
}

static int tombstone(const rec_t *r)
{
    return !r->v1 && (r->hdr & (CS_F_TOMBSTONE << 8));
}

int config_store_read(uint8_t slot, cs_slot_info_t *info,
                      void (*cb)(const cs_entry_t *e, void *ctx), void *ctx)
{
    rec_t r;
    if (!newest(slot, &r) || tombstone(&r))
        return 0;
    if (info) {
        uint32_t named = r.hdr & (CS_F_NAME << 8);
        info->revision = (uint16_t)(r.hdr >> 16);
        info->flags = (uint8_t)(r.hdr >> 8);
        for (uint32_t i = 0; i < CS_NAME_LEN; i++)
            info->name[i] = named ? (char)(fmc_read(r.addr + 16u + (i & ~3u)) >> (8u * (i % 4u))) : 0;
        info->name[CS_NAME_LEN] = 0;
    }
    for (uint32_t i = 0; cb && i < r.count; i++) {
        uint32_t key = fmc_read(r.ent + 8u * i);
        cs_entry_t e = { (uint16_t)key, (uint16_t)(key >> 16), (int32_t)fmc_read(r.ent + 8u * i + 4u) };
        cb(&e, ctx);
    }
    return 1;
}

int config_store_erase_slot(uint8_t slot)
{
    rec_t r;
    if (!newest(slot, &r) || tombstone(&r))
        return GSX_OK;                      /* nothing stored: nothing to hide */
    new_rec_t nr = { (uint32_t)slot | CS_F_TOMBSTONE << 8, V2_HDR + 1u, 0, 0 };
    return append(slot, &nr) ? GSX_FLASH_ERR : GSX_OK;
}

int config_store_erase(void)
{
    int st = GSX_OK;
    for (uint32_t pg = CS_BASE; pg < CS_END; pg += CS_PAGE)
        if (fmc_erase_page(pg) != 0)
            st = GSX_FLASH_ERR;
    return st;
}

uint32_t config_store_max_entries(void)
{
    return MAX_NAMED;
}

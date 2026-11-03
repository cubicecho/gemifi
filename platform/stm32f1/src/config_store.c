/*
 * config_store.c: per-slot settings records (core/include/config_store.h) in
 * main flash: BOARD_CONFIG_PAGES (2) pages of BOARD_FLASH_PAGE bytes from
 * BOARD_CONFIG_BASE, inside the vendor "EEPROM" area, outside the
 * checksummed app range. Words are programmed as two halfwords (the F1
 * flash programs 16 bits at a time); a torn word fails the record CRC.
 *
 * Two pages, one of them active: the page holding the newest record. The
 * active page always holds the newest record of every slot. A write is
 * appended to it; when it is full, the other page is erased, the newest
 * record of every other slot is copied there unchanged (same seq), and the
 * new record goes last. The new record is what makes the other page the
 * active one, so a power cut before it lands leaves the old page active and
 * complete, and the copies on the other page only tie with their originals.
 * A write that can't fit next to the other slots' records fails with
 * GSX_BAD_LEN before anything is erased.
 *
 * This needs every slot's worst-case record to fit one page together; the
 * builder checks it (tools/builder/gemifi_device.py STORE_BUDGETS, stm32f1,
 * which computes the packed sizes below). A third page (the Code 61 has one
 * more free page) would not add room: a swap needs a whole erased page per
 * live page, so 3 pages still hold 1 page of live records.
 *
 * Record v3, packed (this platform only; the NUC and ESP keep v2), words:
 *
 *   magic 'GCF3' (0x33464347), seq,
 *   slot (bits 0-7) | flags (bits 8-15) | revision (bits 16-31),
 *   dwords (the number of group words; bits 16-31 zero),
 *   name: 4 words if flags & CS_F_NAME (as v2),
 *   dwords words of groups,
 *   crc32 (zlib) over all the words before it.
 *
 * A group holds every entry of one param id:
 *   header: id (bits 0-7) | width (bits 8-9: 0 = int8, 1 = int16, 2 = int32)
 *           | span - 1 (bits 10-23: the indices are 0..span-1) | bits 24-31;
 *   span 1, int8: the header is the whole group, its value in bits 24-31;
 *   otherwise bits 24-31 are 0, then (span > 1 only) ceil(span / 32) bitmap
 *   words (bit i of word k: index 32k + i is stored), then the values in
 *   index order, packed 4 / 2 / 1 per word (low bits first).
 * The width is the smallest that holds every value of the group, so a
 * group's worst case follows from the param's count and range. Ids above
 * 255 or indices above 0x3FFF can't be stored (GSX_BAD_LEN).
 *
 * Old records: v2 ('GCF2', 2 words per entry) still load and are copied
 * as they are on a swap; the next write of a slot replaces it with v3.
 * v1 ('GCFG') records are not read: no Code 61 has stored any.
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

#define MAGIC_V3    0x33464347u            /* 'GCF3' */
#define HDR_WORDS   4u                     /* magic, seq, slot | flags | revision, count */
#define NAME_WORDS  (CS_NAME_LEN / 4u)
#define PAGE_WORDS  (CS_PAGE / 4u)
#define DATA_MAX    (PAGE_WORDS - HDR_WORDS - 1u)                /* group words, no name */
#define V2_MAX      ((DATA_MAX - NAME_WORDS) / 2u)               /* entries of a v2 record */
#define SPAN_MAX    0x4000u
#define W_INVALID   3u

typedef struct {
    uint32_t addr;
    uint32_t words;
    uint32_t seq;
    uint32_t hdr;                          /* slot | flags << 8 | revision << 16 */
    uint32_t count;                        /* v2: entries; v3: group words */
    uint32_t ent;                          /* address of the first entry / group */
    int v3;
} rec_t;

#define REC_SLOT(r) ((uint8_t)((r)->hdr & 0xFFu))

typedef void (*entry_cb)(const cs_entry_t *e, void *ctx);

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

/* ---- v3 groups -------------------------------------------------------- */

static uint32_t width_of(int32_t v)
{
    return v >= -128 && v <= 127 ? 0u : v >= -32768 && v <= 32767 ? 1u : 2u;
}

static int32_t unpack(uint32_t raw, uint32_t w)
{
    return w == 0u ? (int32_t)(int8_t)raw : w == 1u ? (int32_t)(int16_t)raw : (int32_t)raw;
}

/* Decode the groups at a (dwords words); cb (may be NULL) gets each entry.
 * Returns 0, or -1 if they are malformed (then cb may have seen some). */
static int decode(uint32_t a, uint32_t dwords, entry_cb cb, void *ctx)
{
    uint32_t end = a + 4u * dwords;
    while (a < end) {
        uint32_t h = flash_read(a);
        uint32_t w = (h >> 8) & 3u, span = ((h >> 10) & 0x3FFFu) + 1u;
        cs_entry_t e = { (uint16_t)(h & 0xFFu), 0, 0 };
        a += 4u;
        if (w == W_INVALID)
            return -1;
        if (span == 1u && w == 0u) {
            e.value = (int32_t)(int8_t)(h >> 24);
            if (cb)
                cb(&e, ctx);
            continue;
        }
        if (h >> 24)
            return -1;
        uint32_t bm = a, nb = span > 1u ? (span + 31u) / 32u : 0u, nv = 0;
        a += 4u * nb;
        if (a > end)
            return -1;
        if (nb && span % 32u && flash_read(bm + 4u * (nb - 1u)) >> (span % 32u))
            return -1;                     /* an index past the span */
        for (uint32_t k = 0; k < nb; k++)
            for (uint32_t bits = flash_read(bm + 4u * k); bits; bits &= bits - 1u)
                nv++;
        if (span == 1u)
            nv = 1;
        uint32_t per = 4u >> w, vals = a;
        a += 4u * ((nv + per - 1u) / per);
        if (a > end)
            return -1;
        for (uint32_t i = 0, k = 0; cb && k < nv; i++) {
            if (nb && !((flash_read(bm + 4u * (i / 32u)) >> (i % 32u)) & 1u))
                continue;
            uint32_t raw = flash_read(vals + 4u * (k / per)) >> ((k % per) * (8u << w));
            e.index = (uint16_t)i;
            e.value = unpack(raw, w);
            cb(&e, ctx);
            k++;
        }
    }
    return 0;
}

/* Where encode() sends its words: counted only (dry), or programmed. */
typedef struct {
    int dry;
    uint32_t a;
    uint32_t crc;
    uint32_t words;
    int err;
} out_t;

/* Program one word, read it back, fold it into the CRC. */
static void put(out_t *o, uint32_t w)
{
    o->words++;
    if (o->dry || o->err)
        return;
    if (flash_program_word(o->a, w) != 0 || flash_read(o->a) != w)
        o->err = 1;
    o->crc = crc32_word(o->crc, w);
    o->a += 4u;
}

/* The smallest index of `id` above `after` (-1: the first), and its value
 * (a repeated index: the last entry wins). -1 if there is none. */
static int32_t next_index(const cs_entry_t *e, uint32_t n, uint16_t id, int32_t after, int32_t *v)
{
    int32_t best = -1;
    for (uint32_t i = 0; i < n; i++)
        if (e[i].id == id && (int32_t)e[i].index > after && (best < 0 || (int32_t)e[i].index <= best)) {
            best = e[i].index;
            *v = e[i].value;
        }
    return best;
}

/* The entries as v3 groups, one per id in order of first appearance.
 * Returns -1 if an entry can't be stored (id > 255, index >= SPAN_MAX). */
static int encode(const cs_entry_t *e, uint32_t n, out_t *o)
{
    for (uint32_t i = 0; i < n; i++) {
        uint16_t id = e[i].id;
        uint32_t j = 0;
        while (j < i && e[j].id != id)
            j++;
        if (j < i)
            continue;                      /* already in an earlier group */
        uint32_t span = 0, w = 0;
        for (j = i; j < n; j++)
            if (e[j].id == id) {
                if (e[j].index + 1u > span)
                    span = e[j].index + 1u;
                if (width_of(e[j].value) > w)
                    w = width_of(e[j].value);
            }
        if (id > 0xFFu || span > SPAN_MAX)
            return -1;
        int32_t v = 0;
        if (span == 1u && w == 0u) {
            next_index(e, n, id, -1, &v);
            put(o, id | (uint32_t)(uint8_t)v << 24);
            continue;
        }
        put(o, id | w << 8 | (span - 1u) << 10);
        for (uint32_t k = 0; span > 1u && k < (span + 31u) / 32u; k++) {
            uint32_t bits = 0;
            for (j = i; j < n; j++)
                if (e[j].id == id && e[j].index / 32u == k)
                    bits |= 1u << (e[j].index % 32u);
            put(o, bits);
        }
        uint32_t bw = 8u << w, mask = w == 2u ? 0xFFFFFFFFu : (1u << bw) - 1u, acc = 0, sh = 0;
        for (int32_t x = next_index(e, n, id, -1, &v); x >= 0; x = next_index(e, n, id, x, &v)) {
            acc |= ((uint32_t)v & mask) << sh;
            sh += bw;
            if (sh == 32u) {
                put(o, acc);
                acc = sh = 0;
            }
        }
        if (sh)
            put(o, acc);
    }
    return 0;
}

/* ---- records ---------------------------------------------------------- */

/* A valid v2 or v3 record at a (inside its page)? */
static int rec_check(uint32_t a, rec_t *r)
{
    uint32_t end = page_of(a) + CS_PAGE;
    if (a + 4u * (HDR_WORDS + 1u) > end)
        return 0;
    uint32_t magic = flash_read(a);
    if (magic != MAGIC_V3 && magic != CONFIG_STORE_MAGIC_V2)
        return 0;
    uint32_t hdr = flash_read(a + 8u), count = flash_read(a + 12u);
    uint32_t named = (hdr >> 8) & CS_F_NAME, v3 = magic == MAGIC_V3;
    if (count > (v3 ? DATA_MAX : V2_MAX))
        return 0;
    uint32_t words = HDR_WORDS + (named ? NAME_WORDS : 0u) + (v3 ? count : 2u * count) + 1u;
    if (a + 4u * words > end)
        return 0;
    uint32_t crc = CRC32_INIT;
    for (uint32_t i = 0; i < words - 1u; i++)
        crc = crc32_word(crc, flash_read(a + 4u * i));
    if (~crc != flash_read(a + 4u * (words - 1u)))
        return 0;
    uint32_t ent = a + 4u * (HDR_WORDS + (named ? NAME_WORDS : 0u));
    if (v3 && decode(ent, count, 0, 0) != 0)
        return 0;
    r->addr = a;
    r->words = words;
    r->seq = flash_read(a + 4u);
    r->hdr = hdr;
    r->count = count;
    r->ent = ent;
    r->v3 = (int)v3;
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

typedef struct {
    uint32_t hdr;
    uint32_t words;                        /* the whole record */
    uint32_t dwords;                       /* its group words */
    const uint32_t *name;                  /* NAME_WORDS words, or NULL */
    const cs_entry_t *e;
    uint32_t n;
} new_rec_t;

static int emit(uint32_t a, uint32_t seq, const new_rec_t *nr)
{
    out_t o = { 0, a, CRC32_INIT, 0, 0 };
    put(&o, MAGIC_V3);
    put(&o, seq);
    put(&o, nr->hdr);
    put(&o, nr->dwords);
    for (uint32_t i = 0; nr->name && i < NAME_WORDS; i++)
        put(&o, nr->name[i]);
    encode(nr->e, nr->n, &o);
    if (o.err || flash_program_word(o.a, ~o.crc) != 0)
        return -1;
    rec_t check;
    return rec_check(a, &check) ? 0 : -1;
}

typedef struct {
    uint8_t skip;                          /* the slot being written */
    int dry;                               /* only add up the words */
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
    for (uint32_t i = 0; i < r->words; i++) {
        uint32_t w = flash_read(r->addr + 4u * i);
        if (!c->dry && (flash_program_word(c->to, w) != 0 || flash_read(c->to) != w)) {
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
            return emit(a, seq, nr) ? GSX_FLASH_ERR : GSX_OK;
    }
    uint32_t to = other_page(act);
    copy_t c = { slot, 1, 0, 0 };
    walk(act, copy_cb, &c);                /* the other slots' words */
    if (c.to + 4u * nr->words > CS_PAGE)
        return GSX_BAD_LEN;                /* the builder's budget rules this out */
    if (flash_erase_page(to) != 0)
        return GSX_FLASH_ERR;
    c = (copy_t){ slot, 0, to, 0 };
    walk(act, copy_cb, &c);
    if (c.err || c.to + 4u * nr->words > to + CS_PAGE)
        return GSX_FLASH_ERR;
    return emit(c.to, seq, nr) ? GSX_FLASH_ERR : GSX_OK;
}

static int locked_append(uint8_t slot, const new_rec_t *nr)
{
    flash_unlock();
    int st = append(slot, nr);
    flash_lock();
    return st;
}

int config_store_write(uint8_t slot, uint16_t revision, const char *name,
                       const cs_entry_t *e, uint32_t n)
{
    uint32_t nw[NAME_WORDS] = { 0 };
    new_rec_t nr = { (uint32_t)slot | (uint32_t)revision << 16, 0, 0, 0, e, n };
    if (name && name[0]) {
        for (uint32_t i = 0; i < CS_NAME_LEN && name[i]; i++)
            nw[i / 4u] |= (uint32_t)(uint8_t)name[i] << (8u * (i % 4u));
        nr.hdr |= CS_F_NAME << 8;
        nr.name = nw;
    }
    out_t o = { 1, 0, 0, 0, 0 };
    if (n > 4u * DATA_MAX || encode(e, n, &o) != 0 || o.words > DATA_MAX - (nr.name ? NAME_WORDS : 0u))
        return GSX_BAD_LEN;
    nr.dwords = o.words;
    nr.words = HDR_WORDS + (nr.name ? NAME_WORDS : 0u) + nr.dwords + 1u;
    return locked_append(slot, &nr);
}

static int tombstone(const rec_t *r)
{
    return (r->hdr >> 8) & CS_F_TOMBSTONE;
}

int config_store_read(uint8_t slot, cs_slot_info_t *info, entry_cb cb, void *ctx)
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
    if (r.v3) {
        if (cb)
            decode(r.ent, r.count, cb, ctx);   /* checked by rec_check */
        return 1;
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
    new_rec_t nr = { (uint32_t)slot | CS_F_TOMBSTONE << 8, HDR_WORDS + 1u, 0, 0, 0, 0 };
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

/* A lower bound: the entries a named record holds whatever their values,
 * with indices below 32 (each its own group: header, bitmap, int32). A
 * record packs far more when ids repeat; the builder computes the real
 * worst case from the params' counts and ranges. */
uint32_t config_store_max_entries(void)
{
    return (DATA_MAX - NAME_WORDS) / 3u;
}

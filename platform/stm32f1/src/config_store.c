/*
 * config_store.c: per-slot settings records (core/include/config_store.h) in
 * main flash: BOARD_CONFIG_PAGES pages of BOARD_FLASH_PAGE bytes from
 * BOARD_CONFIG_BASE, inside the vendor "EEPROM" area, outside the
 * checksummed app range (Code 61: the three pages the stock app never
 * uses, 0x0801EC00-0x0801F7FF). Words are programmed as two halfwords (the
 * F1 flash programs 16 bits at a time); a torn word fails the record CRC.
 *
 * A log over the pages, with no page order: a record goes wherever it fits,
 * and the newest record of a slot (highest seq) wins wherever it is. A copy
 * of a record (same slot, same seq, same words) only ties with it.
 *
 * "Only-live": a slot's newest record when no other page holds a copy of
 * it. A page with no only-live record is a spare: erasing it loses nothing.
 * The rules, checked at every write against what is in flash (ix, as
 * scan() found it):
 *
 *   1. Append: a record goes after the last programmed word of a page with
 *      room for it, but only while another page is a spare. That holds
 *      before the write and so also after a write that a power cut tears.
 *   2. Compact, when no page can take it: pick a spare, and the page with
 *      the fewest only-live words besides the slot's own (the victim; at
 *      most 1/(pages - 1) of them). Erase the spare (skipped if blank),
 *      copy the victim's only-live records into it unchanged (same seq),
 *      then write the new record last. The victim is then a spare too,
 *      erased only when a later compaction needs it.
 *
 * Nothing is erased while it holds the only copy of a slot's newest record,
 * so a power cut anywhere (an erase, a copy, the new record) loses at most
 * the write in progress, and the next write starts over from what is in
 * flash. A torn record or a half-erased page is just unused words: the scan
 * resyncs on the next magic + CRC, and appends go after the last programmed
 * word. A write that can't fit fails with GSX_BAD_LEN before anything is
 * erased or written.
 *
 * Room: compaction always works while, with M the largest record of any
 * slot and L all slots' records together, (L - M) / (pages - 1) + M fits a
 * page, i.e. L + (pages - 2) * M <= (pages - 1) * page. With two pages that
 * is every slot in one page (the former swap scheme); each more page adds
 * one page less one record. The builder checks it at worst case
 * (tools/builder/gemifi_device.py STORE_BUDGETS, stm32f1, which computes
 * the packed sizes below). Erases: one page per compaction, which comes
 * once the free space of every non-spare page is used up, so far less often
 * than with two pages.
 *
 * Old layouts: the two-page swap of earlier firmware (the active page holds
 * every slot's newest record, the other only older copies) is a valid state
 * of this log, so it is read as it is, with no migration step.
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
 * as they are by a compaction; the next write of a slot replaces it with
 * v3. v1 ('GCFG') records are not read: no Code 61 has stored any.
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
#define CS_PAGES  BOARD_CONFIG_PAGES
#define CS_SIZE   (CS_PAGES * CS_PAGE)
#define CS_SLOTS  65u                      /* slot 0 + presets 1..64 (gemifi_device.py MAX_PRESETS) */

/* Stay inside the vendor EEPROM area: never the checksummed app range
 * (below 0x0801B400), the vendor's preset-names page (0x0801F800) nor the
 * update-request flag page (0x0801FC00). */
_Static_assert(CS_BASE >= 0x0801B400u && CS_BASE + CS_SIZE <= 0x0801F800u,
               "config pages must lie in 0x0801B400-0x0801F7FF");
_Static_assert(CS_BASE % CS_PAGE == 0, "config base must be page aligned");
_Static_assert(CS_PAGES >= 2u && CS_PAGES <= 8u, "2 to 8 config pages");
_Static_assert(CS_SIZE <= 0x10000u, "the index keeps 16-bit offsets");

#define MAGIC_V3    0x33464347u            /* 'GCF3' */
#define HDR_WORDS   4u                     /* magic, seq, slot | flags | revision, count */
#define NAME_WORDS  (CS_NAME_LEN / 4u)
#define PAGE_WORDS  (CS_PAGE / 4u)
#define DATA_MAX    (PAGE_WORDS - HDR_WORDS - 1u)                /* group words, no name */
#define V2_MAX      ((DATA_MAX - NAME_WORDS) / 2u)               /* entries of a v2 record */
#define SPAN_MAX    0x4000u
#define W_INVALID   3u
#define NO_SLOT     0xFFu
#define NO_PAGE     0xFFu

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

static uint32_t page_addr(uint32_t p)
{
    return CS_BASE + p * CS_PAGE;
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

/* The words of the v2 or v3 record at a, from its header. */
static uint32_t rec_len(uint32_t a)
{
    uint32_t named = (flash_read(a + 8u) >> 8) & CS_F_NAME, count = flash_read(a + 12u);
    return HDR_WORDS + (named ? NAME_WORDS : 0u) + (flash_read(a) == MAGIC_V3 ? count : 2u * count) + 1u;
}

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
    uint32_t words = rec_len(a);
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

/* What scan() found: the newest record of every slot, and how full each
 * page is. RAM: 3 bytes a slot, about 0.2 KB: a slot's seq and length are
 * read from its record in flash (the record ix points at is one rec_check()
 * accepted; a compaction re-points ix at the copies it keeps before it
 * erases the spare). Every slot a gemifi firmware can write (CS_SLOTS: up
 * to 64 presets) is indexed, whatever this firmware's preset count: a
 * compaction copies only indexed records, so a smaller index would drop
 * the presets of a firmware with more of them (flashed back later).
 *
 * Kept between calls while `ok` (zero at boot: .bss), so a read costs one
 * rec_check() of the indexed record, not a walk of every page. Only this
 * file writes the config pages while the app runs (the bootloader and
 * recovery.c run across a reset, and recovery.c only writes the update
 * flag page), so the index stays true until one of ours changes flash:
 * store() and config_store_erase() clear `ok` before their first erase or
 * program. An append that completes updates the index itself (note() of the
 * new record); a compaction, an error or a power cut leaves it cleared, and
 * the next call scans again. A power cut is a reset anyway: the boot-time
 * scan sees whatever a torn write left. newest() also rescans once if the
 * indexed record no longer checks out (defensive: never expected). */
typedef struct {
    uint16_t off[CS_SLOTS];                /* one copy of it: its byte offset from CS_BASE */
    uint8_t pages[CS_SLOTS];               /* bit p: page p holds a copy; 0 = no record */
    uint16_t used[CS_PAGES];               /* words up to the last programmed one */
    uint32_t top;                          /* the newest seq of all */
    uint8_t top_page;
    uint8_t any;
    uint8_t ok;                            /* ix matches flash */
} index_t;

static index_t ix;

/* Slot s's newest record (one copy of it), its seq and its words; only
 * while ix.pages[s]. */
static uint32_t ix_addr(uint32_t s)
{
    return CS_BASE + ix.off[s];
}

static uint32_t ix_seq(uint32_t s)
{
    return flash_read(ix_addr(s) + 4u);
}

static uint32_t ix_words(uint32_t s)
{
    return rec_len(ix_addr(s));
}

static void note(const rec_t *r, uint32_t p)
{
    uint8_t s = REC_SLOT(r);
    if (!ix.any || seq_newer(r->seq, ix.top)) {
        ix.top = r->seq;
        ix.top_page = (uint8_t)p;
        ix.any = 1;
    }
    if (s >= CS_SLOTS)
        return;                            /* not a slot of ours: never copied */
    if (!ix.pages[s] || seq_newer(r->seq, ix_seq(s))) {
        ix.off[s] = (uint16_t)(r->addr - CS_BASE);
        ix.pages[s] = (uint8_t)(1u << p);
    } else if (r->seq == ix_seq(s)) {
        ix.pages[s] |= (uint8_t)(1u << p);
    }
}

/* Walk page p: records are found by magic + CRC wherever they start (a
 * torn record or old junk in between is skipped a word at a time). fn (may
 * be NULL) gets each; it returns nonzero to stop there. Returns the address
 * after the last programmed word (or of the record fn stopped at). */
static uint32_t walk(uint32_t p, int (*fn)(const rec_t *r, uint32_t p, void *ctx), void *ctx)
{
    uint32_t pg = page_addr(p), a = pg, last = pg;
    rec_t r;
    while (a < pg + CS_PAGE) {
        if (rec_check(a, &r)) {
            if (fn && fn(&r, p, ctx))
                return a;
            a += 4u * r.words;
            last = a;
            continue;
        }
        if (flash_read(a) != 0xFFFFFFFFu)
            last = a + 4u;
        a += 4u;
    }
    return last;
}

static int note_cb(const rec_t *r, uint32_t p, void *ctx)
{
    (void)ctx;
    note(r, p);
    return 0;
}

#ifdef CS_TEST
unsigned cs_test_scans;                    /* scans so far (cs-test) */
#endif

/* Read every page into ix. */
static void scan(void)
{
#ifdef CS_TEST
    cs_test_scans++;
#endif
    for (uint32_t s = 0; s < CS_SLOTS; s++)
        ix.pages[s] = 0;
    ix.any = 0;
    ix.top_page = 0;
    for (uint32_t p = 0; p < CS_PAGES; p++)
        ix.used[p] = (uint16_t)((walk(p, note_cb, 0) - page_addr(p)) / 4u);
    ix.ok = 1;
}

/* ix, scanned only if it may not match flash. */
static void index_flash(void)
{
    if (!ix.ok)
        scan();
}

static int is_newest_cb(const rec_t *r, uint32_t p, void *ctx)
{
    (void)p;
    uint32_t s = *(const uint32_t *)ctx;
    return REC_SLOT(r) == s && r->seq == ix_seq(s);
}

/* The address of slot s's newest record on page p (ix.off may be a copy
 * on another page). */
static uint32_t locate(uint32_t p, uint32_t s)
{
    return walk(p, is_newest_cb, &s);
}

static uint32_t room(uint32_t p)
{
    return PAGE_WORDS - ix.used[p];
}

/* Is page p the only one of the pages in `keep` with slot s's newest record? */
static int only_on(uint32_t s, uint32_t p, uint8_t keep)
{
    return (ix.pages[s] & keep) == (uint8_t)(1u << p);
}

/* Words of page p's only-live records, leaving out slot `skip` and counting
 * copies on page `gone` (about to be erased) as not there. */
static uint32_t only_live(uint32_t p, uint32_t skip, uint32_t gone)
{
    uint32_t n = 0;
    uint8_t keep = (uint8_t)~(gone < CS_PAGES ? 1u << gone : 0u);
    for (uint32_t s = 0; s < CS_SLOTS; s++)
        if (s != skip && only_on(s, p, keep))
            n += ix_words(s);
    return n;
}

/* A spare (a page with no only-live record) other than page `not`, or NO_PAGE.
 * A blank one first: it needs no erase. */
static uint32_t spare(uint32_t not)
{
    uint32_t best = NO_PAGE;
    for (uint32_t p = 0; p < CS_PAGES; p++)
        if (p != not && only_live(p, NO_SLOT, NO_PAGE) == 0 && (best == NO_PAGE || !ix.used[p]))
            best = p;
    return best;
}

typedef struct {
    uint32_t hdr;
    uint32_t words;                        /* the whole record */
    uint32_t dwords;                       /* its group words */
    const uint32_t *name;                  /* NAME_WORDS words, or NULL */
    const cs_entry_t *e;
    uint32_t n;
} new_rec_t;

/* Program the record at a; *check (the record as read back) on success. */
static int emit(uint32_t a, uint32_t seq, const new_rec_t *nr, rec_t *check)
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
    return rec_check(a, check) ? 0 : -1;
}

/* Copy `words` words from `from` to `to`, read back. */
static int copy_words(uint32_t to, uint32_t from, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = flash_read(from + 4u * i);
        if (flash_program_word(to + 4u * i, w) != 0 || flash_read(to + 4u * i) != w)
            return -1;
    }
    return 0;
}

static int store(uint8_t slot, const new_rec_t *nr)
{
    index_flash();
    uint32_t seq = ix.any ? ix.top + 1u : 1u, n = nr->words;
    rec_t r;

    /* 1. append: the page of the newest record first, then the fullest */
    uint32_t t = NO_PAGE;
    for (uint32_t p = 0; p < CS_PAGES; p++) {
        if (room(p) < n || spare(p) == NO_PAGE)
            continue;
        if (t == NO_PAGE || (t != ix.top_page && (p == ix.top_page || ix.used[p] > ix.used[t])))
            t = p;
    }
    if (t != NO_PAGE) {
        ix.ok = 0;                         /* until the record is in */
        if (emit(page_addr(t) + 4u * ix.used[t], seq, nr, &r) != 0)
            return GSX_FLASH_ERR;
        note(&r, t);                       /* what a scan would find now */
        ix.used[t] = (uint16_t)(ix.used[t] + r.words);
        ix.ok = 1;
        return GSX_OK;
    }

    /* 2. compact the victim into a spare, the new record last */
    uint32_t sp = spare(NO_PAGE), v = NO_PAGE, vw = 0;
    if (sp == NO_PAGE)
        return GSX_BAD_LEN;                /* only after writes the budget rules out */
    for (uint32_t p = 0; p < CS_PAGES; p++) {
        uint32_t w = only_live(p, slot, sp);
        if (p != sp && (v == NO_PAGE || w < vw)) {
            v = p;
            vw = w;
        }
    }
    if (vw + n > PAGE_WORDS)
        return GSX_BAD_LEN;                /* the builder's budget rules this out */
    uint32_t to = page_addr(sp);
    uint8_t keep = (uint8_t)~(1u << sp);
    ix.ok = 0;                             /* the next call scans the compacted pages */
    for (uint32_t s = 0; s < CS_SLOTS; s++)
        if (s != slot && only_on(s, v, keep))
            ix.off[s] = (uint16_t)(locate(v, s) - CS_BASE);   /* its copy on v, not on sp */
    if (ix.used[sp] && flash_erase_page(to) != 0)
        return GSX_FLASH_ERR;
    for (uint32_t s = 0; s < CS_SLOTS; s++) {
        if (s == slot || !only_on(s, v, keep))
            continue;
        uint32_t w = ix_words(s);
        if (copy_words(to, ix_addr(s), w) != 0)
            return GSX_FLASH_ERR;
        to += 4u * w;
    }
    return emit(to, seq, nr, &r) ? GSX_FLASH_ERR : GSX_OK;
}

static int locked_store(uint8_t slot, const new_rec_t *nr)
{
    if (slot >= CS_SLOTS)
        return GSX_BAD_LEN;
    flash_unlock();
    int st = store(slot, nr);
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
    return locked_store(slot, &nr);
}

/* The newest record of slot, if it has one (a tombstone counts). */
static int newest(uint8_t slot, rec_t *r)
{
    if (slot >= CS_SLOTS)
        return 0;
    for (int pass = 0;; pass++) {
        int fresh = !ix.ok;
        index_flash();
        if (!ix.pages[slot])
            return 0;
        if (rec_check(ix_addr(slot), r) && REC_SLOT(r) == slot)
            return 1;
        if (fresh || pass)
            return 0;                      /* as scanned: nothing better in flash */
        ix.ok = 0;                         /* flash changed behind ix: scan again */
    }
}

static int tombstone(const rec_t *r)
{
    return (r->hdr >> 8) & CS_F_TOMBSTONE;
}

int config_store_read(uint8_t slot, cs_slot_info_t *info, entry_cb cb, void *ctx)
{
    rec_t r;
    if (!newest(slot, &r) || tombstone(&r))
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
    if (!newest(slot, &r) || tombstone(&r))
        return GSX_OK;                     /* nothing stored: nothing to hide */
    new_rec_t nr = { (uint32_t)slot | CS_F_TOMBSTONE << 8, HDR_WORDS + 1u, 0, 0, 0, 0 };
    return locked_store(slot, &nr);
}

int config_store_erase(void)
{
    int st = GSX_OK;
    ix.ok = 0;
    flash_unlock();
    for (uint32_t p = 0; p < CS_PAGES; p++)
        if (flash_erase_page(page_addr(p)) != 0)
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

#ifdef CS_TEST
/* cs-test: flash was changed behind the store's back (as across a reset). */
void cs_test_reboot(void)
{
    ix.ok = 0;
}

/* cs-test: 1 if the kept index is what a scan of flash finds now, 0 if it
 * differs, -1 if it is not kept (the next call scans). Leaves ix as scanned. */
int cs_test_index_matches(void)
{
    if (!ix.ok)
        return -1;
    index_t kept = ix;
    scan();
    if (kept.any != ix.any || (ix.any && (kept.top != ix.top || kept.top_page != ix.top_page)))
        return 0;
    for (uint32_t p = 0; p < CS_PAGES; p++)
        if (kept.used[p] != ix.used[p])
            return 0;
    for (uint32_t s = 0; s < CS_SLOTS; s++)
        if (kept.pages[s] != ix.pages[s] ||
            (ix.pages[s] && kept.off[s] != ix.off[s]))
            return 0;
    return 1;
}
#endif

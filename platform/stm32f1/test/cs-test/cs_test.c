/*
 * cs_test.c: platform/stm32f1/src/config_store.c on simulated flash.
 *
 * The simulated flash acts like the F1's: erased bytes are 0xFF, a halfword
 * can be programmed only while it is erased (or to 0), and a page erase
 * clears 1 KB. A power cut after the Nth halfword program stops the write
 * there (longjmp), as a reset would.
 *
 * A power cut in an erase leaves the first half of the page erased and the
 * rest as it was.
 *
 * Covers the packed v3 groups (widths, gaps, limits), the Code 61's worst
 * case (5 presets + the globals on 3 pages), the old two-page layout, old v2
 * records, CRC-valid records with bad groups, junk pages, power cuts through
 * compactions (also a second cut while recovering), how often a typical
 * save erases a page, the index the store keeps between calls (reads
 * don't scan; every write keeps it true or drops it), and the presets of
 * a firmware with more of them, kept through compactions.
 *
 * SPDX-License-Identifier: MIT
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "flash.h"
#include "gsx.h"
#include "crc32.h"

uint8_t sim_flash[BOARD_CONFIG_PAGES * BOARD_FLASH_PAGE];

/* config_store.c's test hooks (-DCS_TEST) */
extern unsigned cs_test_scans;
void cs_test_reboot(void);
int cs_test_index_matches(void);

static long cut_after = -1;               /* halfword programs + erases left; -1 = no cut */
static long sim_erases;                    /* page erases so far */
static jmp_buf cut_jb;
static int fails;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            fails++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
        }                                                      \
    } while (0)

void flash_unlock(void) {}
void flash_lock(void) {}

static int out_of_range(uint32_t addr, uint32_t len)
{
    if (addr >= BOARD_CONFIG_BASE && addr + len <= BOARD_CONFIG_BASE + sizeof(sim_flash))
        return 0;
    fails++;
    printf("FAIL: flash access outside the config pages at 0x%08X\n", addr);
    return 1;
}

int flash_erase_page(uint32_t addr)
{
    if (out_of_range(addr, BOARD_FLASH_PAGE) || addr % BOARD_FLASH_PAGE)
        return -1;
    if (cut_after == 0) {
        memset(sim_flash + (addr - BOARD_CONFIG_BASE), 0xFF, BOARD_FLASH_PAGE / 2u);
        longjmp(cut_jb, 1);
    }
    if (cut_after > 0)
        cut_after--;
    sim_erases++;
    memset(sim_flash + (addr - BOARD_CONFIG_BASE), 0xFF, BOARD_FLASH_PAGE);
    return 0;
}

int flash_program_half(uint32_t addr, uint16_t v)
{
    if (out_of_range(addr, 2))
        return -1;
    if (cut_after == 0)
        longjmp(cut_jb, 1);
    if (cut_after > 0)
        cut_after--;
    uint8_t *p = sim_flash + (addr - BOARD_CONFIG_BASE);
    uint16_t cur = (uint16_t)(p[0] | p[1] << 8);
    if (cur != 0xFFFFu && v != 0)
        return -1;                         /* PGERR */
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return 0;
}

int flash_program_word(uint32_t addr, uint32_t w)
{
    if (flash_program_half(addr, (uint16_t)w) != 0)
        return -1;
    return flash_program_half(addr + 2u, (uint16_t)(w >> 16));
}

/* ---- helpers ---- */

#define MAX_E 400

typedef struct {
    int n;
    cs_entry_t e[MAX_E];
} got_t;

static void collect(const cs_entry_t *e, void *ctx)
{
    got_t *g = ctx;
    if (g->n < MAX_E)
        g->e[g->n] = *e;
    g->n++;
}

/* Slot's stored value of entry 0, or -1 when nothing is stored. */
static int32_t value_of(uint8_t slot)
{
    got_t g = { 0 };
    if (!config_store_read(slot, 0, collect, &g))
        return -1;
    return g.n > 0 ? g.e[0].value : -2;
}

/* A record of n entries, every value v: params 10, 11, ... of 32 values each. */
static int write_n(uint8_t slot, int32_t v, uint32_t n, const char *name)
{
    cs_entry_t e[MAX_E];
    for (uint32_t i = 0; i < n; i++)
        e[i] = (cs_entry_t){ (uint16_t)(10 + i / 32u), (uint16_t)(i % 32u), v };
    return config_store_write(slot, 7, name, e, n);
}

/* The Code 61's worst case (devices/m-audio-code61.yaml, core/src/settings.c):
 * every per-preset value of a preset (with preset.program: 153), or every
 * global (12), each at a value of its full range. v varies them. */
static uint32_t code61_preset(cs_entry_t *e, int32_t v)
{
    static const struct { uint16_t id, count; int32_t lo, hi; } p[] = {
        { 1, 1, 1, 16 }, { 2, 1, -24, 24 }, { 3, 1, -4, 4 }, { 4, 1, 0, 3 }, { 5, 1, 1, 127 },
        { 8, 14, 0, 127 }, { 9, 41, 0, 15 }, { 10, 41, 0, 127 }, { 11, 1, 0, 2000 },
        { 12, 1, 0, 5000 }, { 15, 8, 0, 127 }, { 16, 8, 0, 2 }, { 18, 1, -1, 127 },
        { 20, 16, 0, 127 }, { 21, 1, 0, 16 }, { 22, 1, 0, 3 }, { 23, 1, 0, 1 }, { 29, 14, 0, 2 },
    };
    uint32_t n = 0;
    for (uint32_t k = 0; k < sizeof(p) / sizeof(p[0]); k++)
        for (uint32_t i = 0; i < p[k].count; i++) {
            int32_t span = p[k].hi - p[k].lo + 1;
            int32_t x = p[k].lo + (int32_t)((v + 7 * (int32_t)i) % span);
            if (p[k].id == 11 || p[k].id == 12)
                x = p[k].hi;               /* bend.glide, ramp.time: int16 */
            e[n++] = (cs_entry_t){ p[k].id, (uint16_t)i, x };
        }
    e[0].value = 1 + v % 16;               /* midi.channel: entry 0 tells the writes apart */
    return n;
}

static uint32_t code61_globals(cs_entry_t *e, int32_t v)
{
    const cs_entry_t g[] = {
        { 6, 0, 500 }, { 7, 0, 2000 }, { 13, 0, 1 }, { 14, 0, 4 }, { 17, 0, 3 },
        { 24, 0, 4095 }, { 25, 0, 4094 }, { 26, 0, 4095 }, { 27, 0, 50 }, { 28, 0, 500 },
        { 30, 0, 1 }, { 31, 0, 1 },
    };
    uint32_t n = sizeof(g) / sizeof(g[0]);
    for (uint32_t i = 0; i < n; i++)
        e[i] = g[i];
    e[0].value = 300 + v;                  /* velocity.fast: int16, tells the writes apart */
    return n;
}

static int write_code61(uint8_t slot, int32_t v)
{
    cs_entry_t e[MAX_E];
    uint32_t n = slot ? code61_preset(e, v) : code61_globals(e, v);
    return config_store_write(slot, 3, slot ? "Preset name here" : 0, e, n);
}

/* Does slot read back exactly what write_code61(slot, v) wrote? */
static int is_code61(uint8_t slot, int32_t v)
{
    cs_entry_t want[MAX_E];
    uint32_t n = slot ? code61_preset(want, v) : code61_globals(want, v);
    got_t g = { 0 };
    if (!config_store_read(slot, 0, collect, &g) || g.n != (int)n)
        return 0;
    for (uint32_t i = 0; i < n; i++)       /* written in id, index order: read back the same */
        if (g.e[i].id != want[i].id || g.e[i].index != want[i].index || g.e[i].value != want[i].value)
            return 0;
    return 1;
}

static uint32_t rd(uint32_t off)
{
    return flash_read(BOARD_CONFIG_BASE + off);
}

/* Program a word behind the store's back (then it scans, as after a reset). */
static void wr(uint32_t *off, uint32_t w, uint32_t *crc)
{
    cs_test_reboot();
    flash_program_word(BOARD_CONFIG_BASE + *off, w);
    if (crc)
        *crc = crc32_word(*crc, w);
    *off += 4u;
}

/* Blank pages, as on a new device (the store scans them, as after a reset). */
static void fresh(void)
{
    memset(sim_flash, 0xFF, sizeof(sim_flash));
    cs_test_reboot();
}

/* ---- tests ---- */

static void test_basics(void)
{
    fresh();
    CHECK(config_store_read(0, 0, 0, 0) == 0, "empty store has no slot 0");
    CHECK(write_n(0, 5, 20, 0) == GSX_OK, "write globals");
    CHECK(write_n(1, 9, 30, "Drums") == GSX_OK, "write preset 1");
    cs_slot_info_t info;
    got_t g = { 0 };
    CHECK(config_store_read(1, &info, collect, &g) == 1, "read preset 1");
    CHECK(g.n == 30 && g.e[29].id == 10 && g.e[29].index == 29 && g.e[29].value == 9, "entries back");
    CHECK(info.revision == 7 && (info.flags & CS_F_NAME) && strcmp(info.name, "Drums") == 0,
          "revision 7, name 'Drums' (got %u, '%s')", info.revision, info.name);
    CHECK(config_store_read(0, &info, 0, 0) == 1 && info.name[0] == 0, "globals have no name");
    CHECK(value_of(0) == 5, "globals still there");
    CHECK(value_of(2) == -1, "slot 2 never written");

    CHECK(config_store_erase_slot(1) == GSX_OK && value_of(1) == -1, "tombstone hides preset 1");
    CHECK(value_of(0) == 5, "globals untouched by the tombstone");
    CHECK(config_store_erase() == GSX_OK && value_of(0) == -1, "erase clears everything");
    CHECK(write_n(0, 1, 0, 0) == GSX_OK && value_of(0) == -2, "an empty record");
    CHECK(config_store_max_entries() == 82, "max entries 82 (got %u)", config_store_max_entries());
}

/* The packed groups: widths, signs, gaps, repeats and order. */
static void test_packing(void)
{
    fresh();
    const cs_entry_t in[] = {
        { 3, 5, -1 }, { 7, 0, 70000 }, { 3, 0, 127 }, { 3, 40, -128 }, { 9, 0, -5 },
        { 3, 5, 12 },                      /* index 5 again: the last one wins */
        { 4, 0, 300 }, { 5, 2, -32768 }, { 5, 1, 32767 }, { 7, 1, -2147483647 - 1 },
        { 0, 0, 1 }, { 255, 200, 2 },
    };
    const cs_entry_t out[] = {             /* by id in first-seen order, then by index */
        { 3, 0, 127 }, { 3, 5, 12 }, { 3, 40, -128 }, { 7, 0, 70000 }, { 7, 1, -2147483647 - 1 },
        { 9, 0, -5 }, { 4, 0, 300 }, { 5, 1, 32767 }, { 5, 2, -32768 }, { 0, 0, 1 }, { 255, 200, 2 },
    };
    uint32_t n = sizeof(in) / sizeof(in[0]), m = sizeof(out) / sizeof(out[0]);
    CHECK(config_store_write(4, 1, 0, in, n) == GSX_OK, "write mixed entries");
    got_t g = { 0 };
    CHECK(config_store_read(4, 0, collect, &g) == 1 && g.n == (int)m, "%d entries back (want %u)", g.n, m);
    for (uint32_t i = 0; i < m && i < (uint32_t)g.n; i++)
        CHECK(g.e[i].id == out[i].id && g.e[i].index == out[i].index && g.e[i].value == out[i].value,
              "entry %u: got %u[%u] = %d", i, g.e[i].id, g.e[i].index, g.e[i].value);
    /* group words: id 3: header, 2 bitmap words, 3 int8 in 1 word (4); id 7: header, bitmap, 2 int32
     * (4); id 9: inline (1); id 4: header, int16 (2); id 5: header, bitmap, 2 int16 (3); id 0: inline
     * (1); id 255 index 200: header, 7 bitmap words, 1 int8 (9) = 24 */
    CHECK(rd(12) == 24, "24 group words (got %u)", rd(12));

    cs_entry_t big = { 256, 0, 0 };
    CHECK(config_store_write(4, 1, 0, &big, 1) == GSX_BAD_LEN, "id 256 can't be stored");
    big = (cs_entry_t){ 1, 0x4000, 0 };
    CHECK(config_store_write(4, 1, 0, &big, 1) == GSX_BAD_LEN, "index 0x4000 can't be stored");
    cs_entry_t e[MAX_E];
    for (uint32_t i = 0; i < 248; i++)
        e[i] = (cs_entry_t){ 1, (uint16_t)i, 100000 };
    /* one group of k int32: 1 + ceil(k / 32) + k words; 251 fit a page without a name, 247 with */
    CHECK(config_store_write(4, 1, 0, e, 242) == GSX_OK, "242 int32 values fit one record");
    CHECK(config_store_write(4, 1, 0, e, 243) == GSX_BAD_LEN, "243 don't");
    CHECK(config_store_write(4, 1, "x", e, 238) == GSX_OK, "238 fit a named record");
    CHECK(config_store_write(4, 1, "x", e, 239) == GSX_BAD_LEN, "239 with a name don't");
    for (uint32_t i = 0; i < MAX_E; i++)
        e[i] = (cs_entry_t){ (uint16_t)(1 + i / 128u), (uint16_t)(i % 128u), (int32_t)(i % 100u) };
    CHECK(config_store_write(4, 1, "x", e, 384) == GSX_OK, "384 int8 values in 3 groups fit");
    g.n = 0;
    CHECK(config_store_read(4, 0, collect, &g) == 1 && g.n == 384 && g.e[383].value == 83, "384 back");
}

/* Many rewrites: every compaction must keep the other slots. */
static void test_compaction(void)
{
    fresh();
    CHECK(write_n(1, 100, 40, "Keys") == GSX_OK, "preset 1");
    CHECK(write_n(3, 300, 10, 0) == GSX_OK, "preset 3");
    CHECK(write_n(2, 200, 5, 0) == GSX_OK && config_store_erase_slot(2) == GSX_OK, "preset 2, then reset");
    sim_erases = 0;
    for (int i = 0; i < 500; i++) {
        if (write_n(0, i, 40, 0) != GSX_OK) {
            CHECK(0, "rewrite %d failed", i);
            break;
        }
        if (cs_test_index_matches() == 0) {
            CHECK(0, "rewrite %d: the kept index is wrong", i);
            break;
        }
        if (value_of(0) != i || value_of(1) != 100 || value_of(3) != 300 || value_of(2) != -1) {
            CHECK(0, "after rewrite %d: %d %d %d %d", i, value_of(0), value_of(1), value_of(2), value_of(3));
            break;
        }
    }
    cs_slot_info_t info;
    CHECK(config_store_read(1, &info, 0, 0) == 1 && strcmp(info.name, "Keys") == 0, "name kept over compactions");
    /* 500 x 29 words; the two-page swap erased a page every 7 (the free space of one page) */
    CHECK(sim_erases > 0 && sim_erases <= 500 / 8, "%ld erases for 500 rewrites", sim_erases);
}

/* The Code 61's worst case: 153 per-preset values in 304 B, the 12 globals
 * in 92 B. 5 presets + the globals (1612 B) keep every rewrite working on
 * the 3 pages (the builder's rule: 1612 + 304 <= 2 x 1024); more slots work
 * until a write can't fit, which fails with GSX_BAD_LEN before anything is
 * erased or written. */
static void test_code61(void)
{
    fresh();
    CHECK(write_code61(0, 1) == GSX_OK && rd(12) == 18 && rd(4 * 23) == 0xFFFFFFFFu,
          "globals: 18 group words, 23 in all (got %u)", rd(12));
    CHECK(write_code61(1, 1) == GSX_OK && rd(92 + 12) == 67 && rd(92 + 4 * 76) == 0xFFFFFFFFu,
          "preset: 67 group words, 76 in all (got %u)", rd(92 + 12));
    int32_t cur[6] = { 1, 1, 2, 3, 4, 5 };
    for (uint8_t s = 2; s <= 5; s++)
        CHECK(write_code61(s, cur[s]) == GSX_OK, "preset %u", s);
    sim_erases = 0;
    uint32_t rnd = 1;
    for (int round = 0; round < 600; round++) {
        rnd = rnd * 1103515245u + 12345u;
        uint8_t slot = (uint8_t)((rnd >> 16) % 6u);
        if (write_code61(slot, 10 + round) != GSX_OK) {
            CHECK(0, "rewrite %d of slot %u failed", round, slot);
            break;
        }
        cur[slot] = 10 + round;
        if (cs_test_index_matches() == 0) {
            CHECK(0, "rewrite %d of slot %u: the kept index is wrong", round, slot);
            break;
        }
        int ok = 1;
        for (uint8_t s = 0; s < 6; s++)
            ok &= is_code61(s, cur[s]);
        if (!ok) {
            CHECK(0, "after rewrite %d of slot %u", round, slot);
            break;
        }
    }
    printf("code61 worst case, 5 presets: 600 rewrites, %ld erases\n", sim_erases);
    static uint8_t before[sizeof(sim_flash)];
    int st = GSX_OK;
    uint8_t s;
    for (s = 6; s < 20 && st == GSX_OK; s++) {
        memcpy(before, sim_flash, sizeof(sim_flash));
        st = write_code61(s, s);
    }
    s--;
    CHECK(st == GSX_BAD_LEN && s >= 6, "preset %u doesn't fit (%d)", s, st);
    CHECK(memcmp(before, sim_flash, sizeof(sim_flash)) == 0, "and nothing was erased or written");
    int ok = 1;
    for (uint8_t k = 0; k < s; k++)
        ok &= is_code61(k, k < 6 ? cur[k] : k);
    CHECK(ok, "every slot written before it still reads back");
    for (uint8_t k = 6; k < s; k++)
        CHECK(config_store_erase_slot(k) == GSX_OK, "reset preset %u", k);
    CHECK(write_code61(5, 77) == GSX_OK && is_code61(5, 77) && is_code61(0, cur[0]), "fits again after resets");
}

/* A v3 record of `groups` int8 entries (ids 10.., index 0, all v; 5 +
 * groups words), written at off by hand. Returns the offset after it. */
static uint32_t raw_recg(uint32_t off, uint32_t seq, uint8_t slot, int8_t v, uint32_t groups)
{
    uint32_t crc = CRC32_INIT;
    wr(&off, 0x33464347u, &crc);
    wr(&off, seq, &crc);
    wr(&off, slot, &crc);
    wr(&off, groups, &crc);
    for (uint32_t g = 0; g < groups; g++)
        wr(&off, (10u + g) | (uint32_t)(uint8_t)v << 24, &crc);
    wr(&off, ~crc, 0);
    return off;
}

static void raw_rec(uint32_t off, uint32_t seq, uint8_t slot, int8_t v)
{
    raw_recg(off, seq, slot, v, 1);
}

/* A page filled to its last word: records end exactly at the page end and
 * nothing may run on into the next page (the two-page swap did: its walk
 * of a full first page went on into the second, and a swap then copied
 * its own copies on past the config pages). */
static void test_full_page(void)
{
    for (uint32_t p = 0; p < BOARD_CONFIG_PAGES; p++) {
        fresh();
        uint32_t off = p * BOARD_FLASH_PAGE, seq = 1;
        for (int i = 0; i < 41; i++)
            off = raw_recg(off, seq++, (uint8_t)(1 + i % 3), (int8_t)i, 1);
        off = raw_recg(off, seq++, 0, 99, 5);
        CHECK(off == (p + 1) * BOARD_FLASH_PAGE, "page %u filled exactly (%u)", p, off);
        CHECK(value_of(0) == 99 && value_of(1) == 39 && value_of(2) == 40 && value_of(3) == 38,
              "full page %u reads", p);
        int ok = 1;
        for (int i = 0; i < 200 && ok; i++) {
            uint8_t s = (uint8_t)(i % 4);
            ok = write_n(s, i, 20, 0) == GSX_OK && value_of(s) == i;
        }
        CHECK(ok, "writes after a full page %u", p);
    }
}

/* What the two-page swap of earlier firmware leaves: page 1 active (every
 * slot's newest record; slot 3's a copy, same seq, of the one on page 0),
 * page 0 the old page, the third page as the stock app left it (blank, or
 * junk). It reads as it is and keeps working. */
static void test_old_layout(void)
{
    for (int junk = 0; junk < 2; junk++) {
        fresh();
        raw_rec(0, 1, 0, 1);
        raw_rec(24, 2, 1, 2);
        raw_rec(48, 3, 3, 33);
        raw_rec(72, 4, 0, 4);
        raw_rec(1024, 2, 1, 2);            /* the swap's copies */
        raw_rec(1024 + 24, 3, 3, 33);
        raw_rec(1024 + 48, 5, 0, 5);       /* the record that made page 1 active */
        raw_rec(1024 + 72, 6, 1, 6);
        if (junk)
            for (uint32_t i = 2048; i < sizeof(sim_flash); i++)
                sim_flash[i] = (uint8_t)(i * 13u + 5u);
        cs_test_reboot();
        CHECK(value_of(0) == 5 && value_of(1) == 6 && value_of(3) == 33 && value_of(2) == -1,
              "old layout reads (junk %d): %d %d %d", junk, value_of(0), value_of(1), value_of(3));
        int ok = 1;
        for (int i = 0; i < 300 && ok; i++) {
            uint8_t s = (uint8_t)(i % 2 ? 0 : 1);
            ok = write_n(s, i, 30, 0) == GSX_OK && value_of(s) == i && value_of(3) == 33;
        }
        CHECK(ok, "old layout keeps working (junk %d)", junk);
    }
}

/* How often a typical COMMIT erases a page: a preset with 12 changed values
 * and a name, then the globals with 4 changed values, presets 1-5 in turn. */
static void test_wear(void)
{
    fresh();
    sim_erases = 0;
    int saves = 2000;
    for (int i = 0; i < saves; i++) {
        CHECK(write_n((uint8_t)(1 + i % 5), i, 12, "Preset") == GSX_OK, "save %d", i);
        CHECK(write_n(0, i, 4, 0) == GSX_OK, "globals %d", i);
    }
    printf("typical saves: %d saves, %ld erases (%.1f saves per erase)\n", saves, sim_erases,
           (double)saves / (double)sim_erases);
    CHECK(sim_erases * 9 < saves, "%ld erases", sim_erases);  /* the two-page swap: 1 in 7.7 */
}

/* v2 records (2 words per entry) from earlier firmware still load, survive
 * compactions as they are, and give way to v3 on the slot's next write. */
static void test_v2(void)
{
    fresh();
    uint32_t off = 0, crc = CRC32_INIT;
    wr(&off, CONFIG_STORE_MAGIC_V2, &crc);
    wr(&off, 5, &crc);                     /* seq */
    wr(&off, 1u | CS_F_NAME << 8 | 9u << 16, &crc);
    wr(&off, 2, &crc);
    wr(&off, 'O' | 'l' << 8 | 'd' << 16, &crc);
    for (int i = 0; i < 3; i++)
        wr(&off, 0, &crc);
    wr(&off, 10u | 3u << 16, &crc);
    wr(&off, 0xFFFFFF85u, &crc);           /* -123 */
    wr(&off, 9u, &crc);
    wr(&off, 70000u, &crc);
    wr(&off, ~crc, 0);
    cs_slot_info_t info;
    got_t g = { 0 };
    CHECK(config_store_read(1, &info, collect, &g) == 1 && g.n == 2 && g.e[0].id == 10 && g.e[0].index == 3 &&
          g.e[0].value == -123 && g.e[1].id == 9 && g.e[1].value == 70000 && info.revision == 9 &&
          strcmp(info.name, "Old") == 0, "v2 record reads (%d entries)", g.n);
    for (int i = 0; i < 60; i++)
        CHECK(write_code61(0, i) == GSX_OK, "globals %d", i);
    g.n = 0;
    CHECK(config_store_read(1, &info, collect, &g) == 1 && g.n == 2 && g.e[1].value == 70000 &&
          strcmp(info.name, "Old") == 0, "v2 kept over compactions");
    CHECK(is_code61(0, 59), "globals v3");
    CHECK(write_code61(1, 1) == GSX_OK && is_code61(1, 1), "v2 slot rewritten as v3");
}

/* Records that aren't ours: a CRC-valid v3 record with a bad group reads as
 * no record; old junk in a page is never trusted. */
static void test_malformed(void)
{
    static const uint32_t bad[][3] = {
        { 1, 0x00000301u, 0 },             /* width 3 */
        { 1, 0x01000401u, 0 },             /* span 2 with value bits set */
        { 2, 0x00000401u, 0xFFFFFFFCu },   /* span 2: bitmap bits past the span */
        { 2, 0x00000501u, 0x00000003u },   /* int16, 2 values: the value word is missing */
    };
    for (uint32_t t = 0; t < sizeof(bad) / sizeof(bad[0]); t++) {
        fresh();
        uint32_t off = 0, crc = CRC32_INIT, n = bad[t][0];
        wr(&off, 0x33464347u, &crc);
        wr(&off, 1, &crc);
        wr(&off, 2u, &crc);                /* slot 2 */
        wr(&off, n, &crc);
        for (uint32_t i = 0; i < n; i++)
            wr(&off, bad[t][1 + i], &crc);
        wr(&off, ~crc, 0);
        got_t g = { 0 };
        CHECK(config_store_read(2, 0, collect, &g) == 0 && g.n == 0, "bad group %u reads as no record", t);
        CHECK(write_code61(2, 5) == GSX_OK && is_code61(2, 5), "bad group %u: store still works", t);
    }
    for (uint32_t i = 0; i < sizeof(sim_flash); i++)
        sim_flash[i] = (uint8_t)(i * 37u + 11u);
    cs_test_reboot();
    CHECK(config_store_read(0, 0, 0, 0) == 0, "junk pages read as empty");
    CHECK(write_code61(0, 3) == GSX_OK && write_code61(1, 4) == GSX_OK && is_code61(0, 3) && is_code61(1, 4),
          "writes over junk pages");
}

/* A power cut at every halfword program and erase of a run of writes that
 * includes compactions, at the Code 61's worst case (5 presets + the
 * globals): the slot being written reads its old or its new record, the
 * others always read as before, and the store keeps working afterwards,
 * also when a second cut hits the next write. */
#define PC_WRITES 14
static const uint8_t pc_slot[6] = { 0, 2, 4, 1, 5, 3 };
static int32_t pc_cur[6];

static int pc_check(int cut_slot, int32_t cut_v)
{
    for (uint8_t s = 0; s < 6; s++)
        if (!is_code61(s, pc_cur[s]) && !(s == cut_slot && is_code61(s, cut_v)))
            return 0;
    return 1;
}

static void test_power_cuts(void)
{
    volatile int ran = 0, in_compaction = 0, second = 0;
    for (volatile long cut = 0; cut < 100000; cut++) {
        fresh();
        for (uint8_t s = 0; s <= 5; s++) {
            write_code61(s, 100 + s);
            pc_cur[s] = 100 + s;
        }
        volatile int done = 0, last = 0;
        sim_erases = 0;
        cut_after = cut;
        if (!setjmp(cut_jb)) {
            for (int k = 1; k <= PC_WRITES; k++) {
                if (write_code61(pc_slot[k % 6], k) != GSX_OK)
                    break;
                pc_cur[pc_slot[k % 6]] = k;
                last = k;
            }
            done = 1;
        }
        cut_after = -1;
        if (!done && cs_test_index_matches() != -1) {
            CHECK(0, "cut %ld: the store kept its index over a cut write", cut);
            break;
        }
        if (cut & 1)
            cs_test_reboot();              /* the reset after a cut (else: the store's own rescan) */
        if (sim_erases > 0 && !done)
            in_compaction++;
        int next = last + 1;               /* the write that was cut */
        int cs = done ? -1 : pc_slot[next % 6];
        if (!pc_check(cs, next)) {
            CHECK(0, "cut %ld: slots wrong (last %d)", cut, last);
            break;
        }
        if (done)
            CHECK(last == PC_WRITES, "the writes failed at %d", last + 1);
        if (cs >= 0 && is_code61((uint8_t)cs, next))
            pc_cur[cs] = next;
        /* a second cut, somewhere in the next write (a worst-case compaction is ~900 steps) */
        volatile int cut2_done = 0;
        cut_after = (cut * 7919) % 1000;
        if (!setjmp(cut_jb)) {
            write_code61(3, 88);
            cut2_done = 1;
        }
        cut_after = -1;
        if (!cut2_done)
            second++;
        if (cut & 2)
            cs_test_reboot();
        if (!pc_check(3, 88)) {
            CHECK(0, "cut %ld + second cut: slots wrong", cut);
            break;
        }
        if (write_code61(3, 89) != GSX_OK || write_code61(1, 90) != GSX_OK) {
            CHECK(0, "cut %ld: store broken", cut);
            break;
        }
        pc_cur[3] = 89;
        pc_cur[1] = 90;
        if (!pc_check(-1, 0) || cs_test_index_matches() == 0) {
            CHECK(0, "cut %ld: slots wrong after recovery", cut);
            break;
        }
        ran++;
        if (done)
            break;                         /* the writes finished before the cut: every point covered */
    }
    CHECK(in_compaction > 1000 && ran > 3000 && second > 1000,
          "only %d cut points (%d after a compaction began, %d second cuts)", ran, in_compaction, second);
    printf("power cuts: %d points, %d after a compaction began, %d second cuts\n", ran, in_compaction, second);
}

/* The index kept between calls: reads cost no scan; an append keeps it
 * (updated), a compaction, an erase or a failed write drops it and the next
 * call scans; a record that no longer checks out makes a read scan again. */
static void test_index(void)
{
    fresh();
    int32_t cur[6];
    for (uint8_t s = 0; s <= 5; s++) {
        CHECK(write_code61(s, 20 + s) == GSX_OK, "preset %u", s);
        cur[s] = 20 + s;
    }
    unsigned before = cs_test_scans;
    int ok = 1;
    for (int k = 0; k < 10; k++)
        for (uint8_t s = 0; s <= 5; s++)
            ok &= is_code61(s, cur[s]);
    CHECK(ok && config_store_read(7, 0, 0, 0) == 0 && config_store_read(200, 0, 0, 0) == 0, "reads");
    CHECK(cs_test_scans == before, "60 reads: no scan (%u)", cs_test_scans - before);

    /* write -> read -> compaction -> read, the index checked against a scan */
    int appends = 0, compactions = 0;
    for (int i = 0; i < 60; i++) {
        uint8_t s = (uint8_t)(i % 6);
        long erases = sim_erases;
        CHECK(write_code61(s, 100 + i) == GSX_OK, "write %d", i);
        cur[s] = 100 + i;
        /* -1: a compaction (into a blank spare: no erase) */
        int m = cs_test_index_matches();
        if (m == -1)
            compactions++;
        else
            appends++;
        CHECK(m != 0 && (sim_erases == erases || m == -1), "write %d: index %d", i, m);
        if (m == -1) {
            before = cs_test_scans;
            CHECK(is_code61(s, cur[s]) && cs_test_scans == before + 1, "read %d: one scan", i);
        }
        before = cs_test_scans;
        ok = 1;
        for (uint8_t k = 0; k <= 5; k++)
            ok &= is_code61(k, cur[k]);
        CHECK(ok && cs_test_scans == before, "write %d: every slot reads, no scan", i);
    }
    CHECK(appends > 10 && compactions > 3, "%d appends, %d compactions", appends, compactions);

    /* tombstones and erase */
    CHECK(config_store_erase_slot(2) == GSX_OK && cs_test_index_matches() != 0 && value_of(2) == -1,
          "tombstone");
    before = cs_test_scans;
    CHECK(config_store_erase_slot(2) == GSX_OK && value_of(2) == -1 && cs_test_scans == before,
          "a second reset writes nothing, scans nothing");
    CHECK(config_store_erase() == GSX_OK && cs_test_index_matches() == -1, "erase drops the index");
    for (uint8_t s = 0; s <= 5; s++)
        CHECK(value_of(s) == -1, "slot %u gone after erase", s);
    CHECK(cs_test_index_matches() == 1, "erased store indexed");

    /* a failed write (a word in the way is not blank: PGERR) drops the index */
    CHECK(write_n(1, 5, 4, 0) == GSX_OK && write_n(1, 6, 4, 0) == GSX_OK, "two records of slot 1");
    uint32_t newest_at = 0;
    while (rd(newest_at) != 0x33464347u || rd(newest_at + 4u) != 2u)   /* the second record: seq 2 */
        newest_at += 4u;
    uint32_t end = newest_at;
    while (rd(end) != 0xFFFFFFFFu)
        end += 4u;
    flash_program_half(BOARD_CONFIG_BASE + end + 8u, 0);   /* where the next record's header goes */
    CHECK(write_n(3, 7, 4, 0) == GSX_FLASH_ERR && cs_test_index_matches() == -1, "a failed write drops it");
    CHECK(value_of(1) == 6 && write_n(3, 8, 4, 0) == GSX_OK && value_of(3) == 8, "and the store goes on");

    /* defensive: the indexed record changed behind the store's back: a read
     * scans again, and the older record of the slot answers */
    CHECK(cs_test_index_matches() == 1, "indexed");
    flash_program_half(BOARD_CONFIG_BASE + newest_at + 16u, 0);   /* slot 1's newest: its group */
    before = cs_test_scans;
    CHECK(value_of(1) == 5 && cs_test_scans == before + 1, "a broken record: one scan, the older one");
}

/* Presets a firmware with more of them stored (up to slot 64) survive the
 * compactions of one that writes fewer slots, for when it is flashed back. */
static void test_other_slots(void)
{
    fresh();
    CHECK(write_n(64, 64, 10, "Last") == GSX_OK && write_n(40, 40, 10, 0) == GSX_OK, "slots 40 and 64");
    CHECK(write_n(65, 1, 1, 0) == GSX_BAD_LEN, "no slot 65");
    sim_erases = 0;
    int ok = 1;
    for (int i = 0; i < 400 && ok; i++) {
        uint8_t s = (uint8_t)(i % 5);
        ok = write_n(s, i, 20, 0) == GSX_OK && value_of(s) == i && value_of(40) == 40 && value_of(64) == 64;
    }
    CHECK(ok && sim_erases > 10, "slots 40 and 64 kept over %ld compactions", sim_erases);
    cs_test_reboot();
    cs_slot_info_t info;
    CHECK(value_of(40) == 40 && config_store_read(64, &info, 0, 0) == 1 && strcmp(info.name, "Last") == 0,
          "and after a reset");
}

int main(void)
{
    test_basics();
    test_packing();
    test_compaction();
    test_code61();
    test_old_layout();
    test_full_page();
    test_v2();
    test_malformed();
    test_wear();
    test_index();
    test_other_slots();
    test_power_cuts();
    printf("%s\n", fails ? "FAILED" : "cs_test ok");
    return fails != 0;
}

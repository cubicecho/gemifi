/*
 * cs_test.c: platform/stm32f1/src/config_store.c on simulated flash.
 *
 * The simulated flash acts like the F1's: erased bytes are 0xFF, a halfword
 * can be programmed only while it is erased (or to 0), and a page erase
 * clears 1 KB. A power cut after the Nth halfword program stops the write
 * there (longjmp), as a reset would.
 *
 * Covers the packed v3 groups (widths, gaps, limits), the Code 61's worst
 * case (3 presets + the globals in one page), old v2 records, CRC-valid
 * records with bad groups, junk pages, and power cuts through page swaps.
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

static long cut_after = -1;
static long sim_erases;                    /* page erases so far */                /* halfword programs left; -1 = no cut */
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

int flash_erase_page(uint32_t addr)
{
    sim_erases++;
    memset(sim_flash + (addr - BOARD_CONFIG_BASE), 0xFF, BOARD_FLASH_PAGE);
    return 0;
}

int flash_program_half(uint32_t addr, uint16_t v)
{
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
 * every per-preset value of a preset (with preset.program: 134), or every
 * global (10), each at a value of its full range. v varies them. */
static uint32_t code61_preset(cs_entry_t *e, int32_t v)
{
    static const struct { uint16_t id, count; int32_t lo, hi; } p[] = {
        { 1, 1, 1, 16 }, { 2, 1, -24, 24 }, { 3, 1, -4, 4 }, { 4, 1, 0, 3 }, { 5, 1, 1, 127 },
        { 8, 9, 0, 127 }, { 9, 41, 0, 15 }, { 10, 41, 0, 127 }, { 11, 1, 0, 2000 },
        { 12, 1, 0, 5000 }, { 15, 8, 0, 127 }, { 16, 8, 0, 2 }, { 18, 1, -1, 127 },
        { 20, 16, 0, 127 }, { 21, 1, 0, 16 }, { 22, 1, 0, 3 }, { 23, 1, 0, 1 },
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

static void wr(uint32_t *off, uint32_t w, uint32_t *crc)
{
    flash_program_word(BOARD_CONFIG_BASE + *off, w);
    if (crc)
        *crc = crc32_word(*crc, w);
    *off += 4u;
}

static void fresh(void)
{
    memset(sim_flash, 0xFF, sizeof(sim_flash));
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

/* Many rewrites: every page swap must keep the other slots. */
static void test_swaps(void)
{
    fresh();
    CHECK(write_n(1, 100, 40, "Keys") == GSX_OK, "preset 1");
    CHECK(write_n(3, 300, 10, 0) == GSX_OK, "preset 3");
    CHECK(write_n(2, 200, 5, 0) == GSX_OK && config_store_erase_slot(2) == GSX_OK, "preset 2, then reset");
    for (int i = 0; i < 500; i++) {
        if (write_n(0, i, 40, 0) != GSX_OK) {
            CHECK(0, "rewrite %d failed", i);
            break;
        }
        if (value_of(0) != i || value_of(1) != 100 || value_of(3) != 300 || value_of(2) != -1) {
            CHECK(0, "after rewrite %d: %d %d %d %d", i, value_of(0), value_of(1), value_of(2), value_of(3));
            break;
        }
    }
    cs_slot_info_t info;
    CHECK(config_store_read(1, &info, 0, 0) == 1 && strcmp(info.name, "Keys") == 0, "name kept over swaps");
}

/* The Code 61's worst case: 134 per-preset values in 276 B, the globals in
 * 84 B. 3 presets + the globals fit the page (912 B); a 4th fails with
 * GSX_BAD_LEN, before anything is erased. */
static void test_code61(void)
{
    fresh();
    CHECK(write_code61(0, 1) == GSX_OK && rd(12) == 16 && rd(4 * 21) == 0xFFFFFFFFu,
          "globals: 16 group words, 21 in all (got %u)", rd(12));
    CHECK(write_code61(1, 1) == GSX_OK && rd(84 + 12) == 60 && rd(84 + 4 * 69) == 0xFFFFFFFFu,
          "preset: 60 group words, 69 in all (got %u)", rd(84 + 12));
    CHECK(write_code61(2, 2) == GSX_OK && write_code61(3, 3) == GSX_OK, "presets 2 and 3");
    int32_t cur[4] = { 1, 1, 2, 3 };
    for (int round = 0; round < 40; round++) {
        uint8_t slot = (uint8_t)(round % 4);
        if (write_code61(slot, 10 + round) != GSX_OK) {
            CHECK(0, "rewrite %d of slot %u failed", round, slot);
            break;
        }
        cur[slot] = 10 + round;
        int ok = 1;
        for (uint8_t s = 0; s < 4; s++)
            ok &= is_code61(s, cur[s]);
        if (!ok) {
            CHECK(0, "after rewrite %d of slot %u", round, slot);
            break;
        }
    }
    static uint8_t before[sizeof(sim_flash)];
    memcpy(before, sim_flash, sizeof(sim_flash));
    CHECK(write_code61(4, 4) == GSX_BAD_LEN, "a 4th preset doesn't fit");
    CHECK(memcmp(before, sim_flash, sizeof(sim_flash)) == 0, "and nothing was erased or written");
    CHECK(config_store_erase_slot(3) == GSX_OK && write_code61(4, 4) == GSX_OK && is_code61(4, 4) &&
          value_of(3) == -1 && is_code61(0, cur[0]) && is_code61(1, cur[1]) && is_code61(2, cur[2]),
          "it fits once preset 3 is reset");
}

/* v2 records (2 words per entry) from earlier firmware still load, survive
 * swaps as they are, and give way to v3 on the slot's next write. */
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
          strcmp(info.name, "Old") == 0, "v2 kept over swaps");
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
    CHECK(config_store_read(0, 0, 0, 0) == 0, "junk pages read as empty");
    CHECK(write_code61(0, 3) == GSX_OK && write_code61(1, 4) == GSX_OK && is_code61(0, 3) && is_code61(1, 4),
          "writes over junk pages");
}

/* A power cut at every halfword of a run of writes that includes page
 * swaps: the slot being written reads its old or its new record, the others
 * always read as before, and the store keeps working afterwards. Records
 * are the Code 61's worst case (3 presets + the globals: 89% of the page). */
static void test_power_cuts(void)
{
    volatile int ran = 0, in_swap = 0;
    for (volatile long cut = 0; cut < 50000; cut++) {
        fresh();
        write_code61(1, 100);
        write_code61(2, 200);
        write_code61(3, 300);
        write_code61(0, 0);
        volatile int done = 0, last = 0;
        sim_erases = 0;
        cut_after = cut;
        if (!setjmp(cut_jb)) {
            for (int k = 1; k <= 6; k++) {  /* odd k: slot 0, even k: slot 2 */
                if (write_code61(k % 2 ? 0 : 2, k) != GSX_OK)
                    break;
                last = k;
            }
            done = 1;
        }
        cut_after = -1;
        if (sim_erases > 0 && !done)
            in_swap++;
        int v0 = last >= 5 ? 5 : last >= 3 ? 3 : last >= 1 ? 1 : 0;
        int v2 = last >= 6 ? 6 : last >= 4 ? 4 : last >= 2 ? 2 : 200;
        int next = last + 1;                /* the write that was cut */
        int ok0 = is_code61(0, v0) || (!done && next % 2 && is_code61(0, next));
        int ok2 = is_code61(2, v2) || (!done && !(next % 2) && is_code61(2, next));
        CHECK(ok0 && ok2, "cut %ld: written slots wrong (last %d)", cut, last);
        CHECK(is_code61(1, 100) && is_code61(3, 300), "cut %ld: other slots lost", cut);
        if (done)
            CHECK(last == 6, "the writes failed at %d", last + 1);
        CHECK(write_code61(3, 88) == GSX_OK && is_code61(3, 88) && is_code61(1, 100), "cut %ld: store broken",
              cut);
        ran++;
        if (done)
            break;                         /* the writes finished before the cut: every point covered */
    }
    CHECK(in_swap > 100 && ran > 1000, "only %d cut points (%d after a swap began)", ran, in_swap);
    printf("power cuts: %d points, %d after a swap began\n", ran, in_swap);
}

int main(void)
{
    test_basics();
    test_packing();
    test_swaps();
    test_code61();
    test_v2();
    test_malformed();
    test_power_cuts();
    printf("%s\n", fails ? "FAILED" : "cs_test ok");
    return fails != 0;
}

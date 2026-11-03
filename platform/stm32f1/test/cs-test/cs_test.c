/*
 * cs_test.c: platform/stm32f1/src/config_store.c on simulated flash.
 *
 * The simulated flash acts like the F1's: erased bytes are 0xFF, a halfword
 * can be programmed only while it is erased (or to 0), and a page erase
 * clears 1 KB. A power cut after the Nth halfword program stops the write
 * there (longjmp), as a reset would.
 *
 * SPDX-License-Identifier: MIT
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "flash.h"
#include "gsx.h"

uint8_t sim_flash[BOARD_CONFIG_PAGES * BOARD_FLASH_PAGE];

static long cut_after = -1;                /* halfword programs left; -1 = no cut */
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

#define MAX_E 160

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

/* A record of n entries, every value v. */
static int write_n(uint8_t slot, int32_t v, uint32_t n, const char *name)
{
    cs_entry_t e[MAX_E];
    for (uint32_t i = 0; i < n; i++)
        e[i] = (cs_entry_t){ (uint16_t)(10 + i), (uint16_t)i, v };
    return config_store_write(slot, 7, name, e, n);
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
    CHECK(g.n == 30 && g.e[29].id == 39 && g.e[29].index == 29 && g.e[29].value == 9, "entries back");
    CHECK(info.revision == 7 && (info.flags & CS_F_NAME) && strcmp(info.name, "Drums") == 0,
          "revision 7, name 'Drums' (got %u, '%s')", info.revision, info.name);
    CHECK(config_store_read(0, &info, 0, 0) == 1 && info.name[0] == 0, "globals have no name");
    CHECK(value_of(0) == 5, "globals still there");
    CHECK(value_of(2) == -1, "slot 2 never written");

    CHECK(config_store_erase_slot(1) == GSX_OK && value_of(1) == -1, "tombstone hides preset 1");
    CHECK(value_of(0) == 5, "globals untouched by the tombstone");
    CHECK(config_store_erase() == GSX_OK && value_of(0) == -1, "erase clears everything");
    CHECK(write_n(0, 1, config_store_max_entries() + 1u, 0) == GSX_BAD_LEN, "too many entries");
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

/* A power cut at every halfword of a write, and of the swap it may cause:
 * the slot written reads old or new, the others always read as before, and
 * the store keeps working afterwards. */
static void test_power_cuts(void)
{
    volatile int ran = 0;
    for (volatile long cut = 0; cut < 2000; cut++) {
        fresh();
        write_n(1, 100, 40, 0);
        write_n(2, 200, 30, 0);
        for (int i = 0; i < 6; i++)
            write_n(0, i, 40, 0);          /* fills the page: the next write swaps */
        int32_t before = value_of(0);
        volatile int done = 0;
        cut_after = cut;
        if (!setjmp(cut_jb)) {
            write_n(0, 77, 40, 0);
            done = 1;
        }
        cut_after = -1;
        int32_t v = value_of(0);
        CHECK(v == before || v == 77, "cut %ld: slot 0 reads %d (old %d)", cut, v, before);
        CHECK(value_of(1) == 100 && value_of(2) == 200, "cut %ld: other slots lost (%d %d)", cut,
              value_of(1), value_of(2));
        CHECK(write_n(0, 88, 40, 0) == GSX_OK && value_of(0) == 88 && value_of(1) == 100 &&
              value_of(2) == 200, "cut %ld: store broken after the cut", cut);
        ran++;
        if (done)
            break;                         /* the write finished before the cut: every point covered */
    }
    CHECK(ran > 100, "only %d cut points", ran);
    printf("power cuts: %d points\n", ran);
}

int main(void)
{
    test_basics();
    test_swaps();
    test_power_cuts();
    printf("%s\n", fails ? "FAILED" : "cs_test ok");
    return fails != 0;
}

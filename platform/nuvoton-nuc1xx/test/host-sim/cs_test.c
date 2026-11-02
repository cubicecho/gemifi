/* cs_test: the NUC1xx config_store.c journal (record v2 slots, compaction,
 * v1 migration) on a simulated data flash, with power cuts at every flash
 * operation. See README.md.
 *
 *   build/cs_test [-q | -long] [seed]
 *   power-cut sweep on every 4th write (-q: every 25th; -long: 20x the
 *   writes, no sweeps)
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config_store.h"
#include "crc32.h"
#include "gsx.h"
#include "nuc1xx.h"

/* ---- simulated data flash (0x1F000..0x1FFFF, 512 B pages) -------------- */

#define FBASE FLASH_DATA_BASE
#define FSIZE FLASH_DATA_SIZE
#define NPAGE ((FLASH_DEVID_ADDR - FBASE) / 512u)
static uint8_t flash[FSIZE];
static long ops, cut_at = -1;         /* cut at operation number cut_at */
static jmp_buf cut_jb;
static int overprogram;               /* programs onto a non-blank word */
static int bad_addr;
static long erases[8], line_erases, line_writes;
static uint32_t rng = 12345;

static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

void fmc_open(void) {}
uint32_t fmc_read(uint32_t a)
{
    uint32_t w;
    if (a < FBASE || a + 4 > FBASE + FSIZE)
        return 0xFFFFFFFFu;
    memcpy(&w, flash + (a - FBASE), 4);
    return w;
}

/* The cut operation is done partly (a random subset of the bits/words
 * changes), then the "power" goes. */
static void maybe_cut(int erase, uint32_t a, uint32_t w)
{
    if (ops++ != cut_at)
        return;
    if (erase) {
        uint32_t mode = rnd() % 3;    /* nothing, the first k words, random words */
        uint32_t k = rnd() % 128;
        for (uint32_t i = 0; i < 128; i++)
            if ((mode == 1 && i < k) || (mode == 2 && (rnd() & 1)))
                memset(flash + (a - FBASE) + 4 * i, 0xFF, 4);
    } else if (rnd() & 1) {
        uint32_t o, mask = rnd();     /* only some of the 0 bits get programmed */
        memcpy(&o, flash + (a - FBASE), 4);
        o &= w | mask;
        memcpy(flash + (a - FBASE), &o, 4);
    }
    longjmp(cut_jb, 1);
}

int fmc_erase_page(uint32_t a)
{
    if (a < FBASE || a >= FLASH_DEVID_ADDR || (a & 511)) { bad_addr++; return -1; }
    maybe_cut(1, a, 0);
    memset(flash + (a - FBASE), 0xFF, 512);
    erases[(a - FBASE) / 512]++;
    return 0;
}

int fmc_program(uint32_t a, uint32_t w)
{
    if (a < FBASE || a >= FLASH_DEVID_ADDR || (a & 3)) { bad_addr++; return -1; }
    maybe_cut(0, a, w);
    uint32_t o;
    memcpy(&o, flash + (a - FBASE), 4);
    if (o != 0xFFFFFFFFu)
        overprogram++;
    o &= w;
    memcpy(flash + (a - FBASE), &o, 4);
    return 0;
}

/* ---- checks ----------------------------------------------------------- */

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); if (fails > 20) exit(1); } } while (0)

#define MAXE 64
typedef struct {
    int present;
    uint16_t rev;
    uint8_t flags;
    char name[CS_NAME_LEN + 1];
    uint32_t n;
    cs_entry_t e[MAXE];
} slot_t;

static void collect(const cs_entry_t *e, void *ctx)
{
    slot_t *s = ctx;
    if (s->n < MAXE)
        s->e[s->n] = *e;
    s->n++;
}

static void read_slot(uint8_t slot, slot_t *s)
{
    cs_slot_info_t info;
    memset(s, 0, sizeof *s);
    memset(&info, 0x55, sizeof info);
    s->present = config_store_read(slot, &info, collect, s);
    if (s->present) {
        s->rev = info.revision;
        s->flags = info.flags & CS_F_NAME;
        memcpy(s->name, info.name, sizeof s->name);
    }
}

static int same(const slot_t *a, const slot_t *b)
{
    if (a->present != b->present)
        return 0;
    if (!a->present)
        return 1;
    return a->rev == b->rev && a->flags == b->flags && !strcmp(a->name, b->name) &&
           a->n == b->n && !memcmp(a->e, b->e, a->n * sizeof a->e[0]);
}

/* ---- the model -------------------------------------------------------- */

#define NSLOT 65
static slot_t model[NSLOT];
static uint8_t slots[NSLOT];
static uint32_t slot_max[NSLOT];       /* per-slot entry limit (0: the scenario's) */
static int nslots;

typedef struct {
    int erase;                         /* erase_slot instead of write */
    uint8_t slot;
    slot_t rec;
} op_t;

static void random_op(op_t *op, uint32_t max_n, int tomb_pct)
{
    memset(op, 0, sizeof *op);
    op->slot = slots[rnd() % nslots];
    if ((int)(rnd() % 100) < tomb_pct) {
        op->erase = 1;
        return;
    }
    slot_t *r = &op->rec;
    r->present = 1;
    if (slot_max[op->slot])
        max_n = slot_max[op->slot];
    r->rev = (uint16_t)rnd();
    r->n = rnd() % 4 ? max_n : rnd() % (max_n + 1);   /* mostly worst case */
    for (uint32_t i = 0; i < r->n; i++) {
        r->e[i].id = (uint16_t)rnd();
        r->e[i].index = (uint16_t)rnd();
        uint32_t v = rnd();
        r->e[i].value = (int32_t)(v % 5 == 0 ? 0xFFFFFFFFu : v);   /* -1 = a blank word */
    }
    if (rnd() % 3) {
        r->flags = CS_F_NAME;
        uint32_t len = 1 + rnd() % CS_NAME_LEN;
        for (uint32_t i = 0; i < len; i++)
            r->name[i] = (char)(' ' + 1 + rnd() % 94);
    }
}

static int run_op(const op_t *op)
{
    if (op->erase)
        return config_store_erase_slot(op->slot);
    return config_store_write(op->slot, op->rec.rev, op->rec.flags ? op->rec.name : NULL,
                              op->rec.e, op->rec.n);
}

static void apply_model(const op_t *op)
{
    if (op->erase)
        model[op->slot].present = 0;
    else
        model[op->slot] = op->rec;
}

/* Every slot reads its model value; `alt` (slot, may be NULL) may read
 * either the model or alt. Returns 1 if alt was read. */
static int verify(const char *what, int altslot, const slot_t *alt)
{
    int got_alt = 0;
    for (int i = 0; i < nslots; i++) {
        slot_t s;
        read_slot(slots[i], &s);
        if (slots[i] == altslot && alt && same(&s, alt) && !same(&s, &model[slots[i]])) {
            got_alt = 1;
            continue;
        }
        CHECK(same(&s, &model[slots[i]]), "%s: slot %d reads wrong (present %d/%d, n %u/%u)",
              what, slots[i], s.present, model[slots[i]].present, s.n, model[slots[i]].n);
    }
    /* slots nobody writes stay empty */
    for (int t = 0; t < 3; t++) {
        uint8_t free_slot = (uint8_t)(rnd() % NSLOT);
        int used = 0;
        for (int i = 0; i < nslots; i++)
            used |= slots[i] == free_slot;
        if (!used) {
            slot_t s;
            read_slot(free_slot, &s);
            CHECK(!s.present, "%s: unused slot %d reads a record", what, free_slot);
        }
    }
    return got_alt;
}

static void reset_all(void)
{
    memset(flash, 0xFF, sizeof flash);
    memset(model, 0, sizeof model);
    memset(erases, 0, sizeof erases);
    overprogram = bad_addr = 0;
}

/* Run op with a power cut at every flash operation it does. After each
 * cut: every slot reads its old value (the target: old or new); a retry
 * of the op (itself cut once at random, then retried uncut) must then
 * succeed. The state after one random cut + retries becomes the new state. */
static long cut_runs;
static void op_with_cuts(const op_t *op, int sweep)
{
    static uint8_t before[FSIZE], chosen[FSIZE];
    memcpy(before, flash, sizeof flash);
    slot_t newv = op->erase ? (slot_t){ 0 } : op->rec;

    ops = 0; cut_at = -1;
    long e0 = 0;
    for (unsigned p = 0; p < NPAGE; p++) e0 += erases[p];
    int st = run_op(op);
    for (unsigned p = 0; p < NPAGE; p++) line_erases += erases[p];
    line_erases -= e0;
    line_writes++;
    long total = ops;
    CHECK(st == GSX_OK, "uncut op on slot %d failed (%d)", op->slot, st);
    if (!sweep) {
        apply_model(op);
        return;
    }
    long keep = rnd() % (total + 1);    /* total = uncut */
    for (long k = 0; k < total; k++) {
        memcpy(flash, before, sizeof flash);
        ops = 0; cut_at = k;
        if (!setjmp(cut_jb)) {
            run_op(op);
            CHECK(0, "cut %ld of %ld didn't happen", k, total);
        }
        cut_at = -1;
        cut_runs++;
        verify("after a cut", op->slot, &newv);
        /* retry, cut once more at a random point */
        ops = 0; cut_at = rnd() % (total + 4);
        if (!setjmp(cut_jb))
            run_op(op);
        cut_at = -1;
        verify("after a second cut", op->slot, &newv);
        CHECK(run_op(op) == GSX_OK, "retry after cut %ld failed", k);
        slot_t m = model[op->slot];
        model[op->slot] = newv;
        verify("after the retry", -1, NULL);
        model[op->slot] = m;
        if (k == keep) {
            memcpy(chosen, flash, sizeof flash);   /* continue from here */
            keep = -2;
        }
    }
    if (keep == -2)
        memcpy(flash, chosen, sizeof flash);
    apply_model(op);
}

static int wmul = 1;                   /* -long: 20x the writes, no sweeps */
static void scenario(const char *name, const uint8_t *sl, int n, uint32_t max_n,
                     int tomb_pct, int writes, int sweep_every)
{
    writes *= wmul;
    reset_all();
    nslots = n;
    memcpy(slots, sl, n);
    op_t op;
    long sweeps = 0, c0 = cut_runs;
    line_erases = line_writes = 0;
    for (int w = 0; w < writes; w++) {
        random_op(&op, max_n, tomb_pct);
        int sweep = sweep_every && w % sweep_every == 0;
        sweeps += sweep;
        op_with_cuts(&op, sweep);
        verify(name, -1, NULL);
    }
    CHECK(bad_addr == 0 && overprogram == 0, "%s: bad address (%d) or overprogram (%d)", name, bad_addr, overprogram);
    printf("  %-40s %5d writes, %3ld swept (%6ld cuts), %.2f writes/erase\n",
           name, writes, sweeps, cut_runs - c0, (double)line_writes / (line_erases ? line_erases : 1));
}

/* ---- v1 records ------------------------------------------------------- */

static uint32_t put_v1(uint32_t a, uint32_t seq, uint32_t n, uint32_t seed)
{
    uint32_t crc = CRC32_INIT, w[3 + 2 * 64];
    uint32_t k = 0;
    w[k++] = CONFIG_STORE_MAGIC;
    w[k++] = seq;
    w[k++] = n;
    for (uint32_t i = 0; i < n; i++) {
        w[k++] = (seed + i) | i << 16;
        w[k++] = seed * 1000 + i;
    }
    for (uint32_t i = 0; i < k; i++)
        crc = crc32_word(crc, w[i]);
    w[k++] = ~crc;
    memcpy(flash + (a - FBASE), w, 4 * k);
    return a + 4 * k;
}

static void v1_slot(slot_t *s, uint32_t n, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->present = 1;
    s->n = n;
    for (uint32_t i = 0; i < n; i++) {
        s->e[i].id = (uint16_t)(seed + i);
        s->e[i].index = (uint16_t)i;
        s->e[i].value = (int32_t)(seed * 1000 + i);
    }
}

/* An old journal: older v1 records, the newest one (seq near the wrap). */
static void v1_journal(uint32_t n)
{
    reset_all();
    uint32_t a = put_v1(FBASE + 512, 0xFFFFFFF0u, 20, 3);
    put_v1(a, 0xFFFFFFF1u, 20, 4);
    put_v1(FBASE + 1024, 0xFFFFFFF2u, n, 7);      /* newest */
    put_v1(FBASE + 1536 + 0, 0xFFFFFFE0u, 5, 9);  /* older still */
    v1_slot(&model[0], n, 7);
    v1_slot(&model[1], n, 7);
}

static void v1_tests(int quick)
{
    static const uint8_t sl[] = { 0, 1, 2, 3 };
    nslots = 4;
    memcpy(slots, sl, sizeof sl);
    op_t op;

    v1_journal(32);
    verify("v1 reads as slots 0 and 1", -1, NULL);
    {
        cs_slot_info_t info;
        CHECK(config_store_read(1, &info, NULL, NULL) == 1 && info.revision == 0 &&
              info.name[0] == 0 && !(info.flags & CS_F_NAME), "v1 info: revision 0, no name");
    }
    /* writes to another slot: v1 copied forward as v1 (still both slots) */
    for (int i = 0; i < 40; i++) {
        random_op(&op, 20, 0);
        op.slot = 2 + (i & 1);
        op_with_cuts(&op, !quick || i % 8 == 0);
        verify("v1 + slot 2/3 writes", -1, NULL);
    }
    /* slot 0 written: v1 answers for slot 1 only, compaction converts it */
    for (int i = 0; i < 40; i++) {
        random_op(&op, 20, 0);
        op.slot = i ? (uint8_t)(rnd() % 4) : 0;
        if (op.slot == 1)
            op.slot = 0;
        op_with_cuts(&op, !quick || i % 4 == 0);
        verify("v1 as slot 1 only", -1, NULL);
    }
    slot_t s;
    read_slot(1, &s);
    CHECK(same(&s, &model[1]), "v1 still slot 1 after compaction");
    /* tombstone over the v1 for slot 1 */
    op.erase = 1; op.slot = 1;
    op_with_cuts(&op, 1);
    verify("v1 slot 1 reset", -1, NULL);

    /* a full-page v1 (62 entries): a converted copy keeps the first 61
     * (a v2 header is one word longer). Not swept: only the cap is new. */
    v1_journal(62);
    verify("v1 62 entries", -1, NULL);
    random_op(&op, 10, 0);
    op.slot = 1;
    op_with_cuts(&op, 0);
    slot_t cut61 = model[0];
    cut61.n = 61;
    for (int i = 0; i < 30; i++) {
        random_op(&op, 10, 0);
        op.slot = 2;
        op_with_cuts(&op, 0);
        if (verify("v1 62 -> slot 0 only", 0, &cut61))
            model[0] = cut61;
    }
    read_slot(0, &s);
    CHECK(s.present && s.n == 61 && same(&s, &cut61), "62-entry v1 kept as 61 entries in slot 0 (n %u)", s.n);
    printf("  v1 migration (incl. seq wrap, conversion)    ok\n");
}

/* ---- the rest --------------------------------------------------------- */

static void api_tests(void)
{
    static const uint8_t sl[] = { 0, 1, 5, 64 };
    reset_all();
    nslots = 4;
    memcpy(slots, sl, sizeof sl);
    verify("empty", -1, NULL);
    CHECK(config_store_max_entries() == 59, "max entries 59 (got %u)", config_store_max_entries());

    cs_entry_t e[62];
    for (int i = 0; i < 62; i++)
        e[i] = (cs_entry_t){ (uint16_t)i, 0, i };
    CHECK(config_store_write(5, 1, "x", e, 60) == GSX_BAD_LEN, "60 entries + name: BAD_LEN");
    CHECK(config_store_write(5, 1, NULL, e, 62) == GSX_BAD_LEN, "62 entries: BAD_LEN");
    CHECK(config_store_write(5, 1, NULL, e, 61) == GSX_OK, "61 entries, no name");
    CHECK(config_store_write(5, 2, "sixteen chars!!!", e, 59) == GSX_OK, "59 entries + name");
    slot_t s;
    read_slot(5, &s);
    CHECK(s.present && s.n == 59 && s.rev == 2 && !strcmp(s.name, "sixteen chars!!!"), "full named record");
    CHECK(config_store_write(5, 3, "a name that is too long", e, 1) == GSX_OK, "long name");
    read_slot(5, &s);
    CHECK(!strcmp(s.name, "a name that is t") && s.rev == 3 && s.n == 1, "name cut to 16");
    CHECK(config_store_write(5, 4, "", e, 0) == GSX_OK, "empty name, no entries");
    cs_slot_info_t info;
    CHECK(config_store_read(5, &info, NULL, NULL) == 1 && info.name[0] == 0 && !(info.flags & CS_F_NAME) &&
          info.revision == 4, "\"\" = no name; 0 entries is still a record");
    CHECK(config_store_erase_slot(5) == GSX_OK && config_store_read(5, &info, NULL, NULL) == 0, "tombstone");
    long o = ops;
    CHECK(config_store_erase_slot(5) == GSX_OK && ops == o, "erase_slot of an empty slot writes nothing");
    CHECK(config_store_write(64, 7, NULL, e, 3) == GSX_OK && config_store_read(64, NULL, NULL, NULL) == 1, "slot 64");
    CHECK(config_store_read(63, NULL, NULL, NULL) == 0, "slot 63 empty");
    CHECK(config_store_erase() == GSX_OK && config_store_read(64, NULL, NULL, NULL) == 0, "erase all");
    for (unsigned p = 0; p < NPAGE; p++)
        for (unsigned i = 0; i < 512; i++)
            if (flash[p * 512 + i] != 0xFF) { CHECK(0, "erase left data"); p = NPAGE; break; }
    /* the device-ID page is never touched */
    memset(flash + (FLASH_DEVID_ADDR - FBASE), 0x5A, 512);
    for (int i = 0; i < 200; i++)
        config_store_write((uint8_t)(i % 3), 0, "n", e, 30);
    for (unsigned i = 0; i < 512; i++)
        if (flash[FLASH_DEVID_ADDR - FBASE + i] != 0x5A) { CHECK(0, "device-ID page changed"); break; }
    CHECK(overprogram == 0 && bad_addr == 0, "no overprogramming (%d), no bad addresses", overprogram);
    printf("  api: lengths, names, tombstones, erase       ok\n");
}

/* Beyond the budget (8 records of 508 B in 7 pages): a write that can't
 * fit fails with FLASH_ERR, and every slot keeps its last record. */
static void overfull(void)
{
    static const uint8_t s8[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    reset_all();
    nslots = 8;
    memcpy(slots, s8, sizeof s8);
    int failed = 0;
    for (int w = 0; w < 200; w++) {
        op_t op;
        random_op(&op, 59, 0);
        op.rec.n = 59;
        op.rec.flags = CS_F_NAME;
        strcpy(op.rec.name, "full");
        ops = 0; cut_at = -1;
        if (run_op(&op) == GSX_OK)
            apply_model(&op);
        else
            failed++;
        verify("overfull", -1, NULL);
    }
    CHECK(failed > 0, "overfull: some writes fail");
    CHECK(overprogram == 0 && bad_addr == 0, "overfull: no overprogramming");
    printf("  over budget: %3d of 200 writes refused, others intact   ok\n", failed);
}

int main(int argc, char **argv)
{
    int quick = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q"))
            quick = 1;
        else if (!strcmp(argv[i], "-long"))
            wmul = 20;
        else
            rng = (uint32_t)strtoul(argv[i], NULL, 0) * 2654435761u + 1u;
    }
    int sw = wmul > 1 ? 0 : quick ? 25 : 4;   /* sweep every n-th write */
    printf("config_store (NUC1xx journal):\n");
    api_tests();
    v1_tests(quick);

    /* The budget (NUC): the worst-case records, packed whole into 512 B
     * pages, fit in pages - 2 = 5 pages (one page is the erase spare, one
     * is room for a slot's old and new record while it is rewritten). The
     * scenarios below are at or near that edge, mostly at worst-case sizes. */
    static const uint8_t s10[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    /* 1 globals + 8 presets, 27 entries + name = 63 words (252 B): 2 a page */
    scenario("9 slots x 27 + name (252 B), 4.5 pages", s10, 9, 27, 0, 600, sw);
    scenario("9 slots x 27 + name, 10% resets", s10, 9, 27, 10, 600, sw);
    scenario("10 slots x 27 + name, 5 pages", s10, 10, 27, 5, 400, sw * 2);
    /* the record limit: 59 entries + name = 127 words (508 B), 1 a page */
    scenario("2 slots x 59 + name (508 B)", s10, 2, 59, 5, 300, sw);
    scenario("5 slots x 59 + name, 5 pages", s10, 5, 59, 5, 300, sw * 2);
    /* globals at the limit + 8 presets at 252 B: 1 + 4 pages */
    slot_max[0] = 59;
    scenario("1 x 59 + 8 x 27, + name, 5 pages", s10, 9, 27, 5, 400, sw * 2);
    slot_max[0] = 0;
    /* 33 slots (0..31 and 64), 4 entries + name (52 B): 9 a page, 4 pages */
    static uint8_t s33[33];
    for (int i = 0; i < 32; i++) s33[i] = (uint8_t)i;
    s33[32] = 64;
    scenario("33 slots x 4 entries + name (52 B)", s33, 33, 4, 10, 1500, sw * 4);
    CHECK(bad_addr == 0, "no bad addresses");
    overfull();

    printf("cs_test: %d checks, %d failed, %ld power cuts\n", checks, fails, cut_runs);
    return fails != 0;
}

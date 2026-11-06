/*
 * boot_sim.c: bootloader/stm32f1/main.c's boot decision on the host, in the
 * style of the NUC host-sim (platform/nuvoton-nuc1xx/test/host-sim): the
 * real main.c, built with the host gcc against simulated flash, registers
 * and SRAM mailbox. Reaching update mode (sys_clock_init), the hand-off
 * (sys_jump) and a reset (sys_reset) end a boot by longjmp.
 *
 * Covers the boot-loop guard (sysex.md 6): a gemifi image that comes back
 * MAILBOX_BOOT_TRIES times without clearing boot_count keeps the bootloader
 * in update mode with reason 2; cold starts (POR, low-power) zero the count;
 * an app that clears it is always started; a non-gemifi app (no header) is
 * never counted; the reset flags land in the mailbox and RCC_CSR is
 * cleared; the update flag and the entry combo still win; a bad image
 * stays; nothing outside the flag page is ever written.
 *
 * SPDX-License-Identifier: MIT
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "stm32f1.h"
#include "sys.h"
#include "flash.h"
#include "crc32.h"

/* Registers: a small table of simulated words, by address. */
static struct { uint32_t addr, val; } regs[64];
static int n_regs;
static volatile uint32_t *sim_reg(uint32_t a)
{
    for (int i = 0; i < n_regs; i++)
        if (regs[i].addr == a)
            return &regs[i].val;
    regs[n_regs].addr = a;
    regs[n_regs].val = 0;
    return &regs[n_regs++].val;
}
#undef REG32
#define REG32(a) (*sim_reg(a))

static volatile gemifi_mailbox_t sim_mailbox;
#undef MAILBOX
#define MAILBOX (&sim_mailbox)

#define main boot_main
#include "main.c"
#undef main

uint8_t sim_flash[SIM_FLASH_SIZE];
static jmp_buf jb;
static uint32_t ms, jump_sp, jump_pc;
static int bad_writes;
enum { R_APP = 1, R_RESET, R_UPDATE };

/* ---- stubs -------------------------------------------------------------- */

void sys_tick_init(void) {}
uint32_t sys_tick_ms(void) { return ms++; }
void sys_delay_ms(uint32_t d) { ms += d; }
int sys_clock_init(void) { longjmp(jb, R_UPDATE); }
void sys_reset(void) { longjmp(jb, R_RESET); }
void sys_jump(uint32_t sp, uint32_t pc) { jump_sp = sp; jump_pc = pc; longjmp(jb, R_APP); }
uint32_t sys_reset_cause(void) { return 0; }
int sys_hse_ok(void) { return 1; }

static int writable(uint32_t a)
{
    return (a >= GEMIFI_IMG_BASE && a < GEMIFI_IMG_LIMIT) ||
           (a >= UPDATE_FLAG_ADDR && a < UPDATE_FLAG_ADDR + 1024u);
}
void flash_unlock(void) {}
void flash_lock(void) {}
uint32_t flash_size_kb(void) { return 128; }
int flash_erase_page(uint32_t a)
{
    if (!writable(a) || (a & 1023u)) {
        bad_writes++;
        return -1;
    }
    memset(sim_flash + (a - SIM_FLASH_BASE), 0xFF, 1024);
    return 0;
}
int flash_program_half(uint32_t a, uint16_t v)
{
    if (!writable(a) || (a & 1u)) {
        bad_writes++;
        return -1;
    }
    sim_flash[a - SIM_FLASH_BASE] &= (uint8_t)v;
    sim_flash[a - SIM_FLASH_BASE + 1] &= (uint8_t)(v >> 8);
    return 0;
}
int flash_program_word(uint32_t a, uint32_t w)
{
    return flash_program_half(a, (uint16_t)w) || flash_program_half(a + 2, (uint16_t)(w >> 16));
}

int recovery_update_requested(void)
{
    return flash_read(UPDATE_FLAG_ADDR) == ((uint32_t)UPDATE_FLAG_H1 << 16 | UPDATE_FLAG_H0) &&
           flash_read(UPDATE_FLAG_ADDR + 4) == ((uint32_t)UPDATE_FLAG_H3 << 16 | UPDATE_FLAG_H2);
}
int recovery_request_update(void)
{
    flash_erase_page(UPDATE_FLAG_ADDR);
    flash_program_word(UPDATE_FLAG_ADDR, (uint32_t)UPDATE_FLAG_H1 << 16 | UPDATE_FLAG_H0);
    flash_program_word(UPDATE_FLAG_ADDR + 4, (uint32_t)UPDATE_FLAG_H3 << 16 | UPDATE_FLAG_H2);
    return 0;
}
void recovery_check(void) {}

void usbd_init(const usbd_desc_t *d) { (void)d; }
void usbd_poll(void) {}
void usbd_disconnect(void) {}
int midi_rx_poll(gsx_rx_t *r) { (void)r; return 0; }
void midi_tx_sysex(const uint8_t *p, uint32_t n) { (void)p; (void)n; }
int midi_tx_flush(uint32_t t) { (void)t; return 0; }

/* ---- the test ----------------------------------------------------------- */

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(uint32_t a, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        sim_flash[a - SIM_FLASH_BASE + i] = (uint8_t)(v >> (8 * i));
}

/* A gemifi image of n bytes at the app base: vectors, header, CRC. */
static void make_image(uint32_t n, int gemifi)
{
    memset(sim_flash + (GEMIFI_IMG_BASE - SIM_FLASH_BASE), 0xFF, GEMIFI_IMG_LIMIT - GEMIFI_IMG_BASE);
    for (uint32_t a = 0; a < n; a += 4)
        put32(GEMIFI_IMG_BASE + a, 0x01000000u + a);
    put32(GEMIFI_IMG_BASE + 0, 0x20004F00u);                   /* SP */
    put32(GEMIFI_IMG_BASE + 4, GEMIFI_IMG_BASE + 0x201u);      /* reset vector, Thumb */
    if (!gemifi) {
        for (uint32_t a = GEMIFI_IMG_HDR_OFFSET; a < GEMIFI_IMG_HDR_END; a += 4)
            put32(GEMIFI_IMG_BASE + a, 0);                       /* no header: M-Audio's app */
        return;
    }
    uint32_t h = GEMIFI_IMG_BASE + GEMIFI_IMG_HDR_OFFSET;
    put32(h, GEMIFI_IMG_MAGIC);
    put32(h + 4, (uint32_t)BOARD_ID << 16 | GEMIFI_IMG_HDR_VERSION);
    put32(h + 8, n);
    put32(h + 12, 0);
    put32(h + 16, 0x00020058u);
    put32(h + 20, 1);
    put32(h + 24, 0xFFFFFFFFu);
    put32(h + 28, 0xFFFFFFFFu);
    uint32_t c = CRC32_INIT;
    for (uint32_t a = 0; a < n; a += 4)
        c = crc32_word(c, a == GEMIFI_IMG_CRC_OFFSET ? 0 : flash_read(GEMIFI_IMG_BASE + a));
    put32(h + 12, ~c);
}

/* One reset with these RCC_CSR flags (and buttons: 1 = Up + Down held). */
static int boot(uint32_t csr, int combo)
{
    n_regs = 0;
    RCC_CSR = csr;
    GPIO_IDR(PORTB) = combo ? 0xFCFFu : 0xFFFFu;
    jump_sp = jump_pc = 0;
    int r = setjmp(jb);
    if (r)
        return r;
    boot_main();
    return -1;
}

#define POR (RCC_CSR_PORRSTF | RCC_CSR_PINRSTF)
#define SFT (RCC_CSR_SFTRSTF | RCC_CSR_PINRSTF)
#define WDG (RCC_CSR_IWDGRSTF | RCC_CSR_PINRSTF)

int main(void)
{
    memset(sim_flash, 0xFF, sizeof sim_flash);
    make_image(4096, 1);

    /* a cold start hands off with boot_count 1 and the flags in the mailbox */
    sim_mailbox.boot_count = 0xDEADBEEFu;     /* random SRAM */
    sim_mailbox.reason = 0x1234u;
    CHECK(boot(POR, 0) == R_APP && jump_sp == 0x20004F00u && jump_pc == GEMIFI_IMG_BASE + 0x201u,
          "POR: the app starts");
    CHECK(sim_mailbox.boot_count == 1 && sim_mailbox.reason == 0 && sim_mailbox.magic == 0 &&
          sim_mailbox.rst_flags == POR, "POR: count 1, reason 0, the flags in the mailbox");
    CHECK(RCC_CSR & RCC_CSR_RMVF, "RCC_CSR cleared (RMVF)");

    /* the guard: 3 hand-offs without a clear, the 4th reset stays */
    CHECK(boot(WDG, 0) == R_APP && sim_mailbox.boot_count == 2 && sim_mailbox.rst_flags == WDG, "watchdog 1: count 2");
    CHECK(boot(SFT, 0) == R_APP && sim_mailbox.boot_count == 3, "software reset: count 3");
    CHECK(boot(WDG, 0) == R_UPDATE && sim_mailbox.reason == MAILBOX_REASON_BOOT_LOOP &&
          sim_mailbox.boot_count == 0, "the 4th reset stays in update mode: reason 2, count 0");
    /* the next hand-off (REBOOT(0), a reset) gets fresh tries; reason 2 stays for the app */
    CHECK(boot(SFT, 0) == R_APP && sim_mailbox.boot_count == 1 && sim_mailbox.reason == MAILBOX_REASON_BOOT_LOOP,
          "after the trap: started again, the app sees reason 2");

    /* an app that clears the count (after 5 s) is always started */
    sim_mailbox.reason = 0;
    for (int i = 0; i < 10; i++) {
        sim_mailbox.boot_count = 0;
        CHECK(boot(SFT, 0) == R_APP, "an app that clears the count: start %d", i);
    }
    /* cold starts reset the count */
    sim_mailbox.boot_count = 3;
    CHECK(boot(POR, 0) == R_APP && sim_mailbox.boot_count == 1, "POR zeroes a count of 3");
    sim_mailbox.boot_count = 3;
    CHECK(boot(RCC_CSR_LPWRRSTF, 0) == R_APP && sim_mailbox.boot_count == 1, "a low-power reset too");
    sim_mailbox.boot_count = 3;
    CHECK(boot(RCC_CSR_PINRSTF, 0) == R_UPDATE, "a pin reset doesn't");

    /* the update flag and the combo still keep it in update mode */
    recovery_request_update();
    CHECK(boot(SFT, 0) == R_UPDATE && !recovery_update_requested() && sim_mailbox.boot_count == 0,
          "update flag: stays, the flag is cleared");
    CHECK(boot(SFT, 1) == R_UPDATE, "Up + Down held: stays");
    CHECK(boot(SFT, 0) == R_APP, "then the app again");

    /* a gemifi image with a bad CRC is never started */
    sim_mailbox.boot_count = 0;
    sim_flash[GEMIFI_IMG_BASE - SIM_FLASH_BASE + 0x400] ^= 1;
    CHECK(boot(POR, 0) == R_UPDATE && sim_mailbox.boot_count == 0, "bad CRC: stays");

    /* M-Audio's app (no gemifi header) is started and never counted */
    make_image(4096, 0);
    sim_mailbox.boot_count = 0;
    for (int i = 0; i < 6; i++)
        CHECK(boot(WDG, 0) == R_APP && sim_mailbox.boot_count == 0, "a non-gemifi app: start %d, not counted", i);
    sim_mailbox.boot_count = 3;
    CHECK(boot(WDG, 0) == R_APP, "a non-gemifi app starts whatever the count");

    /* nothing at all to start */
    memset(sim_flash + (GEMIFI_IMG_BASE - SIM_FLASH_BASE), 0xFF, 0x1000);
    CHECK(boot(POR, 0) == R_UPDATE, "an erased app area: stays");

    CHECK(bad_writes == 0, "no write outside the app area and the flag page (%d)", bad_writes);
    printf("boot-sim: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}

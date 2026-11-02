/*
 * recovery.c: reset-time recovery combo and the vendor update request
 * (recovery.h). Self-contained on purpose: direct register access only, its
 * own tiny flash routines, no globals (it runs before .data/.bss exist), no
 * calls into the rest of gemifi. Treat it as never-changing code: a bug
 * elsewhere in the app must not be able to break it.
 *
 * Runs on whatever clock the previous stage left (8 MHz HSI after a reset,
 * maybe 72 MHz if the vendor bootloader set up the PLL), so its waits are
 * plain spin counts that are long enough at either speed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "recovery.h"
#include "stm32f1.h"
#include "board.h"

#define FLAG_PAGE_SIZE 1024u            /* the vendor's flag page, 1 KB */

/* ---- flash (PM0075 2.3), independent copy of flash.c's logic ----------- */

static int rc_flash_wait(void)
{
    for (uint32_t i = 0; FLASH_SR & FLASH_SR_BSY; i++) {
        IWDG_KR = IWDG_KEY_RELOAD;
        if (i > 8000000u)
            return -1;
    }
    uint32_t sr = FLASH_SR;
    FLASH_SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
    return (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) ? -1 : 0;
}

static int rc_program(uint32_t addr, uint16_t v)
{
    FLASH_CR = FLASH_CR_PG;
    *(volatile uint16_t *)addr = v;
    __asm__ volatile ("dsb" ::: "memory");
    int rc = rc_flash_wait();
    FLASH_CR = 0;
    if (*(const volatile uint16_t *)addr != v)
        rc = -1;
    return rc;
}

int recovery_update_requested(void)
{
    const volatile uint16_t *f = (const volatile uint16_t *)UPDATE_FLAG_ADDR;
    return f[0] == UPDATE_FLAG_H0 && f[1] == UPDATE_FLAG_H1 &&
           f[2] == UPDATE_FLAG_H2 && f[3] == UPDATE_FLAG_H3;
}

int recovery_request_update(void)
{
    if (recovery_update_requested())
        return 0;                       /* no needless erase cycles */

    RCC_CR |= RCC_CR_HSION;             /* program/erase needs the HSI */
    for (uint32_t i = 0; !(RCC_CR & RCC_CR_HSIRDY) && i < 100000u; i++) {
    }
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
    rc_flash_wait();                    /* clear stale flags */

    int rc = 0;
    FLASH_CR = FLASH_CR_PER;
    FLASH_AR = UPDATE_FLAG_ADDR;
    FLASH_CR = FLASH_CR_PER | FLASH_CR_STRT;
    __asm__ volatile ("dsb" ::: "memory");
    if (rc_flash_wait() != 0)
        rc = -1;
    FLASH_CR = 0;
    for (uint32_t a = 0; rc == 0 && a < FLAG_PAGE_SIZE; a += 4u)
        if (*(const volatile uint32_t *)(UPDATE_FLAG_ADDR + a) != 0xFFFFFFFFu)
            rc = -1;
    /* The rest of the page stays FF (the stock app writes FF there too). */
    if (rc == 0 &&
        (rc_program(UPDATE_FLAG_ADDR + 0u, UPDATE_FLAG_H0) ||
         rc_program(UPDATE_FLAG_ADDR + 2u, UPDATE_FLAG_H1) ||
         rc_program(UPDATE_FLAG_ADDR + 4u, UPDATE_FLAG_H2) ||
         rc_program(UPDATE_FLAG_ADDR + 6u, UPDATE_FLAG_H3)))
        rc = -1;
    FLASH_CR = FLASH_CR_LOCK;
    return rc;
}

/* ---- the combo -------------------------------------------------------- */

#if defined(BOARD_RECOVERY_PORT)

#ifndef BOARD_RECOVERY_ACTIVE
#define BOARD_RECOVERY_ACTIVE 0
#endif

__attribute__((noinline)) static void rc_spin(uint32_t n)
{
    while (n--)
        __asm__ volatile ("");
}

/* Set the 4-bit CNF:MODE field of every pin in mask to cfg. */
static void rc_cfg(uint32_t port, uint32_t mask, uint32_t cfg)
{
    for (uint32_t b = 0; b < 16u; b++) {
        if (!(mask & (1u << b)))
            continue;
        volatile uint32_t *cr = b < 8u ? &GPIO_CRL(port) : &GPIO_CRH(port);
        uint32_t sh = 4u * (b & 7u);
        *cr = (*cr & ~(0xFu << sh)) | cfg << sh;
    }
}

#ifndef BOARD_RECOVERY_PULL_MASK
#define BOARD_RECOVERY_PULL_MASK BOARD_RECOVERY_MASK
#endif
/* Spin counts, sized for the 8 MHz HSI the chip runs on after a reset
 * (about 0.5 us per count there; shorter if the bootloader left the PLL
 * on). Settle >= 10 us, then 8 samples ~2.5 ms apart = ~20 ms debounce. */
#define RC_SETTLE_SPINS  200u
#define RC_SAMPLE_SPINS  5000u
#define RC_SAMPLES       8u

__attribute__((used, externally_visible)) void recovery_check(void)
{
    const uint32_t port = BOARD_RECOVERY_PORT, mask = BOARD_RECOVERY_MASK;
    const uint32_t pull = BOARD_RECOVERY_PULL_MASK | mask;
    RCC_APB2ENR |= RCC_APB2_IOP(port);
#ifdef BOARD_RECOVERY_SWJ_NOJTAG
    /* PB3/PB4 (and PA15) are JTAG pins at reset: JTAG off, SWD stays on
     * (SWJ_CFG = 010). SWJ_CFG is write-only, so no read-back. */
    RCC_APB2ENR |= RCC_APB2_AFIO;
    AFIO_MAPR = (AFIO_MAPR & ~AFIO_MAPR_SWJ_MASK) | AFIO_MAPR_SWJ_NOJTAG;
#endif
#ifdef BOARD_RECOVERY_DRIVE_PORT
    RCC_APB2ENR |= RCC_APB2_IOP(BOARD_RECOVERY_DRIVE_PORT);
    GPIO_BSRR(BOARD_RECOVERY_DRIVE_PORT) = BOARD_RECOVERY_DRIVE_LEVEL ?
        BOARD_RECOVERY_DRIVE_MASK : (uint32_t)BOARD_RECOVERY_DRIVE_MASK << 16;
    rc_cfg(BOARD_RECOVERY_DRIVE_PORT, BOARD_RECOVERY_DRIVE_MASK, GPIO_CFG_OUT_PP);
#endif

    /* Inputs with the pull towards "released": active-low pins pull up. */
    GPIO_BSRR(port) = BOARD_RECOVERY_ACTIVE ? pull << 16 : pull;
    rc_cfg(port, pull, GPIO_CFG_PULL);
    rc_spin(RC_SETTLE_SPINS);

    /* Held = every pin in mask active in all samples. */
    int held = 1;
    for (uint32_t i = 0; i < RC_SAMPLES && held; i++) {
        uint32_t idr = GPIO_IDR(port);
        uint32_t active = BOARD_RECOVERY_ACTIVE ? idr : ~idr;
        if ((active & mask) != mask)
            held = 0;
        IWDG_KR = IWDG_KEY_RELOAD;
        rc_spin(RC_SAMPLE_SPINS);
    }

    if (held) {
        recovery_request_update();
        /* SYSRESETREQ: the vendor bootloader runs next and sees the flag.
         * If it only looks after a power-on reset, we come back here; the
         * flag is already set then, so this just resets again until the
         * combo is released or the power is cycled. */
        __asm__ volatile ("dsb" ::: "memory");
        SCB_AIRCR = SCB_AIRCR_VECTKEY | (SCB_AIRCR & SCB_AIRCR_PRIGROUP) | SCB_AIRCR_SYSRESETREQ;
        __asm__ volatile ("dsb" ::: "memory");
        for (;;) {
        }
    }

    /* Not held: the touched pins back to their reset state (input
     * floating, ODR 0). The JTAG-off remap stays (sys.c does it anyway). */
    rc_cfg(port, pull, GPIO_CFG_FLOAT);
    GPIO_BRR(port) = pull;
#ifdef BOARD_RECOVERY_DRIVE_PORT
    rc_cfg(BOARD_RECOVERY_DRIVE_PORT, BOARD_RECOVERY_DRIVE_MASK, GPIO_CFG_FLOAT);
    GPIO_BRR(BOARD_RECOVERY_DRIVE_PORT) = BOARD_RECOVERY_DRIVE_MASK;
#endif
}

#elif defined(BOARD_RECOVERY_NONE)

/* The board header says the combo isn't known yet (see its TODO). This is
 * a reminder, not a warning, so builds stay warning-free meanwhile. */
#pragma message("recovery combo disabled for " BOARD_NAME ": BOARD_RECOVERY_NONE (see the board header TODO)")

__attribute__((used, externally_visible)) void recovery_check(void)
{
}

#else
#warning "board defines neither BOARD_RECOVERY_PORT nor BOARD_RECOVERY_NONE: no recovery combo"

__attribute__((used, externally_visible)) void recovery_check(void)
{
}

#endif

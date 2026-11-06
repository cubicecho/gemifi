/*
 * sys.h: clocks, protected-register unlock, millisecond tick, the SRAM
 * mailbox and the CPU-reset boot handoff (protocol/sysex.md section 6).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_SYS_H
#define GEMIFI_SYS_H

#include <stdint.h>
#include "nuc1xx.h"

#define HCLK_HZ 48000000u

/* Unlock REGWRPROT (TRM 5.2, REGWRPROT). Safe if it is already unlocked. */
void sys_unlock(void);

/* HXT 12 MHz -> PLL 48 MHz -> HCLK and USB clock. Safe to call when the PLL
 * is already running from an earlier stage (the stock LDROM leaves it so). */
void sys_clock_init(void);

/* Millisecond counter built on SysTick, polled (no interrupt). It only
 * advances while sys_tick_ms() is called at least once per ms; time spent
 * elsewhere (e.g. a flash erase) is not counted, so delays and timeouts
 * built on it can only come out longer than asked, never shorter. */
void sys_tick_init(void);
uint32_t sys_tick_ms(void);
void sys_delay_ms(uint32_t ms);

/* ---- SRAM mailbox at 0x2000_0FF0 (protocol/sysex.md section 6) -------- */
typedef struct {
    uint32_t magic;
    uint32_t magic_inv;
    uint32_t boot_count;
    uint32_t reason;
} gemifi_mailbox_t;

#define MAILBOX_ADDR       0x20000FF0u
#define MAILBOX            ((volatile gemifi_mailbox_t *)MAILBOX_ADDR)
#define MAILBOX_MAGIC_BOOT 0x544F4F42u   /* 'BOOT' little-endian */
/* reason values (sysex.md 6) */
#define MAILBOX_REASON_REBOOT    1u      /* app: REBOOT(bootloader) asked   */
#define MAILBOX_REASON_BOOT_LOOP 2u      /* bootloader: boot-loop guard     */
/* The bootloader stays in update mode once boot_count reaches this: that
 * many hand-offs in a row without the app clearing it (bootloader-design.md
 * 3.6). */
#define MAILBOX_BOOT_TRIES       3u

/* Read GCR_RSTSRC, clear the flags that were set (write 1 to clear) and
 * return them. The bootloader calls it once per reset, so an app that
 * reads RSTSRC later only sees RSTSRC_CPU (the hand-off). */
uint32_t sys_reset_cause(void);

/* Reset the CPU (IPRSTC1.CPU_RST) after setting ISPCON.BS:
 *   to_ldrom = 1: next boot from LDROM (the bootloader),
 *   to_ldrom = 0: next boot from APROM (the app).
 * Never returns. Does not touch the mailbox and does not disconnect USB;
 * callers do those first. */
__attribute__((noreturn)) void sys_reset_to(int to_ldrom);

/* Apps (src/hal.c): calibrate hal_delay_us() for short waits. Call once
 * after sys_tick_init(), before interrupts that use it are enabled. */
void hal_delay_calibrate(void);

/* Apps (src/hal.c): note why the app started (hal_reset_reason,
 * hal_boot_loop) from GCR_RSTSRC and the mailbox. Call first in main(),
 * before anything changes the mailbox's reason. */
void hal_boot_capture(void);

#endif

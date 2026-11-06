/*
 * sys.h: clocks, millisecond tick, independent watchdog and reset for the
 * STM32F1 platform. Same function names as platform/nuvoton-nuc1xx/sys.h
 * where they mean the same thing.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_SYS_H
#define GEMIFI_SYS_H

#include <stdint.h>
#include "stm32f1.h"

/* HCLK after sys_clock_init(): 72 MHz from HSE (8 MHz x 9), or 48 MHz from
 * HSI / 2 x 12 if the crystal never starts. APB1 = HCLK / 2, APB2 = HCLK,
 * so the APB1 timers (TIM2..4) also run at HCLK. */
#define HCLK_HZ_HSE 72000000u
#define HCLK_HZ_HSI 48000000u
extern uint32_t sys_hclk_hz;

/* If the board sets BOARD_SWJ_NOJTAG, first switches JTAG off (SWD stays
 * on) so PB3/PB4/PA15 are plain GPIOs.
 * Bring the clock tree to a known state from whatever the previous stage
 * (the vendor bootloader) left: back to HSI, RCC reset values, then HSE x 9
 * = 72 MHz with flash latency 2 + prefetch, APB1 / 2, USB = PLL / 1.5 =
 * 48 MHz. Returns 1 if HSE started, 0 if it fell back to HSI / 2 x 12 =
 * 48 MHz (USB = PLL / 1; HSI is only +-1 %, outside the USB spec, so USB may
 * or may not work then). */
int sys_clock_init(void);
int sys_hse_ok(void);

/* Millisecond counter built on SysTick, polled (no interrupt), exactly like
 * the NUC platform: it only advances while sys_tick_ms() is called at least
 * once per ms; time lost elsewhere (a flash erase stalls the CPU ~20-40 ms)
 * makes delays and timeouts longer, never shorter. Works with interrupts
 * disabled. */
void sys_tick_init(void);
/* Count milliseconds from the SysTick interrupt instead of by polling, so a
 * long main-loop pass doesn't lose time. Needs interrupts enabled. */
void sys_tick_irq_enable(void);
void sys_tick_irq_disable(void);  /* back to polling (interrupts off) */
uint32_t sys_tick_ms(void);
void sys_delay_ms(uint32_t ms);

/* RCC_CSR at the first call (the reset flags), then cleared (RMVF). */
uint32_t sys_reset_cause(void);

/* Independent watchdog: LSI ~40 kHz / 32, reload 0x4E2 = ~1 s, like the
 * stock firmware. Once started it cannot be stopped; sys_iwdg_kick() at
 * least every few hundred ms. Kicking an IWDG that isn't running is
 * harmless. */
void sys_iwdg_start(void);
static inline void sys_iwdg_kick(void) { IWDG_KR = IWDG_KEY_RELOAD; }

/* SYSRESETREQ. Never returns. Does not disconnect USB; callers do. */
__attribute__((noreturn)) void sys_reset(void);

/* The bootloader's hand-off: MSP = sp, then jump to pc (the app's reset
 * vector), as M-Audio's bootloader did. Never returns. */
__attribute__((noreturn)) void sys_jump(uint32_t sp, uint32_t pc);

/* ---- SRAM mailbox at 0x2000_4FF0 (protocol/sysex.md section 6) --------
 * The top 16 bytes of the 20 KB SRAM, above every stack top (_estack =
 * 0x2000_4F00, ld/app.ld and ld/boot.ld), so neither side's startup or
 * stack touches it. The bootloader writes it at every reset (below); an
 * app trusts it only when the gemifi bootloader is there (its marker,
 * gemifi_image.h): under M-Audio's bootloader it is random. */
typedef struct {
    uint32_t magic;                  /* 0 (the request is the update flag page) */
    uint32_t rst_flags;              /* RCC_CSR as the bootloader found it */
    uint32_t boot_count;
    uint32_t reason;
} gemifi_mailbox_t;

#define MAILBOX_ADDR       0x20004FF0u
#define MAILBOX            ((volatile gemifi_mailbox_t *)MAILBOX_ADDR)
/* reason values (sysex.md 6) */
#define MAILBOX_REASON_REBOOT    1u      /* app: REBOOT(bootloader) asked   */
#define MAILBOX_REASON_BOOT_LOOP 2u      /* bootloader: boot-loop guard     */
/* The bootloader stays in update mode once boot_count reaches this: that
 * many hand-offs of a gemifi image in a row without the app clearing it.
 * The app clears it after running for MAILBOX_RUN_MS. */
#define MAILBOX_BOOT_TRIES       3u
#define MAILBOX_RUN_MS           5000u
/* RCC_CSR flags after which SRAM (and so the mailbox) can't be trusted. */
#define RCC_CSR_COLD             (RCC_CSR_PORRSTF | RCC_CSR_LPWRRSTF)

/* Apps (src/hal.c): set up the hal_delay_us() cycle counter (DWT CYCCNT,
 * SysTick fallback) and hal_delay_ns(). Call once after sys_clock_init()
 * and sys_tick_init(): it keeps the HCLK MHz the delays count in. */
void hal_delay_calibrate(void);

/* Apps (src/hal.c): 1 if the gemifi bootloader below writes the SRAM
 * mailbox (from 0.1.2 on). */
int hal_boot_mailbox(void);
/* Apps (src/hal.c): note why the app started (hal_reset_reason,
 * hal_boot_loop) from RCC_CSR or the mailbox. Call once, early in main(). */
void hal_boot_capture(void);

#endif

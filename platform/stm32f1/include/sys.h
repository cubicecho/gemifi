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

/* Apps (src/hal.c): set up the hal_delay_us() cycle counter (DWT CYCCNT,
 * SysTick fallback). Call once after sys_tick_init(). */
void hal_delay_calibrate(void);

#endif

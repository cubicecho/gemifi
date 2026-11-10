/*
 * sys.c: clocks, SysTick millisecond counter, IWDG, reset (RM0008 7, 19).
 *
 * SPDX-License-Identifier: MIT
 */
#include "sys.h"
#include "board.h"

_Static_assert(BOARD_CRYSTAL_HZ / (BOARD_PLL_HSE_DIV2 ? 2u : 1u) * BOARD_PLL_MUL == HCLK_HZ_HSE &&
               BOARD_PLL_MUL >= 2 && BOARD_PLL_MUL <= 16,
               "the board's crystal and PLL settings must make 72 MHz (tools/builder/gemifi_board.py)");

uint32_t sys_hclk_hz = 8000000u;    /* HSI until sys_clock_init() */
static uint8_t hse_ok;

/* Bounded wait for (reg & mask) == want: roughly 100-200 ms at 8 MHz HSI
 * (HSE start-up is typically 2 ms). Kicks the IWDG in case the previous
 * stage started it. */
static int wait_bits(volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
    for (uint32_t i = 0; i < 150000u; i++) {
        if ((*reg & mask) == want)
            return 1;
        IWDG_KR = IWDG_KEY_RELOAD;
    }
    return 0;
}

int sys_clock_init(void)
{
#ifdef BOARD_SWJ_NOJTAG
    /* JTAG off, SWD on (AFIO_MAPR SWJ_CFG = 010): frees PB3/PB4/PA15 for the
     * board (Code 61: matrix decoder, LED strobe). SWJ_CFG is write-only. */
    RCC_APB2ENR |= RCC_APB2_AFIO;
    AFIO_MAPR = (AFIO_MAPR & ~AFIO_MAPR_SWJ_MASK) | AFIO_MAPR_SWJ_NOJTAG;
#endif
    /* 1. HSI on and selected: the previous stage may have left SYSCLK on
     *    the PLL, which can't be reconfigured while it is in use. */
    RCC_CR |= RCC_CR_HSION;
    wait_bits(&RCC_CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY);
    RCC_CFGR &= ~RCC_CFGR_SW_MASK;
    wait_bits(&RCC_CFGR, RCC_CFGR_SWS_MASK, RCC_CFGR_SWS_HSI);

    /* 2. RCC to reset values (like RCC_DeInit): prescalers /1, PLL off,
     *    HSE off, CSS off, clock interrupts off and cleared. */
    RCC_CFGR = 0;
    RCC_CR &= ~(RCC_CR_HSEON | RCC_CR_CSSON | RCC_CR_PLLON);
    wait_bits(&RCC_CR, RCC_CR_PLLRDY, 0);
    RCC_CR &= ~RCC_CR_HSEBYP;
    RCC_CIR = 0x009F0000u;          /* clear all ready/CSS flags, no IRQs */

    /* 3. HSE on (crystal, not bypass). RM0008 7.2.1: HSERDY within 6 cycles
     *    of the oscillator being stable; typical start-up is ~2 ms. */
    RCC_CR |= RCC_CR_HSEON;
    hse_ok = (uint8_t)wait_bits(&RCC_CR, RCC_CR_HSERDY, RCC_CR_HSERDY);

    /* 4. Flash: 2 wait states for 48 < SYSCLK <= 72 MHz, prefetch on
     *    (RM0008 3.3.3; prefetch must be on for latency > 0 to pay off).
     *    Set before raising the clock. 48 MHz needs 1, 2 is fine too. */
    FLASH_ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY(2);

    /* 5. PLL: HSE (or HSE / 2) x BOARD_PLL_MUL = 72 MHz (board profile;
     *    Code 61: 8 MHz x 9), USB = 72 / 1.5 = 48 MHz.
     *    Fallback HSI / 2 x 12 = 48 MHz, USB = 48 / 1. */
    uint32_t cfgr = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1 |
                    RCC_CFGR_ADCPRE_DIV6;
    if (hse_ok) {
        cfgr |= RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLMUL(BOARD_PLL_MUL) |
                (BOARD_PLL_HSE_DIV2 ? RCC_CFGR_PLLXTPRE : 0u);
    } else {
        RCC_CR &= ~RCC_CR_HSEON;
        cfgr |= RCC_CFGR_PLLMUL(12) | RCC_CFGR_USBPRE_DIV1;
    }
    RCC_CFGR = cfgr;
    RCC_CR |= RCC_CR_PLLON;
    if (!wait_bits(&RCC_CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY)) {
        sys_hclk_hz = 8000000u;     /* stay on HSI; no USB clock */
        return hse_ok;
    }
    RCC_CFGR = cfgr | RCC_CFGR_SW_PLL;
    wait_bits(&RCC_CFGR, RCC_CFGR_SWS_MASK, RCC_CFGR_SWS_PLL);
    sys_hclk_hz = hse_ok ? HCLK_HZ_HSE : HCLK_HZ_HSI;
    return hse_ok;
}

int sys_hse_ok(void)
{
    return hse_ok;
}

/* ---- polled SysTick millisecond counter (see sys.h) ------------------- */

static volatile uint32_t tick_ms;
static uint8_t tick_irq;

void SysTick_Handler(void)
{
    tick_ms++;
}

void sys_tick_irq_enable(void)
{
    tick_irq = 1;
    SYST_CSR |= SYST_CSR_TICKINT;
}

void sys_tick_irq_disable(void)
{
    SYST_CSR &= ~SYST_CSR_TICKINT;
    tick_irq = 0;
}

void sys_tick_init(void)
{
    SYST_CSR = 0;
    SYST_RVR = sys_hclk_hz / 1000u - 1u;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_CLKSOURCE;
}

__attribute__((noinline)) uint32_t sys_tick_ms(void)
{
    if (!tick_irq && (SYST_CSR & SYST_CSR_COUNTFLAG))
        tick_ms++;
    return tick_ms;
}

void sys_delay_ms(uint32_t ms)
{
    uint32_t t0 = sys_tick_ms();
    while (sys_tick_ms() - t0 < ms) {
    }
}

uint32_t sys_reset_cause(void)
{
    static uint32_t cause;
    static uint8_t have;
    if (!have) {
        cause = RCC_CSR;
        RCC_CSR |= RCC_CSR_RMVF;
        have = 1;
    }
    return cause;
}

/* ---- IWDG ------------------------------------------------------------- */

void sys_iwdg_start(void)
{
    /* RM0008 19.3: KR 0xCCCC starts it (and LSI); PR/RLR are writable after
     * 0x5555 once PVU/RVU are clear. Same values as the stock app. */
    IWDG_KR = IWDG_KEY_START;
    IWDG_KR = IWDG_KEY_ACCESS;
    for (uint32_t i = 0; (IWDG_SR & 3u) && i < 100000u; i++) {
    }
    IWDG_PR = 3u;                   /* /32 -> 1.25 kHz */
    IWDG_RLR = 0x4E2u;              /* 1250 -> ~1 s */
    for (uint32_t i = 0; (IWDG_SR & 3u) && i < 100000u; i++) {
    }
    IWDG_KR = IWDG_KEY_RELOAD;
}

/* ---- reset ------------------------------------------------------------ */

void sys_jump(uint32_t sp, uint32_t pc)
{
    __asm__ volatile (
        "msr msp, %0\n\t"
        "bx %1\n\t"
        :: "r" (sp), "r" (pc) : "memory");
    __builtin_unreachable();
}

void sys_reset(void)
{
    __asm__ volatile ("dsb" ::: "memory");
    SCB_AIRCR = SCB_AIRCR_VECTKEY | (SCB_AIRCR & SCB_AIRCR_PRIGROUP) | SCB_AIRCR_SYSRESETREQ;
    __asm__ volatile ("dsb" ::: "memory");
    for (;;) {
    }
}

/*
 * sys.c: clocks, REGWRPROT, SysTick millisecond counter, CPU-reset handoff.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sys.h"

void sys_unlock(void)
{
    /* TRM 5.2 REGWRPROT: write 0x59, 0x16, 0x88 back to back; bit 0 then
     * reads 1. Writing anything else re-locks, so only run the sequence while
     * locked (the stock LDROM leaves it unlocked, which we must tolerate). */
    while (!(GCR_REGWRPROT & REGWRPROT_DIS)) {
        GCR_REGWRPROT = 0x59;
        GCR_REGWRPROT = 0x16;
        GCR_REGWRPROT = 0x88;
    }
}

static void wait_status(uint32_t mask)
{
    /* Bounded wait: ~50 ms worst case even at 48 MHz. If the crystal never
     * becomes stable we carry on; the HCLK switch below then fails safely
     * (TRM 5.3 CLKSTATUS.CLK_SW_FAIL: a switch to an unstable clock is
     * refused and HCLK stays where it was). */
    for (uint32_t i = 0; i < 600000u; i++) {
        if ((CLK_CLKSTATUS & mask) == mask)
            return;
    }
}

void sys_clock_init(void)
{
    sys_unlock();

    /* 1. Make sure both the 12 MHz crystal and the 22 MHz RC run. */
    CLK_PWRCON |= PWRCON_XTL12M_EN | PWRCON_OSC22M_EN;
    wait_status(CLKSTATUS_XTL12M_STB | CLKSTATUS_OSC22M_STB);

    /* 2. Move HCLK off the PLL before touching PLLCON: the previous stage
     *    (stock LDROM, or our own bootloader) may have left HCLK on the PLL.
     *    TRM 5.3 CLKSEL0: "Before clock switching, the related clock sources
     *    (both pre-select and new-select) must be turned on" - both are. */
    CLK_CLKSEL0 = (CLK_CLKSEL0 & ~CLKSEL0_HCLK_S_MASK) | CLKSEL0_HCLK_S_HIRC;
    CLK_CLKDIV &= ~0xFFu;            /* HCLK_N = 0, USB_N = 0 */

    /* 3. Re-program the PLL from a known state: power it down, then set
     *    0xC22E (PD = 0, OE = 0, source = HXT) and wait for lock. */
    CLK_PLLCON = PLLCON_48MHZ_FROM_12MHZ | PLLCON_PD;
    CLK_PLLCON = PLLCON_48MHZ_FROM_12MHZ;
    wait_status(CLKSTATUS_PLL_STB);

    /* 4. HCLK = PLL (48 MHz). The USB clock is PLL / (USB_N + 1) = 48 MHz. */
    CLK_CLKSEL0 = (CLK_CLKSEL0 & ~CLKSEL0_HCLK_S_MASK) | CLKSEL0_HCLK_S_PLL;

    CLK_AHBCLK |= AHBCLK_ISP_EN;     /* reset value already has it; be sure */
}

/* ---- polled SysTick millisecond counter ------------------------------- */
/* SysTick reloads every 1 ms; each poll that sees COUNTFLAG (cleared by the
 * read) adds one. If more than 1 ms passes between polls (a flash erase
 * stalls the CPU for 20-40 ms) the extra ms are lost, so the counter runs
 * slow and every timeout or delay built on it errs on the long side. That
 * is fine for LEDs, debouncing and protocol timeouts, and costs far less
 * code than exact accounting (it matters in the 4 KB bootloader). */

static uint32_t tick_ms;

void sys_tick_init(void)
{
    SYST_RVR = HCLK_HZ / 1000u - 1u;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_CLKSOURCE;
}

__attribute__((noinline)) uint32_t sys_tick_ms(void)
{
    if (SYST_CSR & SYST_CSR_COUNTFLAG)
        tick_ms++;
    return tick_ms;
}

void sys_delay_ms(uint32_t ms)
{
    uint32_t t0 = sys_tick_ms();
    while (sys_tick_ms() - t0 < ms) {
    }
}

void sys_reset_to(int to_ldrom)
{
    sys_unlock();
    /* TRM 6.10 ISPCON.BS: selects LDROM (1) or APROM (0) for the next boot,
     * and keeps its value over a CPU reset (it is only reloaded from CONFIG0
     * CBS on other resets). We never set LDUEN/CFGUEN: mask them out of the
     * read-modify-write so a stray value can't be written back either. */
    uint32_t v = FMC_ISPCON & ~(ISPCON_FORBIDDEN | ISPCON_ISPFF | ISPCON_BS);
    if (to_ldrom)
        v |= ISPCON_BS;
    FMC_ISPCON = v;
    /* TRM 5.2 IPRSTC1.CPU_RST: resets the CPU core and FMC only (not
     * CHIP_RST, which would reload BS from CBS and come back to LDROM).
     * Same handoff as the stock LDROM (ldrom.md section 2). */
    GCR_IPRSTC1 |= IPRSTC1_CPU_RST;
    for (;;) {
    }
}

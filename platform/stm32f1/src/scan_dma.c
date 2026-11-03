/*
 * scan_dma.c: the key/button matrix scanned by TIM1 and DMA1, with no CPU
 * time per row (core/README.md "Hardware scan").
 *
 * TIM1 counts one row per period. Its events start DMA transfers:
 *
 *   update (t = 0)       DMA1 ch5: store the next row's select word
 *                        (decoder address + group lines, BSRR)
 *   CC1 (t = 0.25 us)    DMA1 ch2: columns back to inputs      (precharge)
 *   CC2 (t = settle)     DMA1 ch3: copy the column port (IDR) to the ring
 *   CC3 (settle + 0.25)  DMA1 ch6: columns driven to their idle (precharge)
 *                        level until the next select
 *
 * Precharge is the stock Code 61 firmware's trick (hw-map.md "Key and
 * button matrix"): a column a closed key pulled low only has the weak
 * pull-up (30-50 kOhm, datasheet table 35) to bring it back, which takes
 * microseconds; driving it high between rows does it at once, so the read
 * only waits for a closed key to pull the column down. The columns' idle
 * level is their ODR bit (it picks pull-up or pull-down for an input), so
 * the "driven" setting is push-pull output with the same ODR.
 *
 * The ring holds RING_FRAMES frames; the read channel's half and full
 * transfer interrupts hand each finished half to gm_scan_frames(). Row
 * select table, ring and the CRL/CRH words are all in RAM: during a flash
 * erase the CPU stalls but the DMA keeps scanning (the interrupt then runs
 * late and the frames it missed are counted as lost).
 *
 * Nothing else may reconfigure the column pins (CRL/CRH of that port) while
 * a precharge scan runs: the DMA writes the whole register. scan_dma_stop()
 * puts them back as inputs.
 *
 * The raw readings hold the whole port; the core compares only the column
 * bits (gm_scan_plan_t.in_mask).
 *
 * DMA1 channels used: 2, 3, 5, 6 (TIM1_CH1, CH2, UP, CH3; RM0008 table 78).
 * Channel 7 stays free for the rgb_serial USART2, channel 1 for the ADC,
 * channel 4 for USART1 (5-pin MIDI) or TIM4.
 *
 * SPDX-License-Identifier: MIT
 */
#include "scan_dma.h"
#include "stm32f1.h"
#include "sys.h"

#define CH_SEL 5u                   /* TIM1_UP  */
#define CH_IN  2u                   /* TIM1_CH1 */
#define CH_RD  3u                   /* TIM1_CH2 */
#define CH_PRE 6u                   /* TIM1_CH3 */
#define GAP_NS 250u                 /* between DMA steps within a row */
#define RING_FRAMES 16u             /* two halves of 8: one interrupt per half, which may
                                       then be up to 8 frames late (the scan tick shares
                                       its level) */

static uint32_t ring[RING_FRAMES * GM_SCAN_MAX_ROWS / 2u];  /* 16-bit readings */
static uint32_t sel_next[GM_SCAN_MAX_ROWS];                 /* row r + 1's select at r */
static uint32_t cr_in, cr_out;
static volatile uint32_t *cr_reg;   /* precharge: the columns' CRL/CRH, else 0 */
static uint32_t n_rows, row_ticks, mhz, frame_cycles, last_isr;
static uint8_t running;
volatile uint32_t scan_dma_isr_max_us, scan_dma_overruns, scan_dma_errors;

static int gpio_port(const volatile uint32_t *reg, uint32_t off)
{
    for (uint32_t port = 0; port < GPIO_PORTS; port++)
        if ((uintptr_t)reg == GPIO_BASE(port) + off)
            return (int)port;
    return -1;
}

static void dma_off(uint32_t ch)
{
    DMA1_CCR(ch) = 0;
    DMA1_IFCR = DMA_ISR_TCIF(ch) | DMA_ISR_HTIF(ch) | DMA_ISR_TEIF(ch);
}

static void dma_on(uint32_t ch, volatile const void *periph, const void *mem, uint32_t n, uint32_t ccr)
{
    dma_off(ch);
    DMA1_CPAR(ch) = (uint32_t)(uintptr_t)periph;
    DMA1_CMAR(ch) = (uint32_t)(uintptr_t)mem;
    DMA1_CNDTR(ch) = n;
    DMA1_CCR(ch) = ccr | DMA_CCR_CIRC | DMA_CCR_EN;
}

void scan_dma_stop(void)
{
    TIM_CR1(TIM1_BASE) = 0;
    TIM_DIER(TIM1_BASE) = 0;
    if (!running)
        return;
    NVIC_ICER(IRQ_DMA1_CH3 / 32u) = 1u << (IRQ_DMA1_CH3 % 32u);
    dma_off(CH_SEL);
    dma_off(CH_IN);
    dma_off(CH_RD);
    dma_off(CH_PRE);
    if (cr_reg)
        *cr_reg = cr_in;            /* columns back to inputs: TIM1 may have stopped mid-precharge */
    cr_reg = 0;
    running = 0;
}

int scan_dma_start(const gm_scan_plan_t *p)
{
    if (!p || p->n_rows < 2u || p->n_rows > GM_SCAN_MAX_ROWS || (p->n_rows & 1u))
        return 0;
    int in_port = gpio_port(p->in_reg, 0x08u);          /* IDR */
    int sel_port = gpio_port(p->sel_reg, 0x10u);        /* BSRR */
    if (in_port < 0 || sel_port < 0 || !p->in_mask || (p->in_mask >> 16))
        return 0;

    /* Timer ticks (TIM1 runs at HCLK: APB2 = HCLK / 1). */
    mhz = sys_hclk_hz / 1000000u;
    uint32_t row = (p->row_ns * mhz + 500u) / 1000u;
    uint32_t settle = (p->settle_ns * mhz + 500u) / 1000u;
    uint32_t gap = (GAP_NS * mhz + 999u) / 1000u;
    uint32_t pre = p->precharge != 0;
    if (row > 0x10000u || settle < (pre ? 2u * gap : 1u) || settle + gap * (pre ? 2u : 1u) > row)
        return 0;

    /* Precharge: the columns' CRL or CRH with them as inputs (pull-up /
     * pull-down, as configured) and as push-pull outputs. All columns must be
     * in the one register, and pulled (their ODR bit is their idle level). */
    volatile uint32_t *cr = 0;
    if (pre) {
        uint32_t lo = p->in_mask & 0xFFu, hi = p->in_mask >> 8;
        if (lo && hi)
            return 0;
        cr = lo ? &GPIO_CRL(in_port) : &GPIO_CRH(in_port);
        uint32_t m = lo ? lo : hi;
        cr_in = cr_out = *cr;
        for (uint32_t b = 0; b < 8u; b++) {
            if (!((m >> b) & 1u))
                continue;
            if (((cr_in >> (4u * b)) & 0xFu) != GPIO_CFG_PULL)
                return 0;
            cr_out = (cr_out & ~(0xFu << (4u * b))) | (0x3u << (4u * b));   /* out, 50 MHz */
        }
    }

    scan_dma_stop();
    cr_reg = cr;
    n_rows = p->n_rows;
    row_ticks = row;
    for (uint32_t r = 0; r < n_rows; r++)
        sel_next[r] = p->sel_word[(r + 1u) % n_rows];
    scan_dma_isr_max_us = scan_dma_overruns = scan_dma_errors = 0;

    RCC_AHBENR |= RCC_AHBENR_DMA1EN;
    RCC_APB2ENR |= RCC_APB2_TIM1;
    RCC_APB2RSTR |= RCC_APB2_TIM1;
    RCC_APB2RSTR &= ~RCC_APB2_TIM1;

    /* The read gets the top priority, so the sample instant doesn't move;
     * the stores come next. Same priority: the lower channel goes first. */
    dma_on(CH_SEL, p->sel_reg, sel_next, n_rows,
           DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_PSIZE32 | DMA_CCR_MSIZE32 | DMA_CCR_PL(2));
    dma_on(CH_RD, p->in_reg, ring, RING_FRAMES * n_rows,
           DMA_CCR_MINC | DMA_CCR_PSIZE32 | DMA_CCR_MSIZE16 | DMA_CCR_PL(3) |
           DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE);
    if (pre) {
        dma_on(CH_IN, cr, &cr_in, 1u, DMA_CCR_DIR | DMA_CCR_PSIZE32 | DMA_CCR_MSIZE32 | DMA_CCR_PL(2));
        dma_on(CH_PRE, cr, &cr_out, 1u, DMA_CCR_DIR | DMA_CCR_PSIZE32 | DMA_CCR_MSIZE32 | DMA_CCR_PL(2));
    }

    /* Period 0 reads row 0, selected here; its update event selects row 1
     * (sel_next[0]), and so on: ring slot k is always row k % n_rows. */
    *p->sel_reg = p->sel_word[0];
    frame_cycles = n_rows * row;            /* TIM1 ticks = HCLK cycles */
    last_isr = DWT_CYCCNT;
    __asm__ volatile ("" ::: "memory");     /* tables written before the DMA runs */
    TIM_PSC(TIM1_BASE) = 0;
    TIM_ARR(TIM1_BASE) = row - 1u;
    TIM_RCR(TIM1_BASE) = 0;                 /* an update every period */
    TIM_CCMR1(TIM1_BASE) = 0;               /* compare channels "frozen": events only, */
    TIM_CCMR2(TIM1_BASE) = 0;               /* no outputs */
    TIM_CCER(TIM1_BASE) = 0;
    TIM_CCR1(TIM1_BASE) = gap;
    TIM_CCR2(TIM1_BASE) = settle;
    /* Precharge as late as it can be: the columns are driven only for the
     * last gap of the row (and the first of the next, until CC1), which is
     * when a held key on either row has its column driven against it. */
    TIM_CCR3(TIM1_BASE) = row - gap;
    TIM_CR1(TIM1_BASE) = TIM_CR1_URS;
    TIM_EGR(TIM1_BASE) = TIM_EGR_UG;        /* load PSC/ARR; no DMA requests enabled yet */
    TIM_SR(TIM1_BASE) = 0;
    TIM_CNT(TIM1_BASE) = 0;
    TIM_DIER(TIM1_BASE) = TIM_DIER_UDE | TIM_DIER_CC2DE | (pre ? TIM_DIER_CC1DE | TIM_DIER_CC3DE : 0u);

    NVIC_IPR8(IRQ_DMA1_CH3) = 0x80;         /* the scan tick's level: below USB (0x40) */
    NVIC_ICPR(IRQ_DMA1_CH3 / 32u) = 1u << (IRQ_DMA1_CH3 % 32u);
    NVIC_ISER(IRQ_DMA1_CH3 / 32u) = 1u << (IRQ_DMA1_CH3 % 32u);
    running = 1;
    TIM_CR1(TIM1_BASE) = TIM_CR1_URS | TIM_CR1_CEN;
    return 1;
}

uint32_t scan_dma_frame_q4(void)
{
    return mhz ? n_rows * row_ticks * 16u / mhz : 0;
}

uint32_t scan_dma_frame_ns(void)
{
    return mhz ? n_rows * row_ticks * 1000u / mhz : 0;
}

/* Half or full ring: hand the half the DMA just finished to the core. The
 * half to read is the one the DMA is NOT writing now, whatever the flags
 * say, so a late interrupt still reads finished frames. */
void DMA1_Channel3_IRQHandler(void)
{
    uint32_t c0 = DWT_CYCCNT;
    uint32_t isr = DMA1_ISR;
    DMA1_IFCR = DMA_ISR_TCIF(CH_RD) | DMA_ISR_HTIF(CH_RD) | DMA_ISR_TEIF(CH_RD);
    if (isr & DMA_ISR_TEIF(CH_RD)) {
        scan_dma_errors++;                  /* the channel is off now: the scan stopped */
        return;
    }
    uint32_t total = RING_FRAMES * n_rows, half = total / 2u;
    uint32_t pos = total - DMA1_CNDTR(CH_RD);   /* next slot the DMA writes */
    uint32_t first = pos >= half ? 0u : half;   /* the finished half */
    uint32_t lost = 0;
    if ((isr & DMA_ISR_HTIF(CH_RD)) && (isr & DMA_ISR_TCIF(CH_RD))) {
        /* A half (or many: a flash erase stalls us for ms) went by unread.
         * Count them from the time since the last interrupt, so key timing
         * across the gap stays about right. */
        scan_dma_overruns++;
        uint32_t frames = frame_cycles ? (c0 - last_isr) / frame_cycles : 0;
        lost = frames > RING_FRAMES / 2u ? frames - RING_FRAMES / 2u : RING_FRAMES / 2u;
    }
    last_isr = c0;
    __asm__ volatile ("" ::: "memory");     /* the ring after the flags */
    gm_scan_frames((const uint16_t *)ring + first, RING_FRAMES / 2u, lost);
    uint32_t pos2 = total - DMA1_CNDTR(CH_RD);
    if ((pos2 >= half) == (first >= half))
        scan_dma_overruns++;                /* the DMA came round while we read */
    uint32_t us = (DWT_CYCCNT - c0) / mhz;
    if (us > scan_dma_isr_max_us)
        scan_dma_isr_max_us = us;
}

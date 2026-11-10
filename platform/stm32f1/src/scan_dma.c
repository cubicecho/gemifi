/*
 * scan_dma.c: the key/button matrix scanned by a timer and DMA1, with no CPU
 * time per row (core/README.md "Hardware scan").
 *
 * The timer (TIM1 on the Code 61; dma_map.h) counts one row per period. Its
 * events start DMA transfers (the Code 61's requests and channels):
 *
 *   select (t = 0)       update, ch5: store the next row's select word
 *                        (decoder address + group lines, BSRR)
 *   in (t = 0.25 us)     CC1, ch2: columns back to inputs      (precharge)
 *   read (t = settle)    CC2, ch3: copy the column port (IDR) to the ring
 *   pre (row - 0.25 us)  CC3, ch6: columns driven to their idle (precharge)
 *                        level until the next select
 *
 * hardware_scan.matrix.timer moves them to another timer or other compares
 * (the builder picks requests on free channels). The select may be a
 * compare instead of the update: then every step comes one tick later, and
 * the select of period 0 already happens in period 0 (so the table isn't
 * shifted by a row).
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
 * DMA1 channels: GM_HW_SCAN_*_CH (dma_map.h); the builder hands them out
 * (gemifi_device.py allocate_dma) and refuses a clash. The read channel's
 * interrupt hands the frames to the core.
 *
 * SPDX-License-Identifier: MIT
 */
#include "scan_dma.h"
#include "stm32f1.h"
#include "sys.h"
#include "dma_map.h"

#define TIM    TIM_BASE_N(GM_HW_SCAN_TIM)
#define CH_SEL GM_HW_SCAN_SEL_CH
#define CH_IN  GM_HW_SCAN_IN_CH     /* 0: no precharge */
#define CH_RD  GM_HW_SCAN_RD_CH
#define CH_PRE GM_HW_SCAN_PRE_CH    /* 0: no precharge */
#define HAS_PRE (CH_PRE != 0)
#define SHIFT  (GM_HW_SCAN_SEL != 0)    /* select on a compare: all steps a tick later */

_Static_assert(GM_HW_SCAN_TIM >= 1 && GM_HW_SCAN_TIM <= 4, "hardware_scan.matrix: TIM1..TIM4");
_Static_assert(CH_SEL && DMA1_TIM_CH(GM_HW_SCAN_TIM, GM_HW_SCAN_SEL) == CH_SEL, "select request/channel");
_Static_assert(GM_HW_SCAN_RD && DMA1_TIM_CH(GM_HW_SCAN_TIM, GM_HW_SCAN_RD) == CH_RD, "read request/channel");
_Static_assert(!HAS_PRE || (CH_IN && GM_HW_SCAN_IN && GM_HW_SCAN_PRE &&
                            DMA1_TIM_CH(GM_HW_SCAN_TIM, GM_HW_SCAN_IN) == CH_IN &&
                            DMA1_TIM_CH(GM_HW_SCAN_TIM, GM_HW_SCAN_PRE) == CH_PRE &&
                            CH_IN != CH_PRE && CH_IN != CH_SEL && CH_IN != CH_RD &&
                            CH_PRE != CH_SEL && CH_PRE != CH_RD), "precharge requests/channels");
_Static_assert(CH_SEL != CH_RD, "select and read on one channel");
#define GAP_NS 250u                 /* between DMA steps within a row */
#define RING_FRAMES 16u             /* two halves of 8: one interrupt per half, which may
                                       then be up to 8 frames late (the scan tick shares
                                       its level) */

/* Sized for this device's plan (the builder's GM_HW_SCAN_ROWS: the most
 * rows it can get), else for the largest. */
#ifdef GM_HW_SCAN_ROWS
#define SCAN_ROWS GM_HW_SCAN_ROWS
#else
#define SCAN_ROWS GM_SCAN_MAX_ROWS
#endif
_Static_assert(SCAN_ROWS >= 2u && SCAN_ROWS <= GM_SCAN_MAX_ROWS && !(SCAN_ROWS & 1u), "hardware scan rows");
static uint32_t ring[RING_FRAMES * SCAN_ROWS / 2u];  /* 16-bit readings */
static uint32_t sel_next[SCAN_ROWS];                 /* row r + 1's select at r */
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
    TIM_CR1(TIM) = 0;
    TIM_DIER(TIM) = 0;
    if (!running)
        return;
    NVIC_ICER(IRQ_DMA1_CH(CH_RD) / 32u) = 1u << (IRQ_DMA1_CH(CH_RD) % 32u);
    dma_off(CH_SEL);
    dma_off(CH_RD);
    if (HAS_PRE) {
        dma_off(CH_IN);
        dma_off(CH_PRE);
    }
    if (cr_reg)
        *cr_reg = cr_in;            /* columns back to inputs: the timer may have stopped mid-precharge */
    cr_reg = 0;
    running = 0;
}

int scan_dma_start(const gm_scan_plan_t *p)
{
    if (!p || p->n_rows < 2u || p->n_rows > SCAN_ROWS || (p->n_rows & 1u))
        return 0;
    int in_port = gpio_port(p->in_reg, 0x08u);          /* IDR */
    int sel_port = gpio_port(p->sel_reg, 0x10u);        /* BSRR */
    if (in_port < 0 || sel_port < 0 || !p->in_mask || (p->in_mask >> 16))
        return 0;

    /* Timer ticks (all of TIM1..4 run at HCLK: APB2 = HCLK, APB1 timers x 2). */
    mhz = sys_hclk_hz / 1000000u;
    uint32_t row = (p->row_ns * mhz + 500u) / 1000u;
    uint32_t settle = (p->settle_ns * mhz + 500u) / 1000u;
    uint32_t gap = (GAP_NS * mhz + 999u) / 1000u;
    uint32_t pre = p->precharge != 0;
    if ((pre && !HAS_PRE) || row > 0x10000u || gap < 1u + SHIFT || settle < (pre ? 2u * gap : 1u) ||
        settle + gap * (pre ? 2u : 1u) > row)
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
        sel_next[r] = p->sel_word[(r + (SHIFT ? 0u : 1u)) % n_rows];
    scan_dma_isr_max_us = scan_dma_overruns = scan_dma_errors = 0;

    RCC_AHBENR |= RCC_AHBENR_DMA1EN;
    tim_clock_reset(GM_HW_SCAN_TIM);

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

    /* Period 0 reads row 0, selected here; with the select on the update,
     * the update ending it selects row 1 (sel_next[0]), and so on. With the
     * select on a compare (SHIFT), period 0's own select stores row 0 again
     * (sel_next[0] = row 0). Either way ring slot k is row k % n_rows. */
    *p->sel_reg = p->sel_word[0];
    frame_cycles = n_rows * row;            /* timer ticks = HCLK cycles */
    last_isr = DWT_CYCCNT;
    __asm__ volatile ("" ::: "memory");     /* tables written before the DMA runs */
    TIM_PSC(TIM) = 0;
    TIM_ARR(TIM) = row - 1u;
    if (GM_HW_SCAN_TIM == 1)
        TIM_RCR(TIM) = 0;                   /* an update every period (TIM1 only) */
    TIM_CCMR1(TIM) = 0;                     /* compare channels "frozen": events only, */
    TIM_CCMR2(TIM) = 0;                     /* no outputs */
    TIM_CCER(TIM) = 0;
    if (SHIFT)
        TIM_CCR(TIM, GM_HW_SCAN_SEL) = SHIFT;
    TIM_CCR(TIM, GM_HW_SCAN_RD) = settle + SHIFT;
    if (pre) {
        TIM_CCR(TIM, GM_HW_SCAN_IN) = gap + SHIFT;
        /* Precharge as late as it can be: the columns are driven only for
         * the last gap of the row (and the first of the next, until "in"),
         * which is when a held key on either row has its column driven
         * against it. */
        TIM_CCR(TIM, GM_HW_SCAN_PRE) = row - gap + SHIFT;
    }
    TIM_CR1(TIM) = TIM_CR1_URS;
    TIM_EGR(TIM) = TIM_EGR_UG;              /* load PSC/ARR; no DMA requests enabled yet */
    TIM_SR(TIM) = 0;
    TIM_CNT(TIM) = 0;
    TIM_DIER(TIM) = TIM_DIER_DE(GM_HW_SCAN_SEL) | TIM_DIER_DE(GM_HW_SCAN_RD) |
                    (pre ? TIM_DIER_DE(GM_HW_SCAN_IN) | TIM_DIER_DE(GM_HW_SCAN_PRE) : 0u);

    NVIC_IPR8(IRQ_DMA1_CH(CH_RD)) = 0x80;   /* the scan tick's level: below USB (0x40) */
    NVIC_ICPR(IRQ_DMA1_CH(CH_RD) / 32u) = 1u << (IRQ_DMA1_CH(CH_RD) % 32u);
    NVIC_ISER(IRQ_DMA1_CH(CH_RD) / 32u) = 1u << (IRQ_DMA1_CH(CH_RD) % 32u);
    running = 1;
    TIM_CR1(TIM) = TIM_CR1_URS | TIM_CR1_CEN;
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
void DMA1_IRQ_HANDLER(GM_HW_SCAN_RD_CH)(void)
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

/*
 * adc_dma.c: the analog inputs and pads swept by a timer, ADC1 and DMA1,
 * with no CPU per reading (core/README.md "Hardware scan").
 *
 * The timer (TIM4 on the Code 61; dma_map.h) counts one mux step per period:
 *
 *   mux (t = 1 tick)       CC2, ch4: store step s's select word (BSRR) on
 *                          the muxes' shared select lines (Code 61: PC0-PC2)
 *   trigger (the settle)   OC4REF rises (PWM mode 2): ADC1's regular group
 *                          starts, every plan pin in turn (scan mode)
 *   each conversion        ch1: ADC1_DR to the ring
 *
 * hardware_scan.analog.timer moves it (the builder picks the mux compare):
 * TIM4 triggers by CC4 (mux on CC2 or CC3), TIM3 by TRGO = OC2REF (mux on
 * CC1, CC3 or CC4; no output is enabled), TIM2 by CC2 (mux on CC1 or CC4).
 * CC4 / CC2 are enabled outputs, which reach their pin (PB9 / PA1) only if
 * that pin is an alternate-function output: on the Code 61 PB9 is unused
 * and PA1 an analog input.
 *
 * A sweep is all the steps; the ring holds two, and the half / full
 * interrupts hand the finished one to gm_adc_sweep(). The period leaves the
 * conversions 2 us to spare, so no trigger comes while the ADC is busy (it
 * would be dropped and the readings would land one step off). The interrupt
 * still checks: at the end of a sweep the mux DMA must be about to write
 * step 0 again; if it isn't, both are restarted (adc_dma_resyncs).
 *
 * Every conversion samples for 41.5 ADC cycles (54 with the conversion, at
 * PCLK2 / 6: 4.5 us at 72 MHz), as the stock Code 61 firmware does: up to
 * 37 kOhm of source impedance for 1/4 LSB (datasheet table 48), with the
 * mux's on-resistance in series. TIM2..4 run at HCLK (APB1 / 2, timers x 2),
 * so a conversion is 324 timer ticks at any clock.
 *
 * DMA1 channels used: 1 (ADC1) and GM_HW_ADC_MUX_CH. Nothing else may use
 * ADC1 (hal_adc_read uses ADC2) or reconfigure the plan's pins while this
 * runs.
 *
 * SPDX-License-Identifier: MIT
 */
#include "adc_dma.h"
#include "stm32f1.h"
#include "sys.h"
#include "dma_map.h"

#define TIM    TIM_BASE_N(GM_HW_ADC_TIM)
#define CH_ADC 1u                   /* ADC1 */
#define CH_MUX GM_HW_ADC_MUX_CH
#if GM_HW_ADC_TIM == 4
#define TRIG_CC     4u              /* ADC1 started by TIM4_CC4 */
#define TRIG_CCMR1  0u
#define TRIG_CCMR2  TIM_CCMR2_OC4M_PWM2
#define TRIG_CCER   TIM_CCER_CC4E   /* PB9 isn't an AF output: no pin is driven */
#define TRIG_CR2    0u
#define TRIG_EXTSEL ADC_CR2_EXTSEL_TIM4_CC4
#elif GM_HW_ADC_TIM == 3
#define TRIG_CC     2u              /* by TIM3_TRGO = OC2REF: internal, no output */
#define TRIG_CCMR1  TIM_CCMR1_OC2M_PWM2
#define TRIG_CCMR2  0u
#define TRIG_CCER   0u
#define TRIG_CR2    TIM_CR2_MMS_OC2REF
#define TRIG_EXTSEL ADC_CR2_EXTSEL_TIM3_TRGO
#elif GM_HW_ADC_TIM == 2
#define TRIG_CC     2u              /* by TIM2_CC2 */
#define TRIG_CCMR1  TIM_CCMR1_OC2M_PWM2
#define TRIG_CCMR2  0u
#define TRIG_CCER   TIM_CCER_CC2E   /* PA1 is driven only if it is an AF output */
#define TRIG_CR2    0u
#define TRIG_EXTSEL ADC_CR2_EXTSEL_TIM2_CC2
#else
#error "hardware_scan.analog: TIM2, TIM3 or TIM4"
#endif
_Static_assert(GM_HW_ADC_MUX != 0 && GM_HW_ADC_MUX != TRIG_CC && CH_MUX != CH_ADC &&
               DMA1_TIM_CH(GM_HW_ADC_TIM, GM_HW_ADC_MUX) == CH_MUX, "analog mux request/channel");
#define CONV_TICKS (54u * 6u)       /* 41.5 + 12.5 ADC cycles, ADC clock = HCLK / 6 */
#define MARGIN_NS 2000u
#define LATENCY_NS 500u             /* DMA store -> trigger, trigger -> sampling */

/* Sized for this device's plan (the builder's GM_HW_ADC_PINS / _STEPS:
 * the most it can get), else for the largest. */
#ifdef GM_HW_ADC_PINS
#define ADC_PINS GM_HW_ADC_PINS
#define ADC_STEPS GM_HW_ADC_STEPS
#else
#define ADC_PINS GM_ADC_MAX_PINS
#define ADC_STEPS GM_ADC_MAX_STEPS
#endif
_Static_assert(ADC_PINS >= 1u && ADC_PINS <= GM_ADC_MAX_PINS && ADC_STEPS >= 1u &&
               ADC_STEPS <= GM_ADC_MAX_STEPS, "hardware analog scan sizes");
static uint32_t ring[(2u * ADC_STEPS * ADC_PINS + 1u) / 2u];   /* 16-bit readings */
static uint32_t steps[ADC_STEPS];
static const gm_adc_plan_t *plan;
static uint32_t n_cells, n_steps, ticks, ccr2, ccr4, done, mhz;
static uint8_t running;
volatile uint32_t adc_dma_isr_max_us, adc_dma_overruns, adc_dma_resyncs, adc_dma_errors;

/* Wait about this many HCLK cycles. A loop, not CYCCNT: some clones have no
 * working cycle counter (hal_delay_calibrate), and this must not hang. */
static void spin(uint32_t cycles)
{
    for (volatile uint32_t i = cycles / 4u; i; i--) {
    }
}

int adc_dma_running(void)
{
    return running;
}

static int adc_channel(uint16_t pin)
{
    if (pin <= 7u)
        return pin;                         /* PA0..PA7 */
    if (pin == 16u || pin == 17u)
        return pin - 16 + 8;                /* PB0, PB1 */
    if (pin >= 32u && pin <= 37u)
        return pin - 32 + 10;               /* PC0..PC5 */
    return -1;
}

static int is_bsrr(const volatile uint32_t *reg)
{
    for (uint32_t port = 0; port < GPIO_PORTS; port++)
        if ((uintptr_t)reg == GPIO_BASE(port) + 0x10u)
            return 1;
    return 0;
}

static void dma_off(uint32_t ch)
{
    DMA1_CCR(ch) = 0;
    DMA1_IFCR = DMA_ISR_TCIF(ch) | DMA_ISR_HTIF(ch) | DMA_ISR_TEIF(ch);
}

/* (Re)start the DMA channels and the timer from step 0. ADC1 is set up. */
static void run(void)
{
    TIM_CR1(TIM) = 0;
    TIM_DIER(TIM) = 0;
    dma_off(CH_ADC);
    dma_off(CH_MUX);
    /* A sequence of the old run may be in flight: let it end (the ADC
     * finishes it on its own) with its DMA requests off, so no stale
     * request reaches the new ring (a one-reading shift that the end-of-
     * sweep check couldn't see). Changing DMA alone starts no conversion. */
    ADC1_CR2 &= ~ADC_CR2_DMA;
    spin(plan->n_pins * CONV_TICKS + 2u * mhz);
    (void)ADC1_DR;
    ADC1_CR2 |= ADC_CR2_DMA;

    DMA1_CPAR(CH_ADC) = (uint32_t)(uintptr_t)&ADC1_DR;
    DMA1_CMAR(CH_ADC) = (uint32_t)(uintptr_t)ring;
    DMA1_CNDTR(CH_ADC) = 2u * n_cells;
    __asm__ volatile ("" ::: "memory");     /* steps[] written before the DMA runs */
    DMA1_CCR(CH_ADC) = DMA_CCR_MINC | DMA_CCR_PSIZE32 | DMA_CCR_MSIZE16 | DMA_CCR_PL(2) | DMA_CCR_CIRC |
                       DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE | DMA_CCR_EN;
    if (n_steps > 1u) {
        *plan->sel_reg = steps[0];          /* period 0's step, before its settle starts */
        DMA1_CPAR(CH_MUX) = (uint32_t)(uintptr_t)plan->sel_reg;
        DMA1_CMAR(CH_MUX) = (uint32_t)(uintptr_t)steps;
        DMA1_CNDTR(CH_MUX) = n_steps;
        DMA1_CCR(CH_MUX) = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_PSIZE32 | DMA_CCR_MSIZE32 | DMA_CCR_PL(1) |
                           DMA_CCR_CIRC | DMA_CCR_EN;
    }
    TIM_PSC(TIM) = 0;
    TIM_ARR(TIM) = ticks - 1u;
    TIM_CCR(TIM, GM_HW_ADC_MUX) = ccr2;
    TIM_CCR(TIM, TRIG_CC) = ccr4;
    TIM_CCMR1(TIM) = TRIG_CCMR1;            /* the mux compare: events only */
    TIM_CCMR2(TIM) = TRIG_CCMR2;
    TIM_CCER(TIM) = TRIG_CCER;
    if (TRIG_CR2)
        TIM_CR2(TIM) = TRIG_CR2;            /* TIM3: TRGO = OC2REF (reset: 0) */
    TIM_CR1(TIM) = TIM_CR1_URS;
    TIM_EGR(TIM) = TIM_EGR_UG;
    TIM_SR(TIM) = 0;
    TIM_CNT(TIM) = 0;
    TIM_DIER(TIM) = n_steps > 1u ? TIM_DIER_DE(GM_HW_ADC_MUX) : 0u;
    TIM_CR1(TIM) = TIM_CR1_URS | TIM_CR1_CEN;
}

void adc_dma_stop(void)
{
    TIM_CR1(TIM) = 0;
    if (!running)
        return;
    NVIC_ICER(IRQ_DMA1_CH1 / 32u) = 1u << (IRQ_DMA1_CH1 % 32u);
    TIM_DIER(TIM) = 0;
    dma_off(CH_ADC);
    dma_off(CH_MUX);
    ADC1_CR2 = 0;
    running = 0;
}

int adc_dma_start(const gm_adc_plan_t *p)
{
    if (!p || p->n_pins < 1u || p->n_pins > ADC_PINS || p->n_steps < 1u ||
        p->n_steps > ADC_STEPS || (p->n_steps > 1u && (!p->sel_reg || !is_bsrr(p->sel_reg))))
        return 0;
    int ch[ADC_PINS];
    for (uint32_t k = 0; k < p->n_pins; k++)
        if ((ch[k] = adc_channel(p->pin[k])) < 0)
            return 0;
    mhz = sys_hclk_hz / 1000000u;
    uint32_t c2 = 1u;
    uint32_t c4 = c2 + (p->settle_ns * mhz + 999u) / 1000u + (LATENCY_NS * mhz + 999u) / 1000u;
    uint32_t d = c4 + p->n_pins * CONV_TICKS + (LATENCY_NS * mhz + 999u) / 1000u;
    uint32_t t = d + (MARGIN_NS * mhz + 999u) / 1000u;
    /* Without muxes a step is only the conversions: keep a sweep at 50 us
     * or more, or the interrupt would come every few us. */
    uint32_t min = (50000u * mhz / 1000u + p->n_steps - 1u) / p->n_steps;
    if (t < min)
        t = min;
    if (t > 0x10000u)
        return 0;

    adc_dma_stop();
    plan = p;
    n_steps = p->n_steps;
    n_cells = p->n_steps * p->n_pins;
    ccr2 = c2;
    ccr4 = c4;
    done = d;
    ticks = t;
    for (uint32_t s = 0; s < n_steps; s++)
        steps[s] = p->step_word[s];
    adc_dma_isr_max_us = adc_dma_overruns = adc_dma_resyncs = adc_dma_errors = 0;

    /* The pins as analog inputs. */
    for (uint32_t k = 0; k < p->n_pins; k++) {
        uint32_t port = p->pin[k] >> 4, bit = p->pin[k] & 15u, sh = 4u * (bit & 7u);
        volatile uint32_t *cr = bit < 8u ? &GPIO_CRL(port) : &GPIO_CRH(port);
        uint32_t pm = irq_save();
        RCC_APB2ENR |= RCC_APB2_IOP(port);
        *cr = (*cr & ~(0xFu << sh)) | (GPIO_CFG_ANALOG << sh);
        irq_restore(pm);
    }

    /* ADC1: reset, power on, calibrate (RM0008 11.4), then the sequence:
     * scan mode, DMA, started by the timer (TRIG_EXTSEL). 41.5 cycles on
     * every channel. */
    RCC_AHBENR |= RCC_AHBENR_DMA1EN;
    RCC_APB2ENR |= RCC_APB2_ADC1;
    RCC_APB2RSTR |= RCC_APB2_ADC1;
    RCC_APB2RSTR &= ~RCC_APB2_ADC1;
    uint32_t s1 = 0, s2 = 0;
    for (uint32_t i = 0; i < 10u; i++)
        s2 |= 4u << (3u * i);
    for (uint32_t i = 0; i < 8u; i++)
        s1 |= 4u << (3u * i);
    ADC1_SMPR1 = s1;
    ADC1_SMPR2 = s2;
    uint32_t sq3 = 0, sq2 = 0;
    for (uint32_t k = 0; k < p->n_pins; k++) {
        if (k < 6u)
            sq3 |= (uint32_t)ch[k] << (5u * k);
        else
            sq2 |= (uint32_t)ch[k] << (5u * (k - 6u));
    }
    ADC1_SQR3 = sq3;
    ADC1_SQR2 = sq2;
    ADC1_SQR1 = (p->n_pins - 1u) << 20;     /* L: conversions in the sequence */
    ADC1_CR1 = ADC_CR1_SCAN;
    ADC1_CR2 = ADC_CR2_ADON;
    spin(5u * mhz);                         /* tSTAB, 1 us (a few) */
    ADC1_CR2 |= ADC_CR2_RSTCAL;
    for (uint32_t i = 0; (ADC1_CR2 & ADC_CR2_RSTCAL) && i < 100000u; i++) {
    }
    ADC1_CR2 |= ADC_CR2_CAL;
    for (uint32_t i = 0; (ADC1_CR2 & ADC_CR2_CAL) && i < 100000u; i++) {
    }
    /* Other bits change with ADON: no conversion starts (RM0008 11.12.3). */
    ADC1_CR2 = ADC_CR2_ADON | ADC_CR2_DMA | ADC_CR2_EXTTRIG | TRIG_EXTSEL;

    tim_clock_reset(GM_HW_ADC_TIM);
    NVIC_IPR8(IRQ_DMA1_CH1) = 0x80;         /* the scan tick's level: below USB (0x40) */
    NVIC_ICPR(IRQ_DMA1_CH1 / 32u) = 1u << (IRQ_DMA1_CH1 % 32u);
    NVIC_ISER(IRQ_DMA1_CH1 / 32u) = 1u << (IRQ_DMA1_CH1 % 32u);
    running = 1;
    run();
    return 1;
}

uint32_t adc_dma_sweep_us(void)
{
    return mhz ? n_steps * ticks / mhz : 0;
}

/* Half or full ring: the sweep the DMA just finished (the half it is not
 * writing now) to the core. */
void DMA1_Channel1_IRQHandler(void)
{
    uint32_t c0 = DWT_CYCCNT;
    uint32_t isr = DMA1_ISR;
    DMA1_IFCR = DMA_ISR_TCIF(CH_ADC) | DMA_ISR_HTIF(CH_ADC) | DMA_ISR_TEIF(CH_ADC);
    if (isr & DMA_ISR_TEIF(CH_ADC)) {
        adc_dma_errors++;                   /* the channel is off now: the sweeps stopped */
        return;
    }
    uint32_t cnt = TIM_CNT(TIM), mux_left = DMA1_CNDTR(CH_MUX);
    uint32_t total = 2u * n_cells, pos = total - DMA1_CNDTR(CH_ADC);
    uint32_t first = pos >= n_cells ? 0u : n_cells;
    if ((isr & DMA_ISR_HTIF(CH_ADC)) && (isr & DMA_ISR_TCIF(CH_ADC)))
        adc_dma_overruns++;
    /* In step: if we're still in the sweep's last period (no conversion since
     * the one that ended it, and after it in the period), the mux DMA has
     * stored the last step and is about to store step 0. */
    if (n_steps > 1u && (pos == 0u || pos == n_cells) && cnt >= done && mux_left != n_steps) {
        adc_dma_resyncs++;
        run();
        return;
    }
    __asm__ volatile ("" ::: "memory");     /* the ring after the flags */
    gm_adc_sweep((const uint16_t *)ring + first);
    uint32_t us = (DWT_CYCCNT - c0) / mhz;
    if (us > adc_dma_isr_max_us)
        adc_dma_isr_max_us = us;
}

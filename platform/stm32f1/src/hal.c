/*
 * hal.c: gm_hal.h for the STM32F1 (bare metal, USB-MIDI device).
 *
 * Pin numbers: port * 16 + bit (PA0 = 0, PB8 = 24, PC0 = 32), ports A..G.
 * Inputs with pull-up/down use CNF = 10 with the pull direction in ODR.
 * Outputs are push-pull (HAL_OUT_OD: open-drain), 10 MHz. Writes go through BSRR (or the ODR
 * bit-band alias), so the scan interrupt and the main loop can't clobber
 * each other's pins.
 * ADC1: PA0..PA7 = ch0..7, PB0/PB1 = ch8/9, PC0..PC5 = ch10..15.
 * No I2C yet (I2C hubs are unsupported here, as on the NUC).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_hal.h"
#include "gsx.h"
#include "stm32f1.h"
#include "sys.h"
#include "recovery.h"
#include "usbd.h"
#include "usb_midi.h"
#include "gemifi_image.h"

uint32_t hal_cycles(void)
{
    return DWT_CYCCNT;
}

uint32_t hal_millis(void)
{
    return sys_tick_ms();
}

/* ---- short delays: DWT cycle counter, SysTick fallback ----------------- */

static uint8_t have_dwt;

void hal_delay_calibrate(void)
{
    CORE_DEMCR |= CORE_DEMCR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
    uint32_t t0 = DWT_CYCCNT;
    for (volatile uint32_t i = 0; i < 16u; i++) {
    }
    have_dwt = DWT_CYCCNT != t0;    /* a clone without a working CYCCNT */
}

void hal_delay_us(uint32_t us)
{
    if (us >= 1000u) {
        sys_delay_ms((us + 999u) / 1000u);
        return;
    }
    uint32_t want = us * (sys_hclk_hz / 1000000u);
    if (have_dwt) {
        uint32_t t0 = DWT_CYCCNT;
        while (DWT_CYCCNT - t0 < want) {
        }
        return;
    }
    /* Count SysTick down (HCLK, reloads every 1 ms). Only CVR is read:
     * reading CSR would clear COUNTFLAG, which sys_tick_ms() counts with. */
    uint32_t reload = SYST_RVR + 1u, prev = SYST_CVR, done = 0;
    while (done < want) {
        uint32_t now = SYST_CVR;
        done += now <= prev ? prev - now : prev + reload - now;
        prev = now;
    }
}

/* ---- GPIO ------------------------------------------------------------- */

#define PIN_PORT(pin) ((uint32_t)(pin) >> 4)
#define PIN_BIT(pin)  ((uint32_t)(pin) & 15u)

volatile uint32_t *hal_pin_bsrr(uint16_t pin, uint32_t *bit)
{
    if (PIN_PORT(pin) >= GPIO_PORTS)
        return 0;
    *bit = PIN_BIT(pin);
    return &GPIO_BSRR(PIN_PORT(pin));
}

static void pin_cfg(uint16_t pin, uint32_t cfg)
{
    uint32_t port = PIN_PORT(pin), bit = PIN_BIT(pin);
    volatile uint32_t *cr = bit < 8u ? &GPIO_CRL(port) : &GPIO_CRH(port);
    uint32_t sh = 4u * (bit & 7u);
    uint32_t pm = irq_save();       /* CRL/CRH are read-modify-write */
    RCC_APB2ENR |= RCC_APB2_IOP(port);
    *cr = (*cr & ~(0xFu << sh)) | cfg << sh;
    irq_restore(pm);
}

void hal_pin_mode(uint16_t pin, uint8_t mode)
{
    if (PIN_PORT(pin) >= GPIO_PORTS)
        return;
    uint32_t port = PIN_PORT(pin), m = 1u << PIN_BIT(pin);
    RCC_APB2ENR |= RCC_APB2_IOP(port);
    switch (mode) {
    case HAL_OUT:
        pin_cfg(pin, GPIO_CFG_OUT_PP);
        break;
    case HAL_OUT_OD:
        pin_cfg(pin, GPIO_CFG_OUT_OD);
        break;
    case HAL_IN_PULLUP:
        GPIO_BSRR(port) = m;        /* ODR = 1: pull-up */
        pin_cfg(pin, GPIO_CFG_PULL);
        break;
    case HAL_IN_PULLDOWN:
        GPIO_BRR(port) = m;         /* ODR = 0: pull-down */
        pin_cfg(pin, GPIO_CFG_PULL);
        break;
    default:
        pin_cfg(pin, GPIO_CFG_FLOAT);
        break;
    }
}

int hal_pin_read(uint16_t pin)
{
    if (PIN_PORT(pin) >= GPIO_PORTS)
        return 0;
    return (int)((GPIO_IDR(PIN_PORT(pin)) >> PIN_BIT(pin)) & 1u);
}

void hal_pin_write(uint16_t pin, int level)
{
    if (PIN_PORT(pin) >= GPIO_PORTS)
        return;
    uint32_t m = 1u << PIN_BIT(pin);
    GPIO_BSRR(PIN_PORT(pin)) = level ? m : m << 16;
}

const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask)
{
    if (PIN_PORT(pin) >= GPIO_PORTS)
        return 0;
    *mask = 1u << PIN_BIT(pin);
    return &GPIO_IDR(PIN_PORT(pin));
}

/* The F1 has no per-pin data register, but the ODR bit-band alias is one:
 * a word per pin where writing 1/0 sets/clears just that ODR bit
 * atomically. */
volatile uint32_t *hal_pin_out_reg(uint16_t pin)
{
    if (PIN_PORT(pin) >= GPIO_PORTS)
        return 0;
    return &BITBAND_PERIPH(GPIO_BASE(PIN_PORT(pin)) + 0x0Cu, PIN_BIT(pin));
}

/* ---- ADC1 ------------------------------------------------------------- */

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

/* adc_dma.c (the hardware analog scan) owns ADC1 while it runs. A weak
 * stand-in for apps built without it. */
__attribute__((weak)) int adc_dma_running(void)
{
    return 0;
}

/* The hardware scans' stops (scan_dma.c, adc_dma.c), for hal_reboot; weak
 * stand-ins for apps built without them. */
__attribute__((weak)) void scan_dma_stop(void)
{
}

__attribute__((weak)) void adc_dma_stop(void)
{
}

int32_t hal_adc_read(uint16_t pin)
{
    int ch = adc_channel(pin);
    if (ch < 0 || adc_dma_running())
        return -1;
    static uint8_t ready;
    static uint16_t setup;
    if (!ready) {
        /* ADC clock = PCLK2 / 6 (sys.c: 12 MHz at 72 MHz, <= 14 MHz). */
        RCC_APB2ENR |= RCC_APB2_ADC1;
        RCC_APB2RSTR |= RCC_APB2_ADC1;
        RCC_APB2RSTR &= ~RCC_APB2_ADC1;
        /* 41.5 cycles sample time on every channel (field value 4), as the
         * stock Code 61 firmware uses: 4.5 us per conversion at 12 MHz. */
        uint32_t s1 = 0, s2 = 0;
        for (uint32_t i = 0; i < 10u; i++)
            s2 |= 4u << (3u * i);
        for (uint32_t i = 0; i < 8u; i++)
            s1 |= 4u << (3u * i);
        ADC1_SMPR1 = s1;
        ADC1_SMPR2 = s2;
        ADC1_SQR1 = 0;                       /* one conversion */
        ADC1_CR2 = ADC_CR2_ADON | ADC_CR2_EXTSEL_SWSTART | ADC_CR2_EXTTRIG;
        hal_delay_us(5);                    /* tSTAB (1 us) */
        /* RM0008 11.4: calibrate once after power-up, ADON = 1 for >= 2
         * ADC clock cycles first. */
        ADC1_CR2 |= ADC_CR2_RSTCAL;
        for (uint32_t i = 0; (ADC1_CR2 & ADC_CR2_RSTCAL) && i < 100000u; i++) {
        }
        ADC1_CR2 |= ADC_CR2_CAL;
        for (uint32_t i = 0; (ADC1_CR2 & ADC_CR2_CAL) && i < 100000u; i++) {
        }
        ready = 1;
    }
    if (!(setup & (1u << ch))) {
        pin_cfg(pin, GPIO_CFG_ANALOG);
        setup |= (uint16_t)(1u << ch);
    }
    ADC1_SQR3 = (uint32_t)ch;
    (void)ADC1_DR;                          /* clears a stale EOC */
    ADC1_CR2 |= ADC_CR2_SWSTART;
    for (uint32_t i = 0; !(ADC1_SR & ADC_SR_EOC); i++)
        if (i > 20000u)
            return -1;
    return (int32_t)(ADC1_DR & 0xFFFu);
}

int hal_i2c_write(uint8_t addr, const uint8_t *p, uint32_t n)
{
    (void)addr;
    (void)p;
    (void)n;
    return -1;
}

int hal_i2c_read(uint8_t addr, uint8_t *p, uint32_t n)
{
    (void)addr;
    (void)p;
    (void)n;
    return -1;
}

/* ---- MIDI over USB ---------------------------------------------------- */

/* MIDI out waits: a send finds the 64-byte packet buffer full and waits
 * for the host to take the last packet (or a protocol reply flushes). Sends
 * that took over 100 us, and the longest, for the scan report. */
volatile uint32_t hal_midi_waits, hal_midi_wait_max_us;

static void midi_timed(uint32_t c0)
{
    uint32_t us = (DWT_CYCCNT - c0) / (sys_hclk_hz / 1000000u);
    if (us > 100u) {
        hal_midi_waits++;
        if (us > hal_midi_wait_max_us)
            hal_midi_wait_max_us = us;
    }
}

static void midi_send(const uint8_t *m, uint32_t n)
{
    if (!n)
        return;
    if (m[0] == 0xF0) {
        midi_tx_sysex(m, n);
        return;
    }
    /* Channel messages: CIN = high nibble of the status byte. */
    midi_tx_event(m[0] >> 4, m[0], n > 1 ? m[1] : 0, n > 2 ? m[2] : 0);
}

void hal_midi_send(const uint8_t *m, uint32_t n)
{
    uint32_t c0 = DWT_CYCCNT;
    midi_send(m, n);
    midi_timed(c0);
}

static void midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    if (!n)
        return;
    if (m[0] == 0xF0) {
        midi_tx_sysex_cable(cable, m, n);
        return;
    }
    midi_tx_event((uint8_t)(cable << 4 | m[0] >> 4), m[0], n > 1 ? m[1] : 0, n > 2 ? m[2] : 0);
}

void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    uint32_t c0 = DWT_CYCCNT;
    midi_send_cable(cable, m, n);
    midi_timed(c0);
}

void hal_midi_poll(void)
{
    midi_tx_poll();
}

void hal_midi_flush(uint32_t timeout_ms)
{
    uint32_t c0 = DWT_CYCCNT;
    midi_tx_flush(timeout_ms);
    midi_timed(c0);
}

/* ---- identity, reboot, upload ---------------------------------------- */

void hal_uid(uint8_t uid[12])
{
    /* RM0008 30.2: 96-bit unique ID at 0x1FFFF7E8 (byte-readable). */
    const volatile uint8_t *u = (const volatile uint8_t *)UID_BASE;
    for (uint32_t i = 0; i < 12u; i++)
        uid[i] = u[i];
}

void hal_reboot(int to_bootloader)
{
    irq_disable_all();
    sys_tick_irq_disable();         /* the delay below polls SysTick */
    NVIC_ICER(0) = 0xFFFFFFFFu;     /* the next stage starts with no IRQs */
    NVIC_ICER(1) = 0xFFFFFFFFu;
    TIM_CR1(TIM2_BASE) = 0;
    scan_dma_stop();                /* the hardware scans, if running: columns back to inputs */
    adc_dma_stop();
    sys_iwdg_kick();
    /* Update mode = the vendor bootloader's request flag (recovery.h).
     * If the flash write fails we still reset: the app just comes back. */
    if (to_bootloader)
        recovery_request_update();
    usbd_disconnect();
    sys_delay_ms(10);
    sys_reset();
}

const hal_fw_ops_t *hal_fw_ops(void)
{
    return 0;                       /* the bootloader takes uploads */
}

/* rgb_serial (core/README.md): USART2 TX on PA2, sent by DMA1 channel 7 so
 * the main loop never waits on it. */
#define SERIAL_TX_PIN 2u                    /* PA2 */
static uint8_t serial_buf[16];
static uint8_t serial_open;

int hal_serial_open(uint16_t tx_pin, uint32_t baud)
{
    if (tx_pin != SERIAL_TX_PIN || baud == 0)
        return 0;
    RCC_APB2ENR |= RCC_APB2_IOP(0);
    RCC_APB1ENR |= RCC_APB1_USART2;
    RCC_AHBENR |= RCC_AHBENR_DMA1EN;
    GPIO_CRL(0) = (GPIO_CRL(0) & ~(0xFu << 8)) | (0xBu << 8);   /* AF push-pull, 50 MHz */
    uint32_t pclk1 = sys_hclk_hz / 2u;                          /* APB1 = HCLK / 2 */
    USART2_BRR = (pclk1 + baud / 2u) / baud;
    USART2_CR3 = USART_CR3_DMAT;
    USART2_CR1 = USART_CR1_UE | USART_CR1_TE;
    DMA1_CCR(7) = 0;
    DMA1_CPAR(7) = (uint32_t)&USART2_DR;
    serial_open = 1;
    return 1;
}

int hal_serial_write(uint16_t tx_pin, const uint8_t *b, uint32_t n)
{
    if (!serial_open || tx_pin != SERIAL_TX_PIN || n == 0 || n > sizeof serial_buf)
        return 0;
    if ((DMA1_CCR(7) & DMA_CCR_EN) && DMA1_CNDTR(7) != 0)
        return 0;                           /* the last write is still going */
    DMA1_CCR(7) = 0;
    DMA1_IFCR = 0xFu << 24;                 /* clear channel 7's flags */
    for (uint32_t i = 0; i < n; i++)
        serial_buf[i] = b[i];
    DMA1_CMAR(7) = (uint32_t)serial_buf;
    DMA1_CNDTR(7) = n;
    DMA1_CCR(7) = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_EN;
    return 1;
}

/* Which bootloader sits below us: gemifi's marks itself at 0x08000130
 * (gemifi_image.h); anything else is taken to be M-Audio's. */
uint16_t hal_extra_caps(void)
{
    const volatile uint32_t *m = (const volatile uint32_t *)GEMIFI_BOOT_MARK_ADDR;
    return *m == GEMIFI_BOOT_MAGIC ? GSX_CAP_GEMIFI_BOOT : 0;
}

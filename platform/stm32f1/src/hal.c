/*
 * hal.c: gm_hal.h for the STM32F1 (bare metal, USB-MIDI device).
 *
 * Pin numbers: port * 16 + bit (PA0 = 0, PB8 = 24, PC0 = 32), ports A..G.
 * Inputs with pull-up/down use CNF = 10 with the pull direction in ODR.
 * Outputs are push-pull (HAL_OUT_OD: open-drain), 10 MHz. Writes go through BSRR (or the ODR
 * bit-band alias), so the scan interrupt and the main loop can't clobber
 * each other's pins.
 * ADC2 (hal_adc_read; ADC1 is the hardware analog scan's): PA0..PA7 =
 * ch0..7, PB0/PB1 = ch8/9, PC0..PC5 = ch10..15.
 * No I2C yet (I2C hubs are unsupported here, as on the NUC).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_hal.h"
#include "gm_safety.h"
#include "gsx.h"
#include "stm32f1.h"
#include "sys.h"
#include "recovery.h"
#include "usbd.h"
#include "usb_midi.h"
#include "gemifi_image.h"
#include "dma_map.h"

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
static uint32_t cyc_per_us = 8u;    /* HCLK MHz: HSI until hal_delay_calibrate() */
static uint32_t cyc_per_ns_q12;     /* HCLK cycles per ns, << 12; 0 = no DWT */

void hal_delay_calibrate(void)
{
    CORE_DEMCR |= CORE_DEMCR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
    uint32_t t0 = DWT_CYCCNT;
    for (volatile uint32_t i = 0; i < 16u; i++) {
    }
    have_dwt = DWT_CYCCNT != t0;    /* a clone without a working CYCCNT */
    cyc_per_us = sys_hclk_hz / 1000000u;    /* once, not a division per delay */
    cyc_per_ns_q12 = have_dwt ? (sys_hclk_hz / 1000u * 4096u + 999999u) / 1000000u : 0u;
}

void hal_delay_us(uint32_t us)
{
    if (us >= 1000u) {
        sys_delay_ms((us + 999u) / 1000u);
        return;
    }
    uint32_t want = us * cyc_per_us;
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

/* DWT cycles (HCLK) in ns, rounded up: a multiply and a shift. 72 MHz:
 * 295 / 4096 a ns, 0.03 % long. */
uint32_t hal_ns_to_cycles(uint32_t ns)
{
    return (ns * cyc_per_ns_q12 + 4095u) >> 12;
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
    case HAL_ANALOG:
        pin_cfg(pin, GPIO_CFG_ANALOG);
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

/* ---- ADC2 ------------------------------------------------------------- */

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
    if (ch < 0)
        return -1;
    static uint8_t ready;
    static uint16_t setup;
    if (!ready) {
        /* ADC clock = PCLK2 / 6 (sys.c: 12 MHz at 72 MHz, <= 14 MHz). */
        RCC_APB2ENR |= RCC_APB2_ADC2;
        RCC_APB2RSTR |= RCC_APB2_ADC2;
        RCC_APB2RSTR &= ~RCC_APB2_ADC2;
        /* 41.5 cycles sample time on every channel (field value 4), as the
         * stock Code 61 firmware uses: 4.5 us per conversion at 12 MHz. */
        uint32_t s1 = 0, s2 = 0;
        for (uint32_t i = 0; i < 10u; i++)
            s2 |= 4u << (3u * i);
        for (uint32_t i = 0; i < 8u; i++)
            s1 |= 4u << (3u * i);
        ADC2_SMPR1 = s1;
        ADC2_SMPR2 = s2;
        ADC2_SQR1 = 0;                       /* one conversion */
        ADC2_CR2 = ADC_CR2_ADON | ADC_CR2_EXTSEL_SWSTART | ADC_CR2_EXTTRIG;
        hal_delay_us(5);                    /* tSTAB (1 us) */
        /* RM0008 11.4: calibrate once after power-up, ADON = 1 for >= 2
         * ADC clock cycles first. */
        ADC2_CR2 |= ADC_CR2_RSTCAL;
        for (uint32_t i = 0; (ADC2_CR2 & ADC_CR2_RSTCAL) && i < 100000u; i++) {
        }
        ADC2_CR2 |= ADC_CR2_CAL;
        for (uint32_t i = 0; (ADC2_CR2 & ADC_CR2_CAL) && i < 100000u; i++) {
        }
        ready = 1;
    }
    if (!(setup & (1u << ch))) {
        pin_cfg(pin, GPIO_CFG_ANALOG);
        setup |= (uint16_t)(1u << ch);
    }
    ADC2_SQR3 = (uint32_t)ch;
    (void)ADC2_DR;                          /* clears a stale EOC */
    ADC2_CR2 |= ADC_CR2_SWSTART;
    for (uint32_t i = 0; !(ADC2_SR & ADC_SR_EOC); i++)
        if (i > 20000u)
            return -1;
    return (int32_t)(ADC2_DR & 0xFFFu);
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

/* MIDI out waits: a send finds usb_midi.c's packet ring full and waits for
 * the host to take a packet (or a reboot flushes). Sends that took
 * over 100 us, and the longest, for the scan report. With no host reading
 * there are none: usb_midi.c then drops sends at once. */
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
    TIM_CR1(TIM_BASE_N(GM_SCAN_TICK_TIM)) = 0;   /* the scan tick */
    scan_dma_stop();                /* the hardware scans, if running: columns back to inputs */
    adc_dma_stop();
    sys_iwdg_kick();
    /* Update mode = the vendor bootloader's request flag (recovery.h).
     * If the flash write fails we still reset: the app just comes back. */
    if (to_bootloader) {
        recovery_request_update();
        if (hal_boot_mailbox())
            MAILBOX->reason = MAILBOX_REASON_REBOOT;
    }
    usbd_disconnect();
    sys_delay_ms(10);
    sys_reset();
}

const hal_fw_ops_t *hal_fw_ops(void)
{
    return 0;                       /* the bootloader takes uploads */
}

/* rgb_serial (core/README.md): USART2 TX on PA2, in the background so the
 * main loop never waits on it: by DMA1 channel 7 (HAL_UART_DMA, the
 * default), or from the USART2 interrupt, which frees the channel. */
#define SERIAL_TX_PIN 2u                    /* PA2 */
static uint8_t serial_buf[16];
static uint8_t serial_open, serial_dma;
static volatile uint8_t serial_at, serial_n;   /* interrupt mode: next byte, bytes */
/* HAL_UART_RX: bytes received on PA3, by the USART2 interrupt. */
static volatile uint8_t serial_rx[16];
static volatile uint8_t serial_rx_in, serial_rx_out;

void USART2_IRQHandler(void)
{
    uint32_t sr = USART2_SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t b = (uint8_t)USART2_DR;     /* SR then DR clears RXNE and the error flags */
        uint8_t in = serial_rx_in;
        if ((uint8_t)(in - serial_rx_out) < sizeof serial_rx) {     /* full: dropped */
            serial_rx[in % sizeof serial_rx] = b;
            serial_rx_in = (uint8_t)(in + 1u);
        }
    }
    if (!(sr & USART_SR_TXE) || !(USART2_CR1 & USART_CR1_TXEIE))
        return;
    uint8_t i = serial_at;
    if (i < serial_n) {
        USART2_DR = serial_buf[i];
        serial_at = (uint8_t)(i + 1u);
    } else {
        USART2_CR1 &= ~USART_CR1_TXEIE;
    }
}

int hal_serial_open(uint16_t tx_pin, uint32_t baud, uint32_t flags)
{
    if (tx_pin != SERIAL_TX_PIN || baud == 0)
        return 0;
    serial_dma = (flags & HAL_UART_DMA) != 0;
    RCC_APB2ENR |= RCC_APB2_IOP(0);
    RCC_APB1ENR |= RCC_APB1_USART2;
    GPIO_CRL(0) = (GPIO_CRL(0) & ~(0xFu << 8)) | (0xBu << 8);   /* AF push-pull, 50 MHz */
    uint32_t pclk1 = sys_hclk_hz / 2u;                          /* APB1 = HCLK / 2 */
    USART2_BRR = (pclk1 + baud / 2u) / baud;
    if (serial_dma) {
        RCC_AHBENR |= RCC_AHBENR_DMA1EN;
        USART2_CR3 = USART_CR3_DMAT;
        DMA1_CCR(7) = 0;
        DMA1_CPAR(7) = (uint32_t)&USART2_DR;
    } else {
        USART2_CR3 = 0;
        serial_at = serial_n = 0;
    }
    uint32_t cr1 = USART_CR1_UE | USART_CR1_TE;
    if (flags & HAL_UART_RX) {
        GPIO_CRL(0) = (GPIO_CRL(0) & ~(0xFu << 12)) | (0x8u << 12);   /* PA3: input, pull-up */
        GPIO_BSRR(0) = 1u << 3;
        serial_rx_in = serial_rx_out = 0;
        cr1 |= USART_CR1_RE | USART_CR1_RXNEIE;
    }
    if (!serial_dma || (flags & HAL_UART_RX)) {
        NVIC_IPR8(IRQ_USART2) = 0x40;       /* a byte per 87 us at 115200: below USART1, with USB */
        NVIC_ICPR(IRQ_USART2 / 32u) = 1u << (IRQ_USART2 % 32u);
        NVIC_ISER(IRQ_USART2 / 32u) = 1u << (IRQ_USART2 % 32u);
    }
    USART2_CR1 = cr1;
    serial_open = 1;
    return 1;
}

int hal_serial_read(uint16_t tx_pin)
{
    uint8_t out = serial_rx_out;
    if (tx_pin != SERIAL_TX_PIN || out == serial_rx_in)
        return -1;
    int b = serial_rx[out % sizeof serial_rx];
    serial_rx_out = (uint8_t)(out + 1u);
    return b;
}

int hal_serial_write(uint16_t tx_pin, const uint8_t *b, uint32_t n)
{
    if (!serial_open || tx_pin != SERIAL_TX_PIN || n == 0 || n > sizeof serial_buf)
        return 0;
    if (!serial_dma) {
        if (serial_at < serial_n)
            return 0;                       /* the last write is still going */
        for (uint32_t i = 0; i < n; i++)
            serial_buf[i] = b[i];
        serial_at = 0;
        serial_n = (uint8_t)n;
        USART2_CR1 |= USART_CR1_TXEIE;
        return 1;
    }
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

/* 5-pin DIN MIDI (din_midi:, core/src/din.c): USART1, TX on PA9, RX on
 * PA10, through rings. Two ways to move the bytes:
 *  - interrupt (the default): the USART1 interrupt, highest priority (0x00):
 *    at 31250 baud a byte lasts 320 us, and the handler is a few dozen
 *    cycles;
 *  - HAL_UART_DMA (din_midi dma: true): DMA1 channel 4 sends each run of the
 *    TX ring (its transfer-complete interrupt starts the next), channel 5
 *    fills the RX ring in circular mode. The hardware scans must then be on
 *    other channels (hardware_scan.*.timer): the builder finds them or
 *    refuses. */
#define DIN_TX_PIN 9u                       /* PA9 */
#define DIN_RX_PIN 10u                      /* PA10 */
#define DIN_TX_SIZE 512u                    /* powers of two */
#define DIN_RX_SIZE 256u
#define CH_DIN_TX 4u                        /* USART1_TX */
#define CH_DIN_RX 5u                        /* USART1_RX */
static uint8_t din_tx[DIN_TX_SIZE], din_rx[DIN_RX_SIZE];
static volatile uint32_t din_tx_head, din_tx_tail, din_rx_head, din_rx_tail;
static volatile uint32_t din_tx_run;        /* DMA: bytes of the run going out, 0 = idle */
static uint8_t din_open, din_dma, din_rx_on;
volatile uint32_t hal_din_overruns;         /* RX bytes lost: UART overrun or a full ring */
volatile uint32_t hal_din_rx_errors;        /* RX bytes dropped: framing or noise error */

void USART1_IRQHandler(void)
{
    uint32_t sr = USART1_SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t b = (uint8_t)USART1_DR;     /* reading DR clears RXNE and ORE */
        if (sr & USART_SR_ORE)
            hal_din_overruns++;
        /* A framing or noise error is a garbled byte (noise on an open MIDI
         * In, a cable plugged in mid-byte): drop it rather than pass it on. */
        if ((sr & USART_SR_RXNE) && (sr & (USART_SR_FE | USART_SR_NE))) {
            hal_din_rx_errors++;
        } else if (sr & USART_SR_RXNE) {
            uint32_t h = din_rx_head;
            if (h - din_rx_tail < DIN_RX_SIZE) {
                din_rx[h % DIN_RX_SIZE] = b;
                din_rx_head = h + 1u;
            } else {
                hal_din_overruns++;
            }
        }
    }
    if ((USART1_CR1 & USART_CR1_TXEIE) && (sr & USART_SR_TXE)) {
        uint32_t t = din_tx_tail;
        if (t != din_tx_head) {
            USART1_DR = din_tx[t % DIN_TX_SIZE];
            din_tx_tail = t + 1u;
        } else {
            USART1_CR1 &= ~USART_CR1_TXEIE;
        }
    }
}

/* DMA: send the next run of the TX ring (up to its end), if idle. Called
 * with interrupts off, or from the channel's interrupt. */
static void din_dma_next(void)
{
    if (din_tx_run)
        return;
    uint32_t t = din_tx_tail, n = din_tx_head - t, at = t % DIN_TX_SIZE;
    if (n == 0)
        return;
    if (n > DIN_TX_SIZE - at)
        n = DIN_TX_SIZE - at;
    din_tx_run = n;
    DMA1_CCR(CH_DIN_TX) = 0;
    DMA1_IFCR = 0xFu << (4u * (CH_DIN_TX - 1u));
    DMA1_CMAR(CH_DIN_TX) = (uint32_t)(din_tx + at);
    DMA1_CNDTR(CH_DIN_TX) = n;
    DMA1_CCR(CH_DIN_TX) = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_TCIE | DMA_CCR_EN;
}

/* Channel 4's interrupt is the matrix scan's when its read is there (the
 * builder then refuses din_midi dma: true). */
#if GM_HW_SCAN_RD_CH != CH_DIN_TX
void DMA1_Channel4_IRQHandler(void)
{
    DMA1_IFCR = 0xFu << (4u * (CH_DIN_TX - 1u));
    din_tx_tail += din_tx_run;
    din_tx_run = 0;
    din_dma_next();
}
#elif defined(GM_DIN_DMA) && GM_DIN_DMA
#error "din_midi dma: true needs DMA1 channel 4, which the matrix scan's read uses"
#endif

int hal_din_open(uint16_t tx_pin, uint16_t rx_pin, uint32_t baud, uint32_t flags)
{
    if (tx_pin != DIN_TX_PIN || (rx_pin != DIN_RX_PIN && rx_pin != 0xFFFFu) || baud == 0)
        return 0;
    din_dma = (flags & HAL_UART_DMA) != 0;
    din_rx_on = rx_pin == DIN_RX_PIN;
    RCC_APB2ENR |= RCC_APB2_IOP(0) | RCC_APB2_USART1;
    GPIO_CRH(0) = (GPIO_CRH(0) & ~(0xFu << 4)) | (0xBu << 4);     /* PA9: AF push-pull, 50 MHz */
    uint32_t cr1 = USART_CR1_UE | USART_CR1_TE;
    if (din_rx_on) {
        GPIO_BSRR(0) = 1u << 10;                                    /* pull-up */
        GPIO_CRH(0) = (GPIO_CRH(0) & ~(0xFu << 8)) | (0x8u << 8);  /* PA10: input, pull-up/down */
        cr1 |= USART_CR1_RE;
    }
    USART1_BRR = (sys_hclk_hz + baud / 2u) / baud;                  /* APB2 = HCLK */
    if (din_dma) {
        RCC_AHBENR |= RCC_AHBENR_DMA1EN;
        DMA1_CCR(CH_DIN_TX) = 0;
        DMA1_CPAR(CH_DIN_TX) = (uint32_t)&USART1_DR;
        USART1_CR3 = USART_CR3_DMAT | (din_rx_on ? USART_CR3_DMAR : 0u);
        if (din_rx_on) {
            DMA1_CCR(CH_DIN_RX) = 0;
            DMA1_CPAR(CH_DIN_RX) = (uint32_t)&USART1_DR;
            DMA1_CMAR(CH_DIN_RX) = (uint32_t)din_rx;
            DMA1_CNDTR(CH_DIN_RX) = DIN_RX_SIZE;
            DMA1_CCR(CH_DIN_RX) = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_EN;
        }
        NVIC_IPR8(IRQ_DMA1_CH4) = 0x40;
        NVIC_ICPR(IRQ_DMA1_CH4 / 32u) = 1u << (IRQ_DMA1_CH4 % 32u);
        NVIC_ISER(IRQ_DMA1_CH4 / 32u) = 1u << (IRQ_DMA1_CH4 % 32u);
    } else {
        USART1_CR3 = 0;
        if (din_rx_on)
            cr1 |= USART_CR1_RXNEIE;
        NVIC_IPR8(IRQ_USART1) = 0x00;
        NVIC_ICPR(IRQ_USART1 / 32u) = 1u << (IRQ_USART1 % 32u);
        NVIC_ISER(IRQ_USART1 / 32u) = 1u << (IRQ_USART1 % 32u);
    }
    USART1_CR1 = cr1;
    din_open = 1;
    return 1;
}

int hal_din_write(const uint8_t *b, uint32_t n)
{
    if (!din_open)
        return 0;
    uint32_t h = din_tx_head;
    if (DIN_TX_SIZE - (h - din_tx_tail) < n)
        return 0;
    for (uint32_t i = 0; i < n; i++)
        din_tx[(h + i) % DIN_TX_SIZE] = b[i];
    din_tx_head = h + n;
    if (din_dma) {
        uint32_t pm = irq_save();
        din_dma_next();
        irq_restore(pm);
    } else {
        USART1_CR1 |= USART_CR1_TXEIE;      /* the handler takes it from here */
    }
    return 1;
}

uint32_t hal_din_tx_queued(void)
{
    uint32_t pm = irq_save();               /* the DMA interrupt moves tail and run */
    uint32_t n = din_tx_head - din_tx_tail;
    if (din_tx_run)
        n -= din_tx_run - DMA1_CNDTR(CH_DIN_TX);    /* the part of the run already sent */
    irq_restore(pm);
    return n;
}

int hal_din_read(void)
{
    if (din_dma) {
        if (!din_rx_on)
            return -1;
        /* the DMA's write position; a full lap unread would be lost unseen */
        uint32_t at = (DIN_RX_SIZE - DMA1_CNDTR(CH_DIN_RX)) % DIN_RX_SIZE;   /* CNDTR 0: reloading */
        uint32_t t = din_rx_tail % DIN_RX_SIZE;
        if (t == at)
            return -1;
        int b = din_rx[t];
        din_rx_tail = (t + 1u) % DIN_RX_SIZE;
        return b;
    }
    uint32_t t = din_rx_tail;
    if (t == din_rx_head)
        return -1;
    int b = din_rx[t % DIN_RX_SIZE];
    din_rx_tail = t + 1u;
    return b;
}

void hal_midi_send_usb(const uint8_t ev[4])
{
    uint32_t c0 = DWT_CYCCNT;
    midi_tx_event(ev[0], ev[1], ev[2], ev[3]);
    midi_timed(c0);
}

/* Which bootloader sits below us: gemifi's marks itself at 0x08000130
 * (gemifi_image.h); anything else is taken to be M-Audio's. */
uint16_t hal_extra_caps(void)
{
    const volatile uint32_t *m = (const volatile uint32_t *)GEMIFI_BOOT_MARK_ADDR;
    return *m == GEMIFI_BOOT_MAGIC ? GSX_CAP_GEMIFI_BOOT : 0;
}

/* The gemifi bootloader writes the SRAM mailbox (sys.h) from version 0.1.2
 * on; under M-Audio's, or an older gemifi one, it is random. */
int hal_boot_mailbox(void)
{
    const volatile struct gemifi_boot_mark *m = (const volatile struct gemifi_boot_mark *)GEMIFI_BOOT_MARK_ADDR;
    return m->magic == GEMIFI_BOOT_MAGIC && m->boot_version >= GEMIFI_VERSION(0, 1, 2);
}

/* ---- why the app started (diag.reset_reason, diag.boot_loop; sysex.md 4.10) ----
 * Under the gemifi bootloader RCC_CSR is already cleared: the flags are in
 * the mailbox (sysex.md 6). Otherwise RCC_CSR as the app found it
 * (sys_reset_cause). The F1 reports a brown-out as a power-on reset. */
static uint8_t rst_reason, boot_loop;

void hal_boot_capture(void)
{
    uint32_t f = sys_reset_cause();
    if (hal_boot_mailbox()) {
        f = MAILBOX->rst_flags;
        boot_loop = MAILBOX->reason == MAILBOX_REASON_BOOT_LOOP;
        MAILBOX->reason = 0;
    }
    rst_reason = (uint8_t)(f & RCC_CSR_PORRSTF ? GM_RESET_POWER_ON :
                           f & RCC_CSR_LPWRRSTF ? GM_RESET_LOW_POWER :
                           f & (RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF) ? GM_RESET_WATCHDOG :
                           f & RCC_CSR_SFTRSTF ? GM_RESET_SOFTWARE :
                           f & RCC_CSR_PINRSTF ? GM_RESET_PIN : GM_RESET_UNKNOWN);
}

uint32_t hal_reset_reason(void)
{
    return rst_reason;
}

int hal_boot_loop(void)
{
    return boot_loop;
}

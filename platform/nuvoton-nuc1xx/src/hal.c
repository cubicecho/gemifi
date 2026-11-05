/*
 * hal.c: gm_hal.h for the NUC1xx (bare metal, USB-MIDI device).
 *
 * Pin numbers: port * 16 + bit (PA0 = 0, PB12 = 28, PC7 = 39).
 * The NUC120 has no internal pull resistors; HAL_IN_PULLUP uses the
 * quasi-bidirectional mode's weak pull-up, HAL_IN_PULLDOWN is a plain input.
 * ADC: PA0..PA7 = ADC0..7. No I2C yet (I2C hubs are unsupported here).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_hal.h"
#include "gsx.h"
#include "nuc1xx.h"
#include "sys.h"
#include "fmc.h"
#include "crc32.h"
#include "usbd.h"
#include "usb_midi.h"

/* Bit access: one 32-bit register per pin (TRM 5.6, GPIOx_PDIOn). Reads
 * give the pin level, writes set DOUT for that pin only (no read-modify-
 * write, so the scan interrupt and the main loop can't clobber each other). */
#define GPIO_PDIO(pin) REG32(GPIO_BASE + 0x200u + 0x40u * ((pin) >> 4) + 4u * ((pin) & 15u))

uint32_t hal_irq_save(void)
{
    uint32_t pm;
    __asm__ volatile ("mrs %0, primask\n\tcpsid i" : "=r"(pm) :: "memory");
    return pm;
}

void hal_irq_restore(uint32_t state)
{
    __asm__ volatile ("msr primask, %0" :: "r"(state) : "memory");
}

uint32_t hal_millis(void)
{
    return sys_tick_ms();
}

/* Short waits (the key-matrix settle, 1 us per row, in the scan interrupt)
 * can't afford the SysTick loop's setup (about 2.5 us), so they spin a
 * counted loop, calibrated once by hal_delay_calibrate() against SysTick. */
static uint32_t spins_per_16us;

__attribute__((noinline)) static void spin(uint32_t n)
{
    while (n--)
        __asm__ volatile ("");
}

void hal_delay_calibrate(void)
{
    uint32_t t0 = SYST_CVR;
    spin(1024u);
    uint32_t t1 = SYST_CVR;
    uint32_t cyc = t0 >= t1 ? t0 - t1 : t0 + SYST_RVR + 1u - t1;
    if (cyc)
        spins_per_16us = 1024u * 16u * (HCLK_HZ / 1000000u) / cyc;
}

void hal_delay_us(uint32_t us)
{
    if (us < 16u && spins_per_16us) {
        spin((us * spins_per_16us + 15u) / 16u);
        return;
    }
    if (us >= 1000u) {
        sys_delay_ms((us + 999u) / 1000u);
        return;
    }
    /* Count SysTick down (HCLK, reloads every 1 ms). Only CVR is read: reading
     * CSR would clear COUNTFLAG, which sys_tick_ms() counts milliseconds with. */
    uint32_t want = us * (HCLK_HZ / 1000000u), reload = SYST_RVR + 1u;
    uint32_t prev = SYST_CVR, done = 0;
    while (done < want) {
        uint32_t now = SYST_CVR;
        done += now <= prev ? prev - now : prev + reload - now;
        prev = now;
    }
}

volatile uint32_t *hal_pin_out_reg(uint16_t pin)
{
    return &GPIO_PDIO(pin);
}

const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask)
{
    *mask = 1u << (pin & 15u);
    return (const volatile uint32_t *)(GPIO_BASE + 0x40u * (pin >> 4) + 0x10u);   /* GPIOx_PIN */
}

void hal_pin_mode(uint16_t pin, uint8_t mode)
{
    uint32_t port = pin >> 4, bit = pin & 15u;
    uint32_t pmd = mode == HAL_OUT ? PMD_OUTPUT : mode == HAL_OUT_OD ? PMD_OPEN_DRAIN :
                   mode == HAL_IN_PULLUP ? PMD_QUASI : PMD_INPUT;
    if (mode == HAL_IN_PULLUP)
        GPIO_PDIO(pin) = 1;         /* quasi mode pulls up only while DOUT = 1 */
    GPIO_PMD(port) = (GPIO_PMD(port) & ~(3u << (2u * bit))) | pmd << (2u * bit);
}

int hal_pin_read(uint16_t pin)
{
    return (int)(GPIO_PDIO(pin) & 1u);
}

void hal_pin_write(uint16_t pin, int level)
{
    GPIO_PDIO(pin) = level ? 1u : 0u;
}

int32_t hal_adc_read(uint16_t pin)
{
    if (pin > 7)
        return -1;                  /* ADC0..7 are PA0..PA7 */
    static uint8_t ready, setup;
    if (!ready) {
        CLK_CLKSEL1 &= ~CLKSEL1_ADC_S_MASK;               /* 12 MHz crystal */
        CLK_CLKDIV = (CLK_CLKDIV & ~CLKDIV_ADC_N_MASK) | (1u << CLKDIV_ADC_N_SHIFT);
        CLK_APBCLK |= APBCLK_ADC_EN;                      /* 6 MHz ADC clock */
        ADC_ADCR = ADCR_ADEN;
        ready = 1;
    }
    if (!(setup & (1u << pin))) {
        GPIO_PMD(PORTA) &= ~(3u << (2u * pin));           /* input */
        GPIO_OFFD(PORTA) |= 1u << (16u + pin);            /* digital path off */
        GCR_GPA_MFP |= 1u << pin;                         /* pin = ADCn */
        setup |= (uint8_t)(1u << pin);
    }
    ADC_ADCHER = 1u << pin;
    ADC_ADSR = ADSR_ADF;
    ADC_ADCR = ADCR_ADEN | ADCR_ADST;
    for (uint32_t i = 0; !(ADC_ADSR & ADSR_ADF); i++)
        if (i > 20000u)
            return -1;
    return (int32_t)(ADC_ADDR(pin) & ADDR_RSLT_MASK);
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

void hal_midi_send(const uint8_t *m, uint32_t n)
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

void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    if (!n)
        return;
    if (m[0] == 0xF0) {
        midi_tx_sysex_cable(cable, m, n);
        return;
    }
    midi_tx_event((uint8_t)(cable << 4 | m[0] >> 4), m[0], n > 1 ? m[1] : 0, n > 2 ? m[2] : 0);
}

void hal_midi_poll(void)
{
    midi_tx_poll();
}

void hal_midi_flush(uint32_t timeout_ms)
{
    midi_tx_flush(timeout_ms);
}

/* ---- identity, reboot, upload ---------------------------------------- */

/* The NUC120BN has no unique ID: ISP command 0x04 fails on it (newer chip
 * revisions have one). Then the first boot makes a random ID and keeps it in
 * the last data-flash page, which the config journal and factory reset never
 * touch. Record: 'GDID', id[0..2], crc32 (zlib) over the first 4 words. The
 * bootloader can read it as plain memory. */
#define DEVID_MAGIC 0x44494447u     /* 'GDID' */

static int devid_load(uint32_t w[3])
{
    uint32_t crc = CRC32_INIT;
    if (fmc_read(FLASH_DEVID_ADDR) != DEVID_MAGIC)
        return 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t v = fmc_read(FLASH_DEVID_ADDR + 4u * i);
        crc = crc32_word(crc, v);
        if (i)
            w[i - 1] = v;
    }
    return ~crc == fmc_read(FLASH_DEVID_ADDR + 16u);
}

/* Timer1 runs from the internal 22 MHz RC oscillator, which drifts against
 * the crystal that times the samples: the counts differ from chip to chip
 * (RC tolerance) and from sample to sample (jitter). */
static uint32_t devid_noise(uint32_t crc)
{
    CLK_APBCLK |= APBCLK_TMR1_EN;
    CLK_CLKSEL1 |= CLKSEL1_TMR1_S_MASK;
    TMR1_TCSR = TCSR_CRST;
    TMR1_TCMPR = 0xFFFFFFu;
    TMR1_TCSR = TCSR_CEN | TCSR_MODE_PERIODIC | TCSR_TDR_EN;
    for (uint32_t i = 0; i < 256u; i++) {
        hal_delay_us(20u + (crc & 31u));
        crc = crc32_word(crc, TMR1_TDR ^ (SYST_CVR << 8));
    }
    TMR1_TCSR = 0;
    CLK_APBCLK &= ~APBCLK_TMR1_EN;
    return crc;
}

static void devid_make(uint32_t w[3])
{
    uint32_t crc = CRC32_INIT;
    for (uint32_t i = 0; i < 3; i++)
        w[i] = crc = devid_noise(crc ^ i);
    uint32_t rec[5] = { DEVID_MAGIC, w[0], w[1], w[2], 0 };
    crc = CRC32_INIT;
    for (uint32_t i = 0; i < 4; i++)
        crc = crc32_word(crc, rec[i]);
    rec[4] = ~crc;
    for (uint32_t i = 0; i < 5; i++)
        if (fmc_read(FLASH_DEVID_ADDR + 4u * i) != 0xFFFFFFFFu) {
            fmc_erase_page(FLASH_DEVID_ADDR);
            break;
        }
    for (uint32_t i = 0; i < 5; i++)
        fmc_program(FLASH_DEVID_ADDR + 4u * i, rec[i]);
    /* If that failed the ID still holds until the next power cycle. */
}

void hal_uid(uint8_t uid[12])
{
    static uint32_t w[3];
    static uint8_t have;
    if (!have) {
        if (fmc_read_uid(w) != 0 && !devid_load(w))
            devid_make(w);
        have = 1;
    }
    for (uint32_t i = 0; i < 12; i++)
        uid[i] = (uint8_t)(w[i / 4u] >> (8u * (i & 3u)));
}

void hal_reboot(int to_bootloader)
{
    irq_disable_all();
    NVIC_ICER = 0xFFFFFFFFu;        /* the next stage starts with no IRQs */
    TMR0_TCSR = 0;
    if (to_bootloader) {
        MAILBOX->magic = MAILBOX_MAGIC_BOOT;
        MAILBOX->magic_inv = ~MAILBOX_MAGIC_BOOT;
        MAILBOX->reason = 1;        /* 1 = REBOOT(bootloader) from app */
    }
    usbd_disconnect();
    sys_delay_ms(10);
    sys_reset_to(1);                /* via LDROM; it starts the app unless asked */
}

const hal_fw_ops_t *hal_fw_ops(void)
{
    return 0;                       /* the LDROM bootloader takes uploads */
}

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
#include "usbd.h"
#include "usb_midi.h"

/* Bit access: one 32-bit register per pin (TRM 5.6, GPIOx_PDIOn). Reads
 * give the pin level, writes set DOUT for that pin only (no read-modify-
 * write, so the scan interrupt and the main loop can't clobber each other). */
#define GPIO_PDIO(pin) REG32(GPIO_BASE + 0x200u + 0x40u * ((pin) >> 4) + 4u * ((pin) & 15u))

uint32_t hal_millis(void)
{
    return sys_tick_ms();
}

void hal_delay_us(uint32_t us)
{
    if (us >= 2000u) {
        sys_delay_ms(us / 1000u);
        return;
    }
    /* about 4 cycles per iteration at 48 MHz */
    for (volatile uint32_t i = us * 12u; i; i--) {
    }
}

void hal_pin_mode(uint16_t pin, uint8_t mode)
{
    uint32_t port = pin >> 4, bit = pin & 15u;
    uint32_t pmd = mode == HAL_OUT ? PMD_OUTPUT : mode == HAL_IN_PULLUP ? PMD_QUASI : PMD_INPUT;
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

void hal_midi_poll(void)
{
    midi_tx_poll();
}

void hal_midi_flush(uint32_t timeout_ms)
{
    midi_tx_flush(timeout_ms);
}

/* ---- identity, reboot, upload ---------------------------------------- */

void hal_uid(uint8_t uid[12])
{
    fmc_read_uid(uid);
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

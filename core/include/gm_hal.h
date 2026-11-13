/*
 * gm_hal.h: what a platform (chip family) provides to the gemifi core.
 *
 * Implementations: platform/nuvoton-nuc1xx/src/hal.c (C, bare metal),
 * platform/esp/ (ESP-IDF, ESP32 / S2 / S3). Pin numbers here are MCU pins
 * only (hub 0 of gm_pin_t); expander hubs are handled by core/src/vpin.c.
 *
 * Received MIDI goes the other way: the platform calls gm_midi_rx_byte()
 * (controller.h) for every MIDI byte that arrives.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_HAL_H
#define GEMIFI_GM_HAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MIDI transports (gm_device_config.h GM_MIDI_TRANSPORT, ESP platforms) */
#define GM_MIDI_USB    1
#define GM_MIDI_SERIAL 2
#define GM_MIDI_BLE    3

/* time */
uint32_t hal_millis(void);
void hal_delay_us(uint32_t us);

/* MCU pins */
#define HAL_IN          0
#define HAL_IN_PULLUP   1
#define HAL_IN_PULLDOWN 2
#define HAL_OUT         3
#define HAL_OUT_OD      4            /* open-drain: 1 = released */
#define HAL_ANALOG      5            /* ADC input (platforms without such a mode: HAL_IN) */
void hal_pin_mode(uint16_t pin, uint8_t mode);
int hal_pin_read(uint16_t pin);              /* electrical level 0/1 */
void hal_pin_write(uint16_t pin, int level);
/* Optional fast read path for the key-matrix scan: the register holding
 * `pin`'s input level and the bit within it, so a scan can read a whole
 * port once instead of calling hal_pin_read per pin. Must be safe to read
 * from an interrupt. Return NULL if there is none (the scan then calls
 * hal_pin_read). A weak default returning NULL is in core/src/vpin.c. */
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask);
/* Optional fast write path for the key-matrix rows: a register where
 * writing 1 drives `pin` high and 0 drives it low (a per-pin data register,
 * like the NUC's GPIOx_PDIOn). Must be safe to write from an interrupt.
 * Return NULL if there is none (the scan then calls hal_pin_write). A weak
 * default returning NULL is in core/src/vpin.c. */
volatile uint32_t *hal_pin_out_reg(uint16_t pin);
/* 12-bit reading 0..4095 (blocking, a few us), or -1 if pin has no ADC. */
int32_t hal_adc_read(uint16_t pin);

/* I2C (only needed for MCP23017 / PCF8574 hubs). Return 0 or -1. */
int hal_i2c_write(uint8_t addr, const uint8_t *p, uint32_t n);
int hal_i2c_read(uint8_t addr, uint8_t *p, uint32_t n);

/* MIDI out: one complete message (channel message, or F0 ... F7).
 * Queues; hal_midi_poll() sends what it can without blocking. */
void hal_midi_send(const uint8_t *msg, uint32_t n);
/* The same on a USB-MIDI cable (port): 0 = main, 1 = the "<name> DAW" port
 * (GM_USB_DAW_PORT). Only needed when the device has a DAW port or MCU;
 * platforms without cables send everything on their one port. */
void hal_midi_send_cable(uint8_t cable, const uint8_t *msg, uint32_t n);
void hal_midi_poll(void);
/* Send queued data now (waits up to timeout_ms): before a reboot. Protocol
 * replies are only queued; hal_midi_poll sends them. */
void hal_midi_flush(uint32_t timeout_ms);
/* One raw USB-MIDI 1.0 event packet (cable << 4 | CIN, 3 bytes), queued
 * like hal_midi_send. Only needed for din_midi: (core/src/din.c, which has
 * a weak no-op). */
void hal_midi_send_usb(const uint8_t ev[4]);

/* hal_din_open / hal_serial_open flags: move the bytes by DMA rather than
 * from an interrupt (din_midi dma:, maudio_pad_leds dma:; the builder checks the
 * channels are free). Platforms without DMA ignore it. */
#define HAL_UART_DMA 1u
/* hal_serial_open: also receive, on the pin paired with tx_pin (maudio_pad_leds
 * rx_pin:), for hal_serial_read. */
#define HAL_UART_RX 2u

/* 5-pin DIN MIDI (din_midi:, core/src/din.c; weak defaults there say "none").
 * hal_din_open: the UART on tx_pin / rx_pin (rx_pin 0xFFFF = no MIDI In) at
 * baud, 8N1, with HAL_UART_DMA flags; 0 if the platform can't. hal_din_write: queue all n bytes, or
 * none (0) if they don't fit; never waits. hal_din_read: the next received
 * byte, or -1. Interrupt- or DMA-driven: the main loop only queues and
 * takes. hal_din_tx_queued: bytes written but not yet sent (din.c keeps
 * its backlog short with it; the weak default says 0). */
int hal_din_open(uint16_t tx_pin, uint16_t rx_pin, uint32_t baud, uint32_t flags);
int hal_din_write(const uint8_t *b, uint32_t n);
int hal_din_read(void);
uint32_t hal_din_tx_queued(void);

/* identity */
void hal_uid(uint8_t uid[12]);

/* reboot: to_bootloader = 1 asks for update mode (NUC: mailbox + LDROM;
 * A/B platforms: same as 0). Never returns. */
void hal_reboot(int to_bootloader) __attribute__((noreturn));

/* Firmware upload in the app (A/B platforms). NULL on platforms where
 * the bootloader does it. Each returns a GSX status. */
typedef struct {
    uint16_t max_chunk;
    int (*begin)(uint32_t size, uint32_t crc32, uint16_t board_id, uint32_t fw_version);
    int (*data)(uint32_t offset, const uint8_t *p, uint32_t n);
    int (*end)(void);
    void (*abort)(void);
} hal_fw_ops_t;
const hal_fw_ops_t *hal_fw_ops(void);
/* 0 on platforms whose bootloader takes uploads (nuvoton-nuc1xx, stm32f1:
 * their mk/app.mk sets it): app_proto.c then leaves out the FW_* handling and
 * its buffers, doesn't call hal_fw_ops(), and answers every FW_* with
 * BAD_STATE, as an app whose hal_fw_ops() returns NULL does. */
#ifndef HAL_APP_FW_UPLOAD
#define HAL_APP_FW_UPLOAD 1
#endif

/* Extra HELLO caps bits the platform knows about (e.g. GSX_CAP_GEMIFI_BOOT on
 * boards where the bootloader may be the vendor's or gemifi's). A weak
 * default returning 0 is in core/src/app_proto.c. */
uint16_t hal_extra_caps(void);

/* A serial port on tx_pin (8N1), for maudio_pad_leds hubs.
 * hal_serial_open sets it up, by DMA or interrupt per HAL_UART_DMA in flags
 * (0 if tx_pin can't be one), receiving too with HAL_UART_RX; hal_serial_write
 * starts sending n <= 16 bytes in the background, copied, and returns 0
 * without sending if the last write is still going; hal_serial_read returns
 * the next byte received (kept by interrupt, at least 16), or -1 for none.
 * Weak defaults that do nothing are in core/src/vpin.c. */
int hal_serial_open(uint16_t tx_pin, uint32_t baud, uint32_t flags);
int hal_serial_write(uint16_t tx_pin, const uint8_t *b, uint32_t n);
int hal_serial_read(uint16_t tx_pin);

/* An addressable LED chain on MCU pin pin, for ws2812 hubs (WS2812B,
 * SK6812): 800 kHz, each byte MSB first. The core sends wire bytes (its
 * colour order, gamma and white balance applied), whole frames, from the
 * main loop and never from the scan interrupt.
 * hal_ledstrip_open sets the pin up for frames of up to n bytes: 1, or 0
 * if it can't be one (the builder checks the pin per platform).
 * hal_ledstrip_write starts a frame of n bytes in the background and
 * returns 1, or 0 without sending if the last one is still going (b may
 * change once it returns: the HAL keeps its own copy or encoding).
 * hal_ledstrip_busy: the last frame, or the latch after it (the line low
 * for at least 280 us, so the LEDs show it), is still going.
 * Weak defaults in vpin.c: no strip (open returns 0). */
int hal_ledstrip_open(uint16_t pin, uint32_t n);
int hal_ledstrip_write(uint16_t pin, const uint8_t *b, uint32_t n);
int hal_ledstrip_busy(uint16_t pin);

/* Optional: mask interrupts for a few instructions, so the main loop can
 * read and reset what the scan interrupt collects (vpin_span_take) in one
 * go; hal_irq_save() returns what hal_irq_restore() needs. Weak defaults
 * that do nothing are in core/src/vpin.c: then a button sample can, rarely,
 * miss one scan's reading. */
uint32_t hal_irq_save(void);
void hal_irq_restore(uint32_t state);

/* A free-running CPU cycle counter, for timing the scan (gm_scan_part_max).
 * A weak default returning 0 is in core/src/controller.c. */
uint32_t hal_cycles(void);
/* Optional: how many hal_cycles() counts make ns (< 1000000, rounded up),
 * for short waits on hal_cycles() (the TM1628 bit timing); 0 if hal_cycles()
 * doesn't count. Must not divide at run time on a chip without a fast
 * divider. A weak default returning 0 is in core/src/vpin.c. */
uint32_t hal_ns_to_cycles(uint32_t ns);

/* A set/reset register for pin's port, where writing 1 << *bit sets the pin
 * and 1 << (*bit + 16) clears it (the STM32 BSRR), so several pins of one
 * port change in one store. NULL if the platform has none. A weak default
 * returning NULL is in core/src/vpin.c. */
volatile uint32_t *hal_pin_bsrr(uint16_t pin, uint32_t *bit);

/* Why the app started (diag.reset_reason, sysex.md 4.10): a GM_RESET_*
 * value of gm_safety.h, and whether the bootloader's boot-loop guard had
 * stopped the app before this start (sysex.md 6). Weak defaults returning
 * 0 (unknown, no) are in core/src/safety.c. */
uint32_t hal_reset_reason(void);
int hal_boot_loop(void);

/* Profile storage: config_store.h (config_store_load/commit/erase) is
 * implemented per platform (NUC: data-flash journal; ESP: NVS). */

#ifdef __cplusplus
}
#endif

#endif

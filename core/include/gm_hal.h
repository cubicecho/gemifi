/*
 * gm_hal.h: what a platform (chip family) provides to the gemifi core.
 *
 * Implementations: platform/nuvoton-nuc1xx/src/hal.c (C, bare metal),
 * platform/esp/ (Arduino, ESP32 + ESP8266). Pin numbers here are MCU pins
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
void hal_pin_mode(uint16_t pin, uint8_t mode);
int hal_pin_read(uint16_t pin);              /* electrical level 0/1 */
void hal_pin_write(uint16_t pin, int level);
/* Optional fast read path for the key-matrix scan: the register holding
 * `pin`'s input level and the bit within it, so a scan can read a whole
 * port once instead of calling hal_pin_read per pin. Must be safe to read
 * from an interrupt. Return NULL if there is none (the scan then calls
 * hal_pin_read). A weak default returning NULL is in core/src/vpin.c. */
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask);
/* 12-bit reading 0..4095 (blocking, a few us), or -1 if pin has no ADC. */
int32_t hal_adc_read(uint16_t pin);

/* I2C (only needed for MCP23017 / PCF8574 hubs). Return 0 or -1. */
int hal_i2c_write(uint8_t addr, const uint8_t *p, uint32_t n);
int hal_i2c_read(uint8_t addr, uint8_t *p, uint32_t n);

/* MIDI out: one complete message (channel message, or F0 ... F7).
 * Queues; hal_midi_poll() sends what it can without blocking. */
void hal_midi_send(const uint8_t *msg, uint32_t n);
void hal_midi_poll(void);
/* Send queued data now (waits up to timeout_ms): for protocol replies. */
void hal_midi_flush(uint32_t timeout_ms);

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

/* Profile storage: config_store.h (config_store_load/commit/erase) is
 * implemented per platform (NUC: data-flash journal; ESP: NVS / EEPROM). */

#ifdef __cplusplus
}
#endif

#endif

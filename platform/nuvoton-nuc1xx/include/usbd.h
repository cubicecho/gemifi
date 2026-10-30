/*
 * usbd.h: minimal polled USB full-speed device core for the NUC1xx USBD
 * (TRM 5.4), shaped for a single USB-MIDI function.
 *
 * Endpoint slots (the USBD has 6 simplex slots, TRM 5.4.4.2):
 *   slot 0: EP0 IN    slot 1: EP0 OUT
 *   slot 2: EP 0x81 bulk IN (64 B)    slot 3: EP 0x01 bulk OUT (64 B)
 * USB SRAM (512 B): setup 0x00, EP0 IN 0x08, EP0 OUT 0x48, 0x81 at 0x88,
 * 0x01 at 0xC8 (264 B used).
 *
 * Polled: call usbd_poll() often (every few ms at most; the host retries
 * NAKed transfers, and control transfers time out after 5 s).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_USBD_H
#define GEMIFI_USBD_H

#include <stdint.h>

typedef struct {
    const uint8_t *device;          /* 18-byte device descriptor        */
    const uint8_t *config;          /* full configuration descriptor    */
    uint16_t config_len;
    uint8_t num_strings;            /* string indices 1..num_strings    */
    const char *const *strings;     /* ASCII, <= 40 chars; [0] = index 1 */
} usbd_desc_t;

/* Enable the USBD clock, reset the block, drive SE0 for ~10 ms (so the host
 * sees a disconnect even if an earlier stage was enumerated), then attach.
 * Needs sys_tick_init() first (it uses sys_delay_ms). */
void usbd_init(const usbd_desc_t *d);
void usbd_poll(void);
int usbd_configured(void);

/* Drive SE0 (detach). Stays detached until usbd_init() runs again. */
void usbd_disconnect(void);

/* bulk IN 0x81 */
int usbd_in_busy(void);
void usbd_in_write(const uint8_t *p, uint32_t n);   /* n <= 64 */
void usbd_in_cancel(void);

/* bulk OUT 0x01: the packet stays in USB SRAM (and the endpoint NAKs)
 * until usbd_out_release(). */
int usbd_out_len(void);                             /* -1 = none */
const volatile uint8_t *usbd_out_data(void);
void usbd_out_release(void);

#endif

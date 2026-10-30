/*
 * usb_midi.h: USB-MIDI 1.0 descriptors and event packing on top of usbd.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_USB_MIDI_H
#define GEMIFI_USB_MIDI_H

#include <stdint.h>
#include "gsx.h"

#define U16LE(x) ((x) & 0xFF), (((x) >> 8) & 0xFF)

/* Device descriptor: USB 1.1 full speed, class defined at interface level,
 * EP0 64 B, strings 1 = manufacturer, 2 = product, 3 = serial. */
#define USB_MIDI_DEVICE_DESC(vid, pid, bcd_device) \
    18, 1, U16LE(0x0110), 0, 0, 0, 64, U16LE(vid), U16LE(pid), \
    U16LE(bcd_device), 1, 2, 3, 1

/* Configuration descriptor: USB-MIDI 1.0 spec Appendix B layout (101 B,
 * the same shape as the stock Keystation firmware): Audio Control interface
 * + MIDI Streaming interface with an embedded IN jack (1) and embedded OUT
 * jack (3), each wired to an external jack (2, 4), bulk OUT 0x01 -> jack 1,
 * bulk IN 0x81 <- jack 3, 64 B. Bus powered, 100 mA. */
#define USB_MIDI_CONFIG_DESC_LEN 101
#define USB_MIDI_CONFIG_DESC \
    /* configuration */ \
    9, 2, U16LE(USB_MIDI_CONFIG_DESC_LEN), 2, 1, 0, 0x80, 50, \
    /* interface 0: Audio Control */ \
    9, 4, 0, 0, 0, 1, 1, 0, 0, \
    /* CS AC header: ADC 1.00, total 9, 1 streaming interface (#1) */ \
    9, 0x24, 1, U16LE(0x0100), U16LE(9), 1, 1, \
    /* interface 1: MIDI Streaming, 2 endpoints */ \
    9, 4, 1, 0, 2, 1, 3, 0, 0, \
    /* CS MS header: MSC 1.00, total 65 */ \
    7, 0x24, 1, U16LE(0x0100), U16LE(65), \
    /* MIDI IN jack, embedded, id 1 */ \
    6, 0x24, 2, 1, 1, 0, \
    /* MIDI IN jack, external, id 2 */ \
    6, 0x24, 2, 2, 2, 0, \
    /* MIDI OUT jack, embedded, id 3, source = jack 2 pin 1 */ \
    9, 0x24, 3, 1, 3, 1, 2, 1, 0, \
    /* MIDI OUT jack, external, id 4, source = jack 1 pin 1 */ \
    9, 0x24, 3, 2, 4, 1, 1, 1, 0, \
    /* EP 0x01 bulk OUT 64 (audio-style 9-byte endpoint descriptor) */ \
    9, 5, 0x01, 2, U16LE(64), 0, 0, 0, \
    /* CS MS endpoint: 1 embedded jack, id 1 */ \
    5, 0x25, 1, 1, 1, \
    /* EP 0x81 bulk IN 64 */ \
    9, 5, 0x81, 2, U16LE(64), 0, 0, 0, \
    /* CS MS endpoint: 1 embedded jack, id 3 */ \
    5, 0x25, 1, 1, 3

/* ---- receive ---------------------------------------------------------- */
/* Unpacks pending OUT packets (cable 0, CIN 4..7 = SysEx) into rx. Returns 1
 * when rx holds a complete SysEx body; the rest of the USB packet is kept
 * and consumed on the next call (call gsx_rx_reset first). */
int midi_rx_poll(gsx_rx_t *rx);

/* ---- transmit --------------------------------------------------------- */
/* Queue one 4-byte USB-MIDI event; sent in 64-byte packets. */
void midi_tx_event(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2);
/* Queue a complete SysEx message (F0 ... F7) as CIN 4/5/6/7 events. */
void midi_tx_sysex(const uint8_t *p, uint32_t n);
/* Send whatever is queued. Waits up to timeout_ms for the IN endpoint
 * (polling usbd_poll() meanwhile); on timeout the data is dropped.
 * Returns 0 if sent, -1 if dropped. */
int midi_tx_flush(uint32_t timeout_ms);

#endif

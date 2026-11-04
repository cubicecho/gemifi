/*
 * gsx.h: gemifi SysEx frame codec (protocol/sysex.md sections 1-3).
 *
 * Frame:  F0 <id...> ver cmd seq len_lo len_hi payload... chk F7
 * The codec works on the "body": the bytes between F0 and F7, exclusive.
 * Plain C, no hardware access, so it also builds for the host (unit tests).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GSX_H
#define GEMIFI_GSX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SysEx ID + project signature. 0x7D is the "non-commercial" ID and must be
 * replaced by a real manufacturer ID for public products: override both
 * macros from the build (e.g. -DGSX_ID_BYTES=0x00,0x21,0x7F,0x67,0x6D
 * -DGSX_ID_LEN=5). */
#ifndef GSX_ID_BYTES
#define GSX_ID_BYTES 0x7D, 0x67, 0x6D
#define GSX_ID_LEN 3
#endif

#define GSX_PROTO_VERSION 1

/* body offsets */
#define GSX_OFF_VER     (GSX_ID_LEN + 0)
#define GSX_OFF_CMD     (GSX_ID_LEN + 1)
#define GSX_OFF_SEQ     (GSX_ID_LEN + 2)
#define GSX_OFF_LEN     (GSX_ID_LEN + 3)
#define GSX_OFF_PAYLOAD (GSX_ID_LEN + 5)
#define GSX_OVERHEAD    (GSX_ID_LEN + 6)   /* body bytes besides payload */

/* commands (section 4). Requests are 0x00-0x1F, replies cmd | 0x40
 * (0x40-0x5F), unsolicited device messages 0x60-0x7F. */
#define GSX_CMD_HELLO      0x01
#define GSX_CMD_GET_SCHEMA 0x02
#define GSX_CMD_GET_PARAM  0x03
#define GSX_CMD_SET_PARAM  0x04
#define GSX_CMD_COMMIT     0x07
#define GSX_CMD_GET_PARAMS 0x09
#define GSX_CMD_SET_PARAMS 0x0A
#define GSX_CMD_PRESET_INFO  0x0B
#define GSX_CMD_PRESET_SAVE  0x0C
#define GSX_CMD_PRESET_RESET 0x0D
#define GSX_CMD_PRESET_NAME  0x0E
#define GSX_CMD_GET_COMPONENT 0x11
#define GSX_CMD_PRESET_DELETE 0x12

/* Host -> device notifications (section 4.6.2): never answered */
#define GSX_NOTIFY_FIRST 0x20
#define GSX_NOTIFY_LAST  0x3F
#define GSX_FB_LED       0x20
#define GSX_FB_LED_RGB   0x21
#define GSX_FB_LEDS      0x22
#define GSX_FB_VALUE     0x23
#define GSX_FB_RING      0x24
#define GSX_FB_TEXT      0x25
#define GSX_FB_RELEASE   0x26

/* GET_COMPONENT kinds (section 4.6.1) */
#define GSX_KIND_BUTTON  1
#define GSX_KIND_MATRIX  2
#define GSX_KIND_ANALOG  3
#define GSX_KIND_ENCODER 4
#define GSX_KIND_LED     5
#define GSX_KIND_DISPLAY 6
/* GET_COMPONENT encoder `mode`: encoder.mode 0..2, or 3 for a Mackie V-Pot */
#define GSX_ENC_MODE_MCU 3
#define GSX_NONE_INDEX   0x3FFFu
#define GSX_LSTRING_MAX  31u
#define GSX_CMD_FACTORY_RESET 0x08
#define GSX_CMD_REBOOT     0x10
#define GSX_CMD_FW_BEGIN   0x18
#define GSX_CMD_FW_DATA    0x19
#define GSX_CMD_FW_END     0x1A
#define GSX_CMD_FW_ABORT   0x1B
#define GSX_REPLY          0x40
#define GSX_MSG_LOG        0x60
#define GSX_MSG_EVENT      0x61
#define GSX_MSG_PARAM_CHANGED 0x62
#define GSX_MSG_PRESET_CHANGED 0x63
#define GSX_MSG_BOOTED     0x64   /* section 4.6.3 */

/* status codes (section 3) */
#define GSX_OK           0
#define GSX_BAD_CHECKSUM 1
#define GSX_BAD_CMD      2
#define GSX_BAD_LEN      3
#define GSX_BAD_STATE    4
#define GSX_BAD_ADDR     5
#define GSX_FLASH_ERR    6
#define GSX_BAD_IMAGE    7
#define GSX_BAD_BOARD    8
#define GSX_BUSY         9
#define GSX_BAD_PARAM    10
#define GSX_BAD_VALUE    11

/* HELLO capability bits (section 4.1) */
#define GSX_CAP_FW_UPLOAD (1u << 0)
#define GSX_CAP_CONFIG    (1u << 1)
#define GSX_CAP_LOG       (1u << 2)
#define GSX_CAP_EVENT     (1u << 3)
#define GSX_CAP_PRESETS   (1u << 5)
#define GSX_CAP_FEEDBACK  (1u << 6)
#define GSX_CAP_GEMIFI_BOOT (1u << 7) /* REBOOT(1) leads to a gemifi bootloader */

/* HELLO `node`: longest node name (section 4.1.1) */
#define GSX_NODE_NAME_MAX 31u

/* ---- byte-stream receiver -------------------------------------------- */
typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t n;        /* body bytes so far                          */
    uint8_t state;     /* 0 idle, 1 inside F0..F7, 2 overflowed       */
} gsx_rx_t;

/* Feed one raw MIDI byte. Returns 1 at the F7 of a SysEx message: the body
 * (F0..F7 exclusive) is in rx->buf[0 .. rx->n), or, if it was longer than
 * rx->cap, its first cap bytes with rx->state == 2 (gsx_parse then reports
 * GSX_BAD_LEN). The caller handles it, then calls gsx_rx_reset.
 * Real-time bytes (F8..FF) are ignored; any other status byte aborts. */
int gsx_rx_byte(gsx_rx_t *rx, uint8_t b);
static inline void gsx_rx_reset(gsx_rx_t *rx) { rx->n = 0; rx->state = 0; }

/* ---- frame parser ------------------------------------------------------ */
typedef struct {
    uint8_t cmd;
    uint8_t seq;
    uint16_t len;            /* payload length in SysEx bytes */
    const uint8_t *payload;
} gsx_frame_t;

#define GSX_NOT_OURS (-1)    /* not a gemifi request (other SysEx, or a
                                gemifi reply/unsolicited message, cmd >= 0x40):
                                ignore silently */
/* Returns GSX_NOT_OURS, or a status code: GSX_OK, GSX_BAD_CHECKSUM,
 * GSX_BAD_LEN (length field disagrees with the body) or GSX_BAD_CMD (wrong
 * protocol version), or GSX_BAD_LEN for a frame that overflowed the
 * receive buffer. cmd/seq are valid whenever the result is >= 0. */
int gsx_parse(const gsx_rx_t *rx, gsx_frame_t *f);

/* ---- integer and packed-binary decoding ------------------------------- */
/* n 7-bit groups, least significant first (u14 = 2, u28 = 4, u32 = 5). */
uint32_t gsx_get_u(const uint8_t *p, uint32_t n);
/* 8->7 unpack. Returns the decoded length, or -1 if malformed (a trailing
 * group with only the MSB byte). In-place (dst == src) is safe. */
int gsx_unpack(const uint8_t *src, uint32_t n, uint8_t *dst);

/* ---- frame writer ------------------------------------------------------ */
typedef struct {
    uint8_t *buf;
    uint16_t n;
} gsx_tx_t;

/* Writes F0, id, ver, cmd, seq and a length placeholder. */
void gsx_tx_begin(gsx_tx_t *t, uint8_t *buf, uint8_t cmd, uint8_t seq);
static inline void gsx_put(gsx_tx_t *t, uint8_t b) { t->buf[t->n++] = b & 0x7F; }
void gsx_put_u(gsx_tx_t *t, uint32_t v, uint32_t n);
void gsx_put_packed(gsx_tx_t *t, const uint8_t *src, uint32_t n);
/* Fills the length and checksum, appends F7; returns the frame length. */
uint16_t gsx_tx_end(gsx_tx_t *t);

#ifdef __cplusplus
}
#endif

#endif

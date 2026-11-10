/*
 * gm_hub.h: the hub driver interface (core-internal; core/README.md
 * "Adding a hub type").
 *
 * Each hub type (gm_device.h GM_HUB_*) has one gm_hub_ops_t: what vpin.c
 * calls for a hub of that type. An op left NULL has the default given with
 * it, so a driver only fills in what its chip does. Every op gets the hub's
 * config (gm_device.hubs[n]) and its state (gm_hub_state_t, one per hub:
 * all 0 before init, except never = 1, channel = -1 and flags).
 *
 * Where each op runs:
 *   - scan tick (interrupt): refresh and flush, unless GM_HUBF_BUS;
 *   - main loop: refresh and flush with GM_HUBF_BUS (every I2C_MS: buses
 *     that can't run in an interrupt), send (a few bus bytes a pass) and
 *     poll (every pass);
 *   - either: read, write, analog and rgb (where the component is read or
 *     shown).
 *
 * The scan's hottest paths stay direct, not through the table: vpin_read
 * on a mux channel, vpin_write on a decoder row, the cached reads and
 * writes (s->in / s->out), and the switch-matrix scan. Hub types with more
 * than pins (the switch matrix's scan, the TM1628's fade, the mux's
 * hardware analog scan, the XY pad's touch) keep their own functions in
 * gm_vpin.h; the table is what every hub has.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_HUB_H
#define GEMIFI_GM_HUB_H

#include <stdint.h>
#include "gm_device.h"

/* gm_hub_ops_t.flags */
#define GM_HUBF_BUS     (1u << 0)   /* refresh / flush from the main loop (I2C), not the scan tick */
#define GM_HUBF_IN_REG  (1u << 1)   /* s->in is its pins' levels: one load reads a pin (vpin_in_reg) */
#define GM_HUBF_NO_READ (1u << 2)   /* outputs only: vpin_read is 0 */
#define GM_HUBF_SELECT  (1u << 3)   /* one-of-N select outputs (decoder): "off" does nothing */
#define GM_HUBF_MATRIX  (1u << 4)   /* a switch matrix: vpin_scan_matrices scans it */
/* Set by vpin_init in gm_hub_state_t.flags, so the fast paths don't look
 * at the table: the type has a read / write op. */
#define GM_HUBF_READ    (1u << 6)
#define GM_HUBF_WRITE   (1u << 7)

typedef struct gm_hub_state gm_hub_state_t;

typedef struct {
    uint8_t flags;                                  /* GM_HUBF_* */
    /* Control pins to their idle levels, state to power-up (vpin_init). */
    void (*init)(const gm_hub_t *h, gm_hub_state_t *s);
    /* Pin p is read (vpin_input) / driven (vpin_output; vpin_write(p, 0)
     * follows). NULL: nothing to set up. */
    void (*input)(const gm_hub_t *h, gm_hub_state_t *s, gm_pin_t p);
    void (*output)(const gm_hub_t *h, gm_hub_state_t *s, gm_pin_t p);
    /* Electrical level of pin num, read through (vpin_read applies INVERT).
     * NULL: bit num of s->in (0 with GM_HUBF_NO_READ). */
    int (*read)(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num);
    /* Electrical level for pin num. NULL: bit num of s->out (flush sends it). */
    void (*write)(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num, int level);
    /* 0..4095 on channel num, or -1 (vpin_analog applies INVERT). NULL: not analog. */
    int32_t (*analog)(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num);
    /* A whole colour (0xRRGGBB) on the LED whose red pin is num: 1, or 0 if
     * num isn't one. NULL: on/off pins only (vpin_rgb is 0). */
    int (*rgb)(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num, uint32_t rgb);
    /* The chip's inputs into s->in / s->out to the chip, if changed. */
    void (*refresh)(const gm_hub_t *h, gm_hub_state_t *s);
    void (*flush)(const gm_hub_t *h, gm_hub_state_t *s);
    /* Main loop: write what changed, at most budget bus bytes (~12 us each
     * on the STM32); what's left waits for the next call. Returns the
     * budget left. Called with ~0 for everything at once (vpin_send_all),
     * and not at all on a pass that has no budget (vpin_poll busy). */
    uint32_t (*send)(const gm_hub_t *h, gm_hub_state_t *s, uint32_t budget);
    /* Main loop, every pass (vpin_poll): protocol timing, keepalives. */
    void (*poll)(const gm_hub_t *h, gm_hub_state_t *s, uint32_t now_ms);
} gm_hub_ops_t;

/* Per-hub state: a few common fields, then the type's own. */
struct gm_hub_state {
    uint32_t in;          /* last read, electrical levels, bit = hub pin (GM_HUBF_IN_REG) */
    uint32_t out;         /* wanted output levels                          */
    uint32_t written;     /* what the chip has                             */
    int8_t channel;       /* MUX / DECODER: selected output, -1 = none; SWMATRIX:
                             next row; MAUDIO_PADS: last LED sent; XY: axis driven */
    uint8_t never;        /* outputs never written yet                     */
    uint8_t flags;        /* the type's GM_HUBF_* (in RAM, for the fast paths) */
    uint16_t ram;         /* where its bytes start in vpin.c's hub RAM, if any */
    union {
        struct {          /* MCP23017 / PCF8574 */
            uint32_t dir_out;     /* pins used as outputs          */
            uint32_t pullup;      /* MCP: pins with the pull-up    */
            uint32_t in_used;     /* pins something reads          */
            uint8_t cfg_dirty;    /* MCP: IODIR / GPPU need writing */
        } io;
        struct {          /* TM1628 */
            uint8_t dirty;        /* RAM changed since the last write */
            uint8_t ctl;          /* display control wanted (0x88 | step, 0x80 off), 0 = its intensity */
            uint8_t ctl_sent;     /* ... and what the chip has     */
            uint16_t peak[2];     /* fade: brightest LED noted + 1, 0 = none:
                                     [0] looks and plain levels, [1] host colours */
        } tm;
        struct {          /* MAX7219 / TM1637 / HT16K33 */
            uint8_t dirty;        /* RAM changed since the last write */
        } seg;
        struct {          /* SWMATRIX */
            uint32_t seen_hi;     /* bits read high / low by any scan */
            uint32_t seen_lo;     /*   since the last vpin_span_take() */
            uint32_t span_hi;     /* seen_hi / seen_lo as of that take */
            uint32_t span_lo;
        } sm;
        struct {          /* MAUDIO_PADS (the M-Audio Code pad LED board) */
            uint32_t t_ka, t_any; /* last A5 keepalive, last write   */
            uint32_t t_led;       /* when the awaited LED went out   */
            uint8_t repainted;    /* every LED sent again once        */
            uint8_t wait_led;     /* rx_pin: the answer awaited, LED + 1, 0 = none */
            uint8_t misses;       /* ... unanswered in a row          */
            uint8_t acks;         /* 0 none yet, 1 answering, 2 gave up */
        } mpads;
        struct {          /* WS2812 */
            uint32_t t_frame;     /* ms the last frame went out       */
            uint8_t dirty;        /* a colour changed since           */
            uint8_t open;         /* hal_ledstrip_open took the pin   */
            uint8_t tried;        /* a frame went out (t_frame is set) */
        } ws;
        struct {          /* XY */
            uint16_t raw[2];      /* last X / Y reading, 0xFFFF = none */
            uint8_t touch;        /* readings in a row with both in the window */
            uint8_t fresh;        /* bit = axis, touched since vpin_analog_jump */
            uint32_t t;           /* ms of the last axis switch      */
        } xy;
    } u;
};

#endif

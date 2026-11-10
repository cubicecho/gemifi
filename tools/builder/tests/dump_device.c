/* Prints every field of gm_device and the default arrays, so two configs
 * (the hand-written reference and the builder's output) can be compared
 * by value. Built by tests/test_builder.py with host gcc. */
#include <stdio.h>
#include "gm_device.h"
#include "gm_device_config.h"

#define ARR(n) ((n) > 0 ? (n) : 1)

static void pin(const char *what, gm_pin_t p)
{
    printf("  %s hub=%u flags=%u num=%u\n", what, p.hub, p.flags, p.num);
}

static void str(const char *what, const char *s)
{
    printf("  %s=%s\n", what, s ? s : "(null)");
}

/* A hub's config as the flat fields it once had (the checks in test_builder.py name them). */
typedef struct {
    unsigned chain, n_sel, addr, settle, ram, bright, grids;
    gm_pin_t data, clock, latch, sel[4], common, enable;
} flat_t;

static void hub(unsigned i, const gm_hub_t *h)
{
    static const gm_pin_t none = GM_NO_PIN;
    flat_t f = { 0, 0, 0, 0, 0, 0, 0, none, none, none, { none, none, none, none }, none, none };
    unsigned j;
    switch (h->type) {
    case GM_HUB_HC595:
    case GM_HUB_HC165:
        f.chain = h->sr.chain;
        f.data = h->sr.data;
        f.clock = h->sr.clock;
        f.latch = h->sr.latch;
        f.enable = h->sr.oe;
        break;
    case GM_HUB_MUX:
        f.n_sel = h->mux.n_sel;
        f.settle = h->mux.settle_us;
        for (j = 0; j < 4; j++) f.sel[j] = h->mux.sel[j];
        f.common = h->mux.common;
        f.enable = h->mux.enable;
        break;
    case GM_HUB_MCP23017:
    case GM_HUB_PCF8574:
        f.addr = h->i2c.addr;
        break;
    case GM_HUB_DECODER:
        f.n_sel = h->dec.n_addr;
        for (j = 0; j < 4; j++) f.sel[j] = h->dec.addr[j];
        break;
    case GM_HUB_SWMATRIX:
        f.settle = h->sm.settle_us;
        break;
    case GM_HUB_TM1628:
        f.ram = h->tm.ram_bytes;
        f.bright = h->tm.brightness;
        f.grids = h->tm.grids;
        f.data = h->tm.dio;
        f.clock = h->tm.clk;
        f.latch = h->tm.stb;
        break;
    case GM_HUB_RGB_SERIAL:
        f.chain = h->rgb.count;
        f.ram = 3u * h->rgb.count;
        f.data = h->rgb.tx;
        f.latch = h->rgb.rx;
        break;
    case GM_HUB_XY:
        for (j = 0; j < 4; j++) f.sel[j] = h->xy.drive[j];
        f.data = h->xy.sense[0];
        f.clock = h->xy.sense[1];
        break;
    default:
        break;
    }
    printf(" hub %u type=%u chain=%u n_sel=%u addr=0x%02x settle=%u ram=%u bright=%u grids=%u\n", i,
           h->type, f.chain, f.n_sel, f.addr, f.settle, f.ram, f.bright, f.grids);
    pin("data", f.data); pin("clock", f.clock); pin("latch", f.latch);
    for (j = 0; j < 4; j++) pin("sel", f.sel[j]);
    pin("common", f.common); pin("enable", f.enable);
}

int main(void)
{
    const gm_device_t *d = &gm_device;
    unsigned i, j;
    str("name", d->name);
    printf("board_id=%u usb=%04x:%04x boot_combo=0x%08lx scan_us=%u\n", d->board_id, d->usb_vid,
           d->usb_pid, (unsigned long)d->boot_combo, d->scan_us);
    printf("macros: name=%s board=%u vid=%u pid=%u hubs=%d buttons=%d leds=%d analogs=%d "
           "encoders=%d matrix=%d keys=%d velocity=%d i2c=%d\n",
           GM_DEVICE_NAME, GM_BOARD_ID, GM_USB_VID, GM_USB_PID, GM_N_HUBS, GM_N_BUTTONS, GM_N_LEDS,
           GM_N_ANALOGS, GM_N_ENCODERS, GM_HAS_MATRIX, GM_MATRIX_KEYS, GM_MATRIX_VELOCITY, GM_HAS_I2C);
    printf("hubs=%u (%s)\n", d->n_hubs, d->hubs ? "ptr" : "null");
    for (i = 0; i < d->n_hubs; i++)
        hub(i, &d->hubs[i]);
    printf("buttons=%u (%s)\n", d->n_buttons, d->buttons ? "ptr" : "null");
    for (i = 0; i < d->n_buttons; i++) {
        pin("pin", d->buttons[i].pin); str("name", d->buttons[i].name);
        printf("  led=%d action=%ld number=%ld\n", d->buttons[i].led,
               (long)gm_button_action_defaults[i], (long)gm_button_number_defaults[i]);
    }
    printf("leds=%u (%s)\n", d->n_leds, d->leds ? "ptr" : "null");
    for (i = 0; i < d->n_leds; i++) {
        pin("pin", d->leds[i].pin); pin("pin2", d->leds[i].pin2); pin("pin3", d->leds[i].pin3);
    }
    printf("analogs=%u (%s)\n", d->n_analogs, d->analogs ? "ptr" : "null");
    for (i = 0; i < d->n_analogs; i++) {
        pin("pin", d->analogs[i].pin); str("name", d->analogs[i].name);
        printf("  raw=%u..%u cc=%ld\n", d->analogs[i].raw_min, d->analogs[i].raw_max,
               (long)gm_analog_cc_defaults[i]);
    }
    printf("encoders=%u (%s)\n", d->n_encoders, d->encoders ? "ptr" : "null");
    for (i = 0; i < d->n_encoders; i++) {
        pin("a", d->encoders[i].a); pin("b", d->encoders[i].b); str("name", d->encoders[i].name);
        printf("  steps=%u cc=%ld\n", d->encoders[i].steps_per_detent, (long)gm_encoder_cc_defaults[i]);
    }
    /* the arrays must exist (at least one element) even for a count of 0 */
    printf("defaults0: %ld %ld %ld %ld\n", (long)gm_button_action_defaults[0],
           (long)gm_button_number_defaults[0], (long)gm_analog_cc_defaults[0],
           (long)gm_encoder_cc_defaults[0]);
    if (d->matrix) {
        const gm_matrix_t *m = d->matrix;
        printf("matrix kind=%u keys=%u first=%u settle=%u rows=%u cols=%u\n", m->kind, m->n_keys,
               m->first_note, m->settle_us, m->n_rows, m->n_cols);
        for (i = 0; i < m->n_rows; i++) pin("row", m->rows[i]);
        for (i = 0; i < m->n_cols; i++) pin("col", m->cols[i]);
    } else {
        printf("matrix=null\n");
    }
    printf("presets=%d revision=%d\n", GM_N_PRESETS, GM_PRESETS_REVISION);
    for (i = 0; i < GM_N_PRESETS; i++) {
        const gm_preset_def_t *p = &gm_presets[i];
        printf(" preset %u name=%s program=%d n=%u vals=%s\n", i + 1, p->name, p->program, p->n_vals,
               p->vals ? "ptr" : "null");
        for (j = 0; j < p->n_vals; j++)
            printf("  val id=%u index=%u value=%ld\n", p->vals[j].id, p->vals[j].index,
                   (long)p->vals[j].value);
    }
#if GM_N_PADS > 0
    printf("pads=%d channel=%d curve=%d aftertouch=%d threshold=%d release=%d full=%d window=%d "
           "retrigger=%d\n", GM_N_PADS, GM_PAD_CHANNEL_DEFAULT, GM_PAD_CURVE_DEFAULT,
           GM_PAD_AFTERTOUCH_DEFAULT, GM_PAD_THRESHOLD_DEFAULT, GM_PAD_RELEASE_DEFAULT,
           GM_PAD_FULL_DEFAULT, GM_PAD_WINDOW_DEFAULT, GM_PAD_RETRIGGER_DEFAULT);
    for (i = 0; i < GM_N_PADS; i++) {
        pin("pad", gm_pads[i].pin); str("name", gm_pads[i].name);
        printf("  note=%ld\n", (long)gm_pad_note_defaults[i]);
    }
#else
    printf("pads=0\n");
#endif
#ifdef GM_I2C_SDA
    printf("i2c sda=%d scl=%d\n", GM_I2C_SDA, GM_I2C_SCL);
#endif
#ifdef GM_MIDI_TRANSPORT
    printf("transport=%d\n", GM_MIDI_TRANSPORT);
#endif
    return 0;
}

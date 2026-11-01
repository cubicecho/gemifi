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
    for (i = 0; i < d->n_hubs; i++) {
        const gm_hub_t *h = &d->hubs[i];
        printf(" hub %u type=%u chain=%u n_sel=%u addr=0x%02x settle=%u\n", i, h->type, h->chain,
               h->n_sel, h->i2c_addr, h->settle_us);
        pin("data", h->data); pin("clock", h->clock); pin("latch", h->latch);
        for (j = 0; j < 4; j++) pin("sel", h->sel[j]);
        pin("common", h->common); pin("enable", h->enable);
    }
    printf("buttons=%u (%s)\n", d->n_buttons, d->buttons ? "ptr" : "null");
    for (i = 0; i < d->n_buttons; i++) {
        pin("pin", d->buttons[i].pin); str("name", d->buttons[i].name);
        printf("  led=%d action=%ld number=%ld\n", d->buttons[i].led,
               (long)gm_button_action_defaults[i], (long)gm_button_number_defaults[i]);
    }
    printf("leds=%u (%s)\n", d->n_leds, d->leds ? "ptr" : "null");
    for (i = 0; i < d->n_leds; i++) {
        pin("pin", d->leds[i].pin); pin("pin2", d->leds[i].pin2);
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
#ifdef GM_I2C_SDA
    printf("i2c sda=%d scl=%d\n", GM_I2C_SDA, GM_I2C_SCL);
#endif
#ifdef GM_MIDI_TRANSPORT
    printf("transport=%d\n", GM_MIDI_TRANSPORT);
#endif
    return 0;
}

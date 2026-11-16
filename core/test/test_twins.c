/*
 * test_twins.c: host test of layer twins on a device with more than 32
 * buttons (core/test/twins-test.yaml), so the button reading is 64 bits
 * wide: a twin takes its base's reading (read_buttons,
 * components/buttons/buttons.c), with one base below bit 32 (p3) and one
 * above (p35).
 *
 * Buttons p0..p39 send CC 0..39 on channel 1, p40 toggles the layer "lay"
 * (p3: CC 103, p35: CC 115). All pins inverted (pressed = 0).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"

#if GM_N_BUTTONS != 43 || GM_BTN_TWINS != 2 || GM_N_MODES != 1
#error "test_twins.c needs the twins-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

static uint32_t now_ms;
static uint8_t level[64];

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l) { level[pin] = (uint8_t)l; }
int hal_pin_read(uint16_t pin) { return level[pin]; }
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask) { (void)pin; (void)mask; return 0; }
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return 0; }
int hal_i2c_write(uint8_t bus, uint8_t a, const uint8_t *p, uint32_t n) { (void)bus; (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t bus, uint8_t a, uint8_t *p, uint32_t n) { (void)bus; (void)a; (void)p; (void)n; return -1; }

static uint8_t out[4096];
static int out_n;
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    if (cable == 0 && out_n + (int)n < (int)sizeof out) {
        memcpy(out + out_n, m, n);
        out_n += (int)n;
    }
}
void hal_midi_send(const uint8_t *m, uint32_t n) { hal_midi_send_cable(0, m, n); }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

/* ---- helpers ---------------------------------------------------------- */

enum { P3 = 3, P35 = 35, P40 = 40 };

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        gm_poll();
    }
}

static void hold(int b, int down)
{
    level[gm_device.buttons[b].pin.num] = down ? 0 : 1;   /* inverted */
    run_ms(30);
}

static void tap(int b)
{
    hold(b, 1);
    hold(b, 0);
}

/* CCs on channel 1 with number d1 sent since the last clear; *last the
 * last one's value (SysEx skipped). */
static int count_cc(int d1, int *last)
{
    int n = 0;
    for (int i = 0; i + 2 < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == 0xB0 && out[i + 1] == d1) {
            n++;
            if (last)
                *last = out[i + 2];
        }
    }
    return n;
}

/* ---- the tests -------------------------------------------------------- */

int main(void)
{
    int v = -1;
    for (int i = 0; i < 64; i++)
        level[i] = 1;               /* buttons up (inverted) */
    gm_init(0x00010000u, 0);
    run_ms(2000);
    out_n = 0;

    /* -- layer off: the bases take the presses ---------------------------- */
    tap(P3);
    CHECK(count_cc(3, &v) == 2 && v == 0 && count_cc(103, 0) == 0, "p3: CC 3 press and release, not its twin's");
    out_n = 0;
    tap(P35);
    CHECK(count_cc(35, &v) == 2 && v == 0 && count_cc(115, 0) == 0, "p35: CC 35 press and release, not its twin's");
    out_n = 0;

    /* -- layer on: the twins take them, from their bases' pins ------------ */
    tap(P40);
    out_n = 0;
    hold(P3, 1);
    CHECK(count_cc(103, &v) == 1 && v == 127 && count_cc(3, 0) == 0, "lay: p3 pressed sends its twin's CC 103 127");
    hold(P3, 0);
    CHECK(count_cc(103, &v) == 2 && v == 0, "lay: p3 released, CC 103 0");
    out_n = 0;
    hold(P35, 1);
    CHECK(count_cc(115, &v) == 1 && v == 127 && count_cc(35, 0) == 0,
          "lay: p35 (above bit 32) pressed sends its twin's CC 115 127");
    hold(P35, 0);
    CHECK(count_cc(115, &v) == 2 && v == 0, "lay: p35 released, CC 115 0");
    out_n = 0;

    /* -- a neighbour of a base doesn't reach the twin ---------------------- */
    tap(34);
    tap(36);
    tap(2);
    tap(4);
    CHECK(count_cc(115, 0) == 0 && count_cc(103, 0) == 0 && count_cc(34, 0) == 2 && count_cc(36, 0) == 2,
          "lay: the bases' neighbours send their own CCs only");
    out_n = 0;

    /* -- layer off again: back to the bases ------------------------------- */
    tap(P40);
    out_n = 0;
    tap(P35);
    CHECK(count_cc(35, 0) == 2 && count_cc(115, 0) == 0, "layer off: p35 sends CC 35 again");

    printf("test_twins: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}

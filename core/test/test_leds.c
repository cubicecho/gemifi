/*
 * test_leds.c: host test of the LED looks (YAML idle: / active:, gm_led_t
 * looks, controller.c led_show) with core/test/leds-test.yaml (host
 * feedback on) and leds-local-test.yaml (the same, feedback off: no host
 * LEDs). The LEDs are on MCU pins; the simulated HAL keeps each pin's level
 * and counts the writes to the LED pins.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_presets.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_device_config.h"
#include "gm_feedback.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

/* stm32f1 pin numbers: PA = 0.., PB = 16.., PC = 32.. */
#define B_SINGLE 16             /* PB0 */
#define B_BI     17             /* PB1 */
#define B_OCT    19             /* PB3 */
#define B_DEF    20             /* PB4 */
#define B_RGB    21             /* PB5 */
#define L_SINGLE 32             /* PC0 */
#define L_BI     33             /* PC1, PC2 */
#define L_RGB    35             /* PC3-PC5 */
#define L_OCT    38             /* PC6, PC7 */
#define L_DEF    40             /* PC8 */
#define L_SPARE  41             /* PC9 */
#define L_FB     42             /* PC10-PC12 */
#define L_FIRST  32
#define L_LAST   44

static uint32_t now_ms;
static uint8_t level[64], ever_high[64];
static int led_writes;          /* writes to the LED pins */

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l)
{
    if (pin >= 64)
        return;
    level[pin] = (uint8_t)(l != 0);
    if (l)
        ever_high[pin] = 1;
    if (pin >= L_FIRST && pin <= L_LAST)
        led_writes++;
}
int hal_pin_read(uint16_t pin) { return pin < 64 ? level[pin] : 0; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return -1; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n) { (void)cable; (void)m; (void)n; }
void hal_midi_send(const uint8_t *m, uint32_t n) { (void)m; (void)n; }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }
int hal_serial_open(uint16_t tx_pin, uint32_t baud, uint32_t flags) { (void)tx_pin; (void)baud; (void)flags; return 1; }
int hal_serial_write(uint16_t tx_pin, const uint8_t *b, uint32_t n) { (void)tx_pin; (void)b; (void)n; return 1; }

/* ---- helpers ---------------------------------------------------------- */

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        gm_poll();
    }
}

static void press(uint16_t pin) { level[pin] = 0; run_ms(30); }     /* inverted, pull-up */
static void release(uint16_t pin) { level[pin] = 1; run_ms(30); }

/* The LED's pins from p (n of them) as a mask, bit 0 = the first. */
static unsigned pins(int p, int n)
{
    unsigned m = 0;
    for (int k = 0; k < n; k++)
        m |= (unsigned)level[p + k] << k;
    return m;
}

#if GM_HOST_LEDS
static void usb(uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint8_t ev[4] = { (uint8_t)(b0 >> 4), b0, b1, b2 };
    gm_midi_rx_usb(ev);
}
#endif

/* ---- tests ------------------------------------------------------------ */

int main(void)
{
    level[B_SINGLE] = level[B_BI] = level[B_OCT] = level[B_DEF] = level[B_RGB] = 1;   /* released */
    printf("LED looks (%s)\n", GM_HOST_LEDS ? "host LEDs" : "local only");

    gm_init(0x01000000u, 0);
    CHECK(presets_rec_fits(), "a record of either scope fits presets.c's rec[]");
    {
        int all = 1, dark = 1;
        for (int p = L_FIRST; p <= L_LAST; p++) {
            all &= ever_high[p];
            dark &= !level[p];
        }
        CHECK(all, "lamp test: every pin of every LED lit, whatever its looks");
        CHECK(dark, "lamp test over: every pin dark");
    }

    run_ms(20);
    CHECK(level[L_SINGLE] == 1, "single, idle: on: lit while idle");
    CHECK(pins(L_BI, 2) == 2, "bicolour, idle: colour2: pin2 (%u)", pins(L_BI, 2));
    CHECK(pins(L_RGB, 3) == 4, "RGB, idle: blue (%u)", pins(L_RGB, 3));
    CHECK(pins(L_OCT, 2) == 1, "octave LED, idle: colour1 (%u)", pins(L_OCT, 2));
    CHECK(level[L_DEF] == 0, "no looks: dark while idle, as before");
    CHECK(level[L_SPARE] == 1, "an LED nothing drives shows its idle look");
    CHECK(pins(L_FB, 3) == 4, "host LED with no host message: idle blue (%u)", pins(L_FB, 3));

    /* steady: nothing is written again (the LED cache) */
    led_writes = 0;
    run_ms(1200);
    CHECK(led_writes == 0, "steady idle LEDs aren't rewritten (%d writes)", led_writes);

    press(B_SINGLE);
    CHECK(level[L_SINGLE] == 0, "single, pressed: dark (no active: given)");
    release(B_SINGLE);
    CHECK(level[L_SINGLE] == 1, "single, released: lit again");

    press(B_BI);
    release(B_BI);
    CHECK(pins(L_BI, 2) == 1, "bicolour toggled on: active colour1 (%u)", pins(L_BI, 2));
    press(B_BI);
    release(B_BI);
    CHECK(pins(L_BI, 2) == 2, "bicolour toggled off: idle colour2 (%u)", pins(L_BI, 2));

    press(B_RGB);
    CHECK(pins(L_RGB, 3) == 7, "RGB held: active white (%u)", pins(L_RGB, 3));
    release(B_RGB);
    CHECK(pins(L_RGB, 3) == 4, "RGB released: blue (%u)", pins(L_RGB, 3));

    press(B_DEF);
    CHECK(level[L_DEF] == 1, "no looks: lit while held, as before");
    release(B_DEF);
    CHECK(level[L_DEF] == 0, "no looks: dark again");

#if GM_HOST_LEDS
    /* host feedback on an LED no button drives: CC 90 on channel 1 */
    usb(0xB0, 90, 127);
    run_ms(20);
    CHECK(pins(L_FB, 3) == 2, "host on: active green (%u)", pins(L_FB, 3));
    usb(0xB0, 90, 1);
    {
        int act = 0, idle = 0, other = 0;
        for (int i = 0; i < 120; i++) {
            run_ms(10);
            unsigned m = pins(L_FB, 3);
            act += m == 2;
            idle += m == 4;
            other += m != 2 && m != 4;
        }
        CHECK(act > 0 && idle > 0 && other == 0, "host blink: green / blue (%d, %d, %d other)", act, idle, other);
    }
    usb(0xB0, 90, 0);
    run_ms(20);
    CHECK(pins(L_FB, 3) == 4, "host off: idle blue (%u)", pins(L_FB, 3));
#if GM_HOST_FB
    gm_led_script_rgb(6, 127, 0, 0, 0);
    run_ms(20);
    CHECK(pins(L_FB, 3) == 1, "a DAW script's colour wins: red (%u)", pins(L_FB, 3));
    gm_led_script_rgb(6, 0, 0, 0, 0);
    run_ms(20);
    CHECK(pins(L_FB, 3) == 4, "DAW script off: idle blue (%u)", pins(L_FB, 3));
#endif
#endif

    /* octave count on a bicolour LED: 1 = active, 2 both, 3 pin2, 4 pin2
     * blinking with the idle look in the dark half */
    press(B_OCT);
    release(B_OCT);
    CHECK(pins(L_OCT, 2) == 2, "octave +1: active colour2 (%u)", pins(L_OCT, 2));
    press(B_OCT);
    release(B_OCT);
    CHECK(pins(L_OCT, 2) == 3, "octave +2: both (%u)", pins(L_OCT, 2));
    press(B_OCT);
    release(B_OCT);
    CHECK(pins(L_OCT, 2) == 2, "octave +3: pin2 (%u)", pins(L_OCT, 2));
    press(B_OCT);
    release(B_OCT);
    {
        int on = 0, idle = 0, other = 0;
        for (int i = 0; i < 120; i++) {
            run_ms(10);
            unsigned m = pins(L_OCT, 2);
            on += m == 2;
            idle += m == 1;
            other += m != 2 && m != 1;
        }
        CHECK(on > 0 && idle > 0 && other == 0, "octave +4: pin2 / idle colour1 (%d, %d, %d other)", on, idle, other);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}

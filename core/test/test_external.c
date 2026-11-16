/*
 * test_external.c: host test of external components (core/README.md
 * "External components") with core/test/external-test.yaml: a TCA9555
 * expander (hub type GM_HUB_EXT_TCA9555) and an ST7567 LCD (display
 * platform GM_SCREEN_EXT_ST7567), both from core/test/external/, on a fake
 * bus. Buttons b0 (pin 0) and b9 (pin 9) on the expander, b0's LED on pin 3;
 * a text display on the LCD shows the last message.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_screen.h"

#if !(GM_N_HUBS == 1 && GM_N_BUTTONS == 2 && GM_N_LEDS == 1 && GM_N_SCREENS == 1)
#error "test_external.c needs the external-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

static uint32_t now_ms;

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l) { (void)pin; (void)l; }
int hal_pin_read(uint16_t pin) { (void)pin; return 1; }
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask) { (void)pin; (void)mask; return 0; }
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return -1; }

/* The TCA9555 at 0x21: 8 registers (input 0-1, output 2-3, polarity 4-5,
 * config 6-7, 1 = input), the pointer moving on with each byte. */
static uint8_t tca[8] = { 0, 0, 0xFF, 0xFF, 0, 0, 0xFF, 0xFF }, tca_ptr;
static uint8_t tca_pins[2] = { 0xFF, 0xFF };    /* what the pins read */
static int tca_reads, tca_writes;

/* The ST7567 at 0x3F: commands (0x00 / 0x80 control bytes) and data
 * (0x40), into a 128 x 8 page RAM. */
static uint8_t st_ram[8][128], st_page, st_cmds[64];
static int st_n_cmds, st_data_writes;

static void st_write(const uint8_t *p, uint32_t n)
{
    if (!n)
        return;
    if (p[0] == 0x40) {             /* data: the rest of the page from column 0 */
        st_data_writes++;
        for (uint32_t k = 1; k < n && k - 1 < 128; k++)
            st_ram[st_page][k - 1] = p[k];
        return;
    }
    /* commands: after 0x00 all of them; 0x80 before each */
    for (uint32_t k = 1; k < n; k++) {
        if (p[0] == 0x80 && (k % 2) == 0)
            continue;               /* the next control byte */
        uint8_t c = p[k];
        if ((c & 0xF0) == 0xB0)
            st_page = c & 7u;
        if (st_n_cmds < (int)sizeof st_cmds)
            st_cmds[st_n_cmds++] = c;
    }
}

int hal_i2c_write(uint8_t bus, uint8_t a, const uint8_t *p, uint32_t n)
{
    (void)bus;
    if (a == 0x21 && n) {
        if (n > 1)
            tca_writes++;
        tca_ptr = p[0];
        for (uint32_t k = 1; k < n; k++, tca_ptr++)
            if (tca_ptr < 8)
                tca[tca_ptr] = p[k];
        return 0;
    }
    if (a == 0x3F) {
        st_write(p, n);
        return 0;
    }
    return -1;
}

int hal_i2c_read(uint8_t bus, uint8_t a, uint8_t *p, uint32_t n)
{
    (void)bus;
    if (a != 0x21)
        return -1;
    tca_reads++;
    for (uint32_t k = 0; k < n; k++, tca_ptr++)
        p[k] = tca_ptr < 2 ? tca_pins[tca_ptr] : tca_ptr < 8 ? tca[tca_ptr] : 0;
    return 0;
}

static uint8_t out[4096];
static int out_n;
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    (void)cable;
    if (out_n + (int)n < (int)sizeof out) {
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

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        gm_poll();
    }
}

/* Channel messages only (SysEx skipped): 1 if CC num = val was sent. */
static int sent_cc(int num, int val)
{
    for (int i = 0; i < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if ((out[i] & 0xF0) == 0xB0 && i + 2 < out_n && out[i + 1] == num && out[i + 2] == val)
            return 1;
    }
    return 0;
}

static int st_had(uint8_t c)
{
    for (int i = 0; i < st_n_cmds; i++)
        if (st_cmds[i] == c)
            return 1;
    return 0;
}

static int st_lit(void)
{
    int n = 0;
    for (int pg = 0; pg < 8; pg++)
        for (int x = 0; x < 128; x++)
            n += st_ram[pg][x] != 0;
    return n;
}

int main(void)
{
    CHECK(gm_device.hubs[0].type == GM_HUB_EXT_TCA9555 && GM_HUB_EXT_TCA9555 >= GM_HUB_N_TYPES,
          "the expander's type is numbered after the built-ins (%d)", gm_device.hubs[0].type);
    CHECK(gm_screens[0].type == GM_SCREEN_EXT_ST7567 && GM_SCREEN_EXT_ST7567 >= GM_SCREEN_N_TYPES,
          "the LCD's platform is numbered after the built-ins (%d)", gm_screens[0].type);

    gm_init(0x01000000u, 0);
    run_ms(2000);

    /* The TCA9555 through the hub base: pins 0 and 9 inputs, 3 an output. */
    CHECK(tca[6] == 0xF7 && tca[7] == 0xFF, "config: pin 3 an output, the rest inputs (%02X %02X)", tca[6], tca[7]);
    CHECK(!sent_cc(20, 127) && !sent_cc(29, 127), "nothing pressed: no CC 20 / 29");
    tca_reads = 0;
    run_ms(100);
    CHECK(tca_reads >= 99 && tca_reads <= 101, "read every ms (%d in 100 ms)", tca_reads);

    /* The ST7567 through the display base: set up (reset, contrast 33), then every page. */
    CHECK(st_had(0xE2) && st_had(0x81) && st_had(33) && st_had(0xAF), "LCD set up: reset, contrast 33, display on");
    CHECK(st_data_writes >= 8, "every page sent at the start (%d page writes)", st_data_writes);

    /* b0 (pin 0 low): CC 20, its LED on pin 3, and the LCD shows the message. */
    out_n = 0;
    int lit_before = st_lit();
    st_data_writes = 0;
    tca_pins[0] = 0xFE;
    run_ms(100);
    CHECK(sent_cc(20, 127), "b0 pressed: CC 20 127");
    CHECK(tca[2] & 0x08, "b0's LED on: output pin 3 high (%02X)", tca[2]);
    CHECK(st_data_writes > 0 && st_lit() != lit_before, "the LCD redrawn with the message (%d page writes, %d -> %d lit)",
          st_data_writes, lit_before, st_lit());
    tca_pins[0] = 0xFF;
    run_ms(100);
    CHECK(sent_cc(20, 0) && !(tca[2] & 0x08), "b0 released: CC 20 0, LED off");

    /* b9 (pin 9 = the second port's bit 1): CC 29. */
    out_n = 0;
    tca_pins[1] = 0xFD;
    run_ms(100);
    CHECK(sent_cc(29, 127), "b9 pressed (pin 9): CC 29 127");

    printf("test_external: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}

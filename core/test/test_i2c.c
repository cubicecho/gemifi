/*
 * test_i2c.c: host test of the I2C hubs (core/src/vpin.c) with
 * core/test/i2c-test.yaml, on a fake bus: MCP23017 led_io (0x20, LEDs
 * only), MCP23017 mixed_io (0x21, button play on GPA0, its LED on GPB0) and
 * PCF8574 btn_io (0x38, button stop on P0), and HT16K33 alpha (0x70), a
 * 4-character 14-segment display with Adafruit's segment order.
 *
 * A chip with no input on it is never read; the others are read every ms,
 * and outputs are written only when they change.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "app_proto.h"
#include "gm_display.h"

#if !(GM_N_HUBS == 4 && GM_N_BUTTONS == 2 && GM_N_LEDS == 3 && GM_N_DISPLAYS == 1)
#error "test_i2c.c needs the i2c-test.yaml config"
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

/* Two MCP23017s (BANK = 0: register pairs A, B; the pointer moves on with
 * each byte) and a PCF8574. */
typedef struct {
    uint8_t addr, reg[0x16], ptr;
    uint8_t gpio[2];                    /* what the pins read */
    int reads, writes;                  /* reads, register writes (not pointer-only) */
} mcp_t;
static mcp_t mcp[2] = { { .addr = 0x20, .gpio = { 0xFF, 0xFF } }, { .addr = 0x21, .gpio = { 0xFF, 0xFF } } };
static uint8_t pcf_in = 0xFF, pcf_out = 0xFF;   /* outputs high at power-up, as the chip */
static int pcf_reads, pcf_writes;

/* The HT16K33: its commands and RAM writes, logged as "21|00 xx xx ..|". */
static uint8_t ht_ram[16];
static int ht_ram_writes, ht_bytes;
static char ht_log[2048];
static size_t ht_len;

static void ht_write(const uint8_t *p, uint32_t n)
{
    for (uint32_t k = 0; k < n && ht_len + 4 < sizeof ht_log; k++)
        ht_len += (size_t)sprintf(ht_log + ht_len, k ? " %02X" : "%02X", p[k]);
    if (ht_len + 2 < sizeof ht_log) {
        ht_log[ht_len++] = '|';
        ht_log[ht_len] = 0;
    }
    ht_bytes += (int)n + 1;
    if (n > 1 || (n == 1 && p[0] < 0x10)) {   /* a RAM address, then data */
        ht_ram_writes++;
        for (uint32_t k = 1, a = p[0]; k < n; k++, a++)
            if (a < 16)
                ht_ram[a] = p[k];
    }
}

/* A 14-segment glyph in Adafruit's order: bits 11 and 13 (the lower
 * diagonals) swapped. */
static uint16_t ada(char c)
{
    uint16_t g = gm_seg14_glyph(c);
    return (uint16_t)((g & ~0x2800u) | ((g >> 11) & 1u) << 13 | ((g >> 13) & 1u) << 11);
}

static int ht_shows(const char *t)
{
    for (int i = 0; i < 4; i++)
        if ((ht_ram[2 * i] | ht_ram[2 * i + 1] << 8) != ada(t[i]))
            return 0;
    return 1;
}

static mcp_t *mcp_at(uint8_t a)
{
    for (int i = 0; i < 2; i++)
        if (mcp[i].addr == a)
            return &mcp[i];
    return 0;
}

int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n)
{
    mcp_t *m = mcp_at(a);
    if (m && n) {
        if (n > 1)
            m->writes++;                /* data; n = 1 only sets the pointer for a read */
        m->ptr = p[0];
        for (uint32_t k = 1; k < n; k++, m->ptr++)
            if (m->ptr < sizeof m->reg)
                m->reg[m->ptr] = p[k];
        return 0;
    }
    if (a == 0x38 && n == 1) {
        pcf_writes++;
        pcf_out = p[0];
        return 0;
    }
    if (a == 0x70) {
        ht_write(p, n);
        return 0;
    }
    return -1;
}

int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n)
{
    mcp_t *m = mcp_at(a);
    if (m) {
        m->reads++;
        for (uint32_t k = 0; k < n; k++, m->ptr++)
            p[k] = m->ptr == 0x12 ? m->gpio[0] : m->ptr == 0x13 ? m->gpio[1] :
                   m->ptr < sizeof m->reg ? m->reg[m->ptr] : 0;
        return 0;
    }
    if (a == 0x38 && n == 1) {
        pcf_reads++;
        p[0] = (uint8_t)(pcf_in & pcf_out);   /* quasi-bidirectional: written low reads low */
        return 0;
    }
    return -1;
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

int main(void)
{
    gm_init(0x01000000u, 0);
    CHECK(strncmp(ht_log, "21|00 ", 6) == 0 && strstr(ht_log, "|E9|81|") != NULL,
          "HT16K33: oscillator on, the RAM from 0, dimming 9, display on: %s", ht_log);
    run_ms(2000);
    CHECK(ada('0') == 0x0C3F, "the order: 0 = a-f and the K / L slash, as Adafruit's font (%04X)", ada('0'));

    CHECK(mcp[0].reg[0x00] == 0xFE && mcp[0].reg[0x01] == 0x7F, "led_io: IODIR set, GPA0 and GPB7 outputs (%02X %02X)",
          mcp[0].reg[0x00], mcp[0].reg[0x01]);
    CHECK(mcp[1].reg[0x0C] == 0x01, "mixed_io: GPA0 pulled up (GPPUA %02X)", mcp[1].reg[0x0C]);

    /* Reads: none of the LED-only chip, one per ms of the others. */
    mcp[0].reads = mcp[1].reads = pcf_reads = 0;
    mcp[0].writes = mcp[1].writes = pcf_writes = ht_ram_writes = 0;
    ht_len = 0;
    ht_log[0] = 0;
    run_ms(100);
    CHECK(mcp[0].reads == 0, "led_io (LEDs only) isn't read (%d reads in 100 ms)", mcp[0].reads);
    CHECK(mcp[1].reads >= 99 && mcp[1].reads <= 101, "mixed_io read every ms (%d in 100 ms)", mcp[1].reads);
    CHECK(pcf_reads >= 99 && pcf_reads <= 101, "btn_io read every ms (%d in 100 ms)", pcf_reads);
    CHECK(mcp[0].writes == 0 && mcp[1].writes == 0 && pcf_writes == 0,
          "nothing changed: no writes (%d %d %d)", mcp[0].writes, mcp[1].writes, pcf_writes);
    CHECK(ht_ram_writes == 0, "HT16K33: nothing changed, nothing written (%d): %s", ht_ram_writes, ht_log);

    /* play (GPA0 low = pressed): CC 20, and its LED on GPB0. */
    out_n = 0;
    mcp[1].gpio[0] = 0xFE;
    run_ms(30);
    CHECK(sent_cc(20, 127), "play pressed: CC 20 127");
    CHECK(mcp[1].reg[0x15] & 0x01, "play's LED lit (OLATB %02X)", mcp[1].reg[0x15]);
    mcp[1].gpio[0] = 0xFF;
    run_ms(30);
    CHECK(sent_cc(20, 0), "play released: CC 20 0");
    CHECK(ht_shows("20 0"), "the display: CC 20 0 (%02X%02X %02X%02X %02X%02X %02X%02X)", ht_ram[1], ht_ram[0],
          ht_ram[3], ht_ram[2], ht_ram[5], ht_ram[4], ht_ram[7], ht_ram[6]);
    ht_len = 0;
    ht_log[0] = 0;
    ht_bytes = 0;
    gm_display_message("20 5");
    run_ms(20);
    CHECK(ht_shows("20 5"), "a message: 20 5");
    CHECK(strcmp(ht_log, "06 ED|07 00|") == 0 && ht_bytes == 6,
          "only the changed character's bytes, one a pass within the budget: %s (%d bus bytes)", ht_log, ht_bytes);

    /* stop on the PCF8574 (P0 low = pressed). */
    out_n = 0;
    pcf_in = 0xFE;
    run_ms(30);
    CHECK(sent_cc(21, 127), "stop pressed: CC 21 127");
    pcf_in = 0xFF;
    run_ms(30);
    CHECK(sent_cc(21, 0), "stop released: CC 21 0");
    CHECK(mcp[0].reads == 0, "led_io still never read (%d)", mcp[0].reads);

    printf("test_i2c: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

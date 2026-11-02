/*
 * test_controller.c: host test of the core runtime (controller, params,
 * app_proto, settings, vpin) with the Keystation device config, a simulated
 * HAL (pins, key matrix, ADC, MIDI out) and the NUC1xx data-flash profile
 * store running on simulated flash.
 *
 * Writes the schema blob to argv[1] (if given) so the Python and web
 * parsers can be checked against the firmware's own serializer.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "app_proto.h"
#include "params.h"
#include "config_store.h"
#include "fmc.h"
#include "gsx.h"
#include "nuc1xx.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

static uint32_t now_ms;
static uint8_t level[64];            /* electrical pin levels (outputs + inputs) */
static uint8_t mode[64];
static uint8_t contact[8][8];        /* key matrix: [row][col] closed */
static int32_t adc[8];

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { mode[pin] = m; }
static void update_port_b(void);
void hal_pin_write(uint16_t pin, int l) { level[pin] = (uint8_t)l; update_port_b(); }

/* Keystation rows PA8 PA9 PC0..PC3 PC6 PC7, cols PB0..PB5 PB8 PB12 */
static const uint16_t row_pin[8] = { 8, 9, 32, 33, 34, 35, 38, 39 };
static const uint16_t col_pin[8] = { 16, 17, 18, 19, 20, 21, 24, 28 };

/* Port B input register for the fast column path (hal_pin_in_reg),
 * recomputed whenever a pin is written. GM_TEST_SLOW_COLS=1 turns the fast
 * path off, so both scan paths are tested. */
static volatile uint32_t port_b;
static int slow_cols, col_reads;
static int sim_level(uint16_t pin);
static void update_port_b(void)
{
    uint32_t v = 0;
    for (int b = 0; b < 16; b++)
        if (sim_level((uint16_t)(16 + b)))
            v |= 1u << b;
    port_b = v;
}
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask)
{
    if (slow_cols || pin < 16 || pin >= 32)
        return 0;
    *mask = 1u << (pin & 15u);
    return &port_b;
}

int hal_pin_read(uint16_t pin)
{
    for (int c = 0; c < 8; c++)
        if (col_pin[c] == pin)
            col_reads++;
    return sim_level(pin);
}

static int sim_level(uint16_t pin)
{
    for (int c = 0; c < 8; c++) {
        if (col_pin[c] != pin)
            continue;
        for (int r = 0; r < 8; r++)
            if (level[row_pin[r]] && contact[r][c])
                return 1;
        return 0;
    }
    return level[pin];
}

int32_t hal_adc_read(uint16_t pin) { return pin < 8 ? adc[pin] : -1; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }

static uint8_t out[16384];
static int out_n;
void hal_midi_send(const uint8_t *m, uint32_t n) { if (out_n + (int)n < (int)sizeof out) { memcpy(out + out_n, m, n); out_n += (int)n; } }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }

static jmp_buf reboot_jb;
static int reboot_to;
void hal_reboot(int to_bootloader) { reboot_to = to_bootloader; longjmp(reboot_jb, 1); }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

/* data flash, for platform/nuvoton-nuc1xx/src/config_store.c */
static uint8_t dflash[FLASH_DATA_SIZE];
static int erases;
void fmc_open(void) {}
uint32_t fmc_read(uint32_t a)
{
    uint32_t w;
    if (a < FLASH_DATA_BASE || a + 4 > FLASH_DATA_BASE + FLASH_DATA_SIZE)
        return 0xFFFFFFFFu;
    memcpy(&w, dflash + (a - FLASH_DATA_BASE), 4);
    return w;
}
int fmc_erase_page(uint32_t a)
{
    if (a < FLASH_DATA_BASE || a >= FLASH_DATA_BASE + FLASH_DATA_SIZE || (a & 511))
        return -1;
    memset(dflash + (a - FLASH_DATA_BASE), 0xFF, 512);
    erases++;
    return 0;
}
int fmc_program(uint32_t a, uint32_t w)
{
    if (a < FLASH_DATA_BASE || a + 4 > FLASH_DATA_BASE + FLASH_DATA_SIZE || (a & 3))
        return -1;
    uint32_t o;
    memcpy(&o, dflash + (a - FLASH_DATA_BASE), 4);
    o &= w;                          /* flash can only clear bits */
    memcpy(dflash + (a - FLASH_DATA_BASE), &o, 4);
    return 0;
}
int fmc_read_uid(uint32_t u[3]) { uint8_t b[12]; hal_uid(b); memcpy(u, b, 12); return 0; }

/* ---- helpers ---------------------------------------------------------- */

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)  /* scan ticks per ms */
            gm_scan_tick();
        now_ms++;
        gm_poll();
    }
}

static void ticks(uint32_t n)
{
    while (n--)
        gm_scan_tick();
}

/* Find a 3-byte channel message with this status in the output. */
static int find_msg(uint8_t status, int *d1, int *d2)
{
    for (int i = 0; i + 2 < out_n; i++) {
        if (out[i] == 0xF0) {        /* skip SysEx */
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == status) {
            if (d1) *d1 = out[i + 1];
            if (d2) *d2 = out[i + 2];
            return 1;
        }
    }
    return 0;
}

static int count_msgs(uint8_t status_hi)
{
    int n = 0;
    for (int i = 0; i < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if ((out[i] & 0xF0) == status_hi)
            n++;
    }
    return n;
}

/* Find a gemifi SysEx frame with this cmd; returns its payload offset. */
static int find_sysex(uint8_t cmd, int *len)
{
    for (int i = 0; i + GSX_OVERHEAD < out_n; i++) {
        if (out[i] == 0xF0 && out[i + 1] == 0x7D && out[i + 2] == 0x67 && out[i + 3] == 0x6D &&
            out[i + 1 + GSX_OFF_CMD] == cmd) {
            if (len)
                *len = (int)gsx_get_u(out + i + 1 + GSX_OFF_LEN, 2);
            return i + 1 + GSX_OFF_PAYLOAD;
        }
    }
    return -1;
}

static uint8_t seq;

/* Send a request; returns the reply status (or -1) and payload offset. */
static int request(uint8_t cmd, const uint8_t *payload, int n, int *pl)
{
    uint8_t buf[512];
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, cmd, ++seq & 0x7F);
    for (int i = 0; i < n; i++)
        gsx_put(&t, payload[i]);
    uint16_t len = gsx_tx_end(&t);
    out_n = 0;
    for (int i = 0; i < len; i++)
        gm_midi_rx_byte(buf[i]);
    int l;
    int p = find_sysex(cmd | GSX_REPLY, &l);
    if (p < 0)
        return -1;
    if (pl)
        *pl = p + 1;
    return out[p];
}

static int put_u(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++, v >>= 7)
        p[i] = v & 0x7F;
    return n;
}

static int set_param(uint16_t id, uint16_t index, int32_t v)
{
    uint8_t p[9];
    int n = put_u(p, id, 2);
    n += put_u(p + n, index, 2);
    n += put_u(p + n, (uint32_t)v, 5);
    return request(GSX_CMD_SET_PARAM, p, n, 0);
}

static int get_param(uint16_t id, uint16_t index, int32_t *v)
{
    uint8_t p[4];
    put_u(p, id, 2);
    put_u(p + 2, index, 2);
    int pl;
    int st = request(GSX_CMD_GET_PARAM, p, 4, &pl);
    if (st == GSX_OK)
        *v = (int32_t)gsx_get_u(out + pl + 4, 5);
    return st;
}

/* A key whose second contact closes dt_us after the first. */
static void press_key(int k, int dt_us)
{
    int dt_ticks = dt_us / (int)gm_device.scan_us;
    int r = k % 8, p = k / 8;
    out_n = 0;
    contact[r][2 * p + 1] = 1;
    ticks(1);
    ticks((uint32_t)dt_ticks);
    contact[r][2 * p] = 1;
    ticks(1);
    run_ms(1);
}

static void release_key(int k)
{
    int r = k % 8, p = k / 8;
    out_n = 0;
    contact[r][2 * p] = 0;
    contact[r][2 * p + 1] = 0;
    ticks(1);
    run_ms(1);
}

/* buttons are active-low on PA1..PA7 */
static void button(int i, int down)
{
    level[1 + i] = down ? 0 : 1;
    run_ms(15);
}

static void boot(void)
{
    gm_init(0x00010000u, 0);
    run_ms(2000);                    /* past the boot log */
    out_n = 0;
}

/* ---- tests ------------------------------------------------------------ */

int main(int argc, char **argv)
{
    memset(dflash, 0xFF, sizeof dflash);
    slow_cols = getenv("GM_TEST_SLOW_COLS") != NULL;
    printf("matrix scan: %s column path\n", slow_cols ? "per-pin" : "fast (port runs)");
    for (int i = 1; i <= 7; i++)
        level[i] = 1;                /* buttons released */
    adc[0] = 2048;

    boot();
    CHECK(gm_cfg.channel == 1 && gm_cfg.octave == 0 && gm_cfg.btn_action[0] == ACT_CC_MOMENTARY,
          "defaults");
    CHECK(mode[8] == HAL_OUT && mode[16] == HAL_IN, "matrix pin modes");
    CHECK(level[12] == 1, "sustain LED off (active-low) after the lamp test");

    /* schema */
    uint32_t ssize = params_schema_size();
    uint8_t *schema = malloc(ssize);
    CHECK(params_schema_read(0, schema, ssize) == ssize, "schema read");
    CHECK(schema[0] == 1, "schema version");
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        fwrite(schema, 1, ssize, f);
        fclose(f);
    }
    {
        uint8_t p[6];
        put_u(p, 10, 4);
        put_u(p + 4, 64, 2);
        int pl, len;
        CHECK(request(GSX_CMD_GET_SCHEMA, p, 6, &pl) == GSX_OK, "GET_SCHEMA");
        CHECK(gsx_get_u(out + pl, 4) == ssize, "GET_SCHEMA total");
        find_sysex(GSX_CMD_GET_SCHEMA | GSX_REPLY, &len);
        uint8_t data[64];
        int n = gsx_unpack(out + pl + 8, (uint32_t)(len - 9), data);
        CHECK(n == 64 && memcmp(data, schema + 10, 64) == 0, "GET_SCHEMA window matches");
        put_u(p + 4, 65, 2);
        CHECK(request(GSX_CMD_GET_SCHEMA, p, 6, 0) == GSX_BAD_LEN, "GET_SCHEMA max 65");
    }

    /* keys and velocity */
    int d1, d2;
    col_reads = 0;
    run_ms(5);
    CHECK(slow_cols ? col_reads > 0 : col_reads == 0, "scan used the %s column path (%d pin reads)",
          slow_cols ? "per-pin" : "fast", col_reads);
    press_key(0, 500);               /* 0.5 ms */
    CHECK(find_msg(0x90, &d1, &d2) && d1 == 48 && d2 == 127, "fast key: note 48 vel 127 (got %d %d)", d1, d2);
    release_key(0);
    CHECK(find_msg(0x80, &d1, 0) && d1 == 48, "note off 48");
    press_key(5, 10000);             /* 10 ms */
    CHECK(find_msg(0x90, &d1, &d2) && d1 == 53 && d2 >= 60 && d2 <= 78,
          "10 ms key: note 53 vel ~69 (got %d %d)", d1, d2);
    release_key(5);
    press_key(31, 250000);           /* 250 ms */
    CHECK(find_msg(0x90, &d1, &d2) && d1 == 79 && d2 == 1, "slow key: note 79 vel 1 (got %d %d)", d1, d2);
    release_key(31);
    /* order of contacts doesn't matter (keys 16..31 swap them on the stock board) */
    out_n = 0;
    contact[2][5] = 1; ticks(20); contact[2][4] = 1; ticks(1); run_ms(1);
    CHECK(find_msg(0x90, &d1, 0) && d1 == 48 + 18, "swapped contacts: note 66");
    release_key(18);
    /* half press then let go: no note */
    out_n = 0;
    contact[1][1] = 1; ticks(10); contact[1][1] = 0; ticks(1); run_ms(2);
    CHECK(count_msgs(0x90) == 0 && count_msgs(0x80) == 0, "half press: nothing");

    /* octave buttons, PARAM_CHANGED, LEDs */
    out_n = 0;
    button(3, 1);
    CHECK(gm_cfg.octave == 1, "oct+ -> 1");
    CHECK(find_sysex(GSX_MSG_PARAM_CHANGED, 0) >= 0, "PARAM_CHANGED sent");
    run_ms(20);
    CHECK(level[11] == 0 && level[15] == 1, "oct+ LED level 1 = colour A only");
    button(3, 0);
    press_key(0, 500);
    CHECK(find_msg(0x90, &d1, 0) && d1 == 60, "octave 1: note 60");
    button(3, 1); button(3, 0);
    run_ms(20);
    CHECK(gm_cfg.octave == 2 && level[11] == 0 && level[15] == 0, "oct+ 2: both colours");
    /* key held across an octave change: note off for the note that sounded */
    button(2, 1);
    button(3, 1);                    /* oct- held + oct+ = reset */
    CHECK(gm_cfg.octave == 0, "oct- + oct+ resets the octave (got %d)", (int)gm_cfg.octave);
    button(3, 0); button(2, 0);
    release_key(0);
    CHECK(find_msg(0x80, &d1, 0) && d1 == 60, "held note released as 60");

    /* sustain (CC momentary 64) and its LED */
    out_n = 0;
    button(0, 1);
    CHECK(find_msg(0xB0, &d1, &d2) && d1 == 64 && d2 == 127, "sustain on");
    run_ms(20);
    CHECK(level[12] == 0, "sustain LED lit");
    out_n = 0;
    button(0, 0);
    CHECK(find_msg(0xB0, &d1, &d2) && d1 == 64 && d2 == 0, "sustain off");

    /* edit = CC toggle 80 */
    out_n = 0;
    button(1, 1); button(1, 0);
    CHECK(find_msg(0xB0, &d1, &d2) && d1 == 80 && d2 == 127, "toggle on");
    out_n = 0;
    button(1, 1); button(1, 0);
    CHECK(find_msg(0xB0, &d1, &d2) && d1 == 80 && d2 == 0, "toggle off");

    /* pitch bend glide: down to -8192 within ~80 ms */
    out_n = 0;
    button(4, 1);
    run_ms(100);
    {
        int last = -1;
        for (int i = 0; i + 2 < out_n; i++)
            if (out[i] == 0xE0)
                last = out[i + 1] | out[i + 2] << 7;
        CHECK(last == 0, "bend reached the bottom (got %d)", last);
        CHECK(count_msgs(0xE0) > 10, "bend glides (%d messages)", count_msgs(0xE0));
    }
    out_n = 0;
    button(4, 0);
    run_ms(100);
    {
        int last = -1;
        for (int i = 0; i + 2 < out_n; i++)
            if (out[i] == 0xE0)
                last = out[i + 1] | out[i + 2] << 7;
        CHECK(last == 8192, "bend back to centre (got %d)", last);
    }

    /* mod = CC ramp on CC 1 */
    out_n = 0;
    button(6, 1);
    run_ms(400);
    CHECK(find_msg(0xB0, &d1, 0) && d1 == 1, "mod ramp sends CC 1");
    {
        int last = -1;
        for (int i = 0; i + 2 < out_n; i++)
            if (out[i] == 0xB0 && out[i + 1] == 1)
                last = out[i + 2];
        CHECK(last == 127, "mod ramp reaches 127 (got %d)", last);
    }
    button(6, 0);
    run_ms(400);

    /* knob */
    out_n = 0;
    adc[0] = 4095;
    run_ms(50);
    {
        int last = -1;
        for (int i = 0; i + 2 < out_n; i++)
            if (out[i] == 0xB0 && out[i + 1] == 7)
                last = out[i + 2];
        CHECK(last == 127, "knob to the top: CC 7 = 127 (got %d)", last);
    }
    out_n = 0;
    adc[0] = 4080;                   /* jitter near the top: no new messages */
    run_ms(50);
    CHECK(count_msgs(0xB0) == 0, "knob hysteresis");

    /* config over SysEx */
    CHECK(set_param(P_CHANNEL, 0, 2) == GSX_OK, "SET channel 2");
    CHECK(set_param(P_CHANNEL, 0, 17) == GSX_BAD_VALUE, "channel 17 -> BAD_VALUE");
    CHECK(set_param(999, 0, 1) == GSX_BAD_PARAM, "unknown id -> BAD_PARAM");
    CHECK(set_param(P_BTN_ACTION, 7, 1) == GSX_BAD_PARAM, "index 7 of 7 -> BAD_PARAM");
    int32_t v = 0;
    CHECK(get_param(P_CHANNEL, 0, &v) == GSX_OK && v == 2, "GET channel");
    CHECK(set_param(P_TRANSPOSE, 0, -3) == GSX_OK && get_param(P_TRANSPOSE, 0, &v) == GSX_OK && v == -3,
          "negative s32 round trip");
    press_key(0, 500);
    CHECK(find_msg(0x91, &d1, 0) && d1 == 45, "channel 2, transpose -3: 0x91 note 45");
    release_key(0);
    {
        /* GET_PARAMS / SET_PARAMS on button.number */
        uint8_t p[6 + 16 * 5];
        int n = put_u(p, P_BTN_NUMBER, 2);
        n += put_u(p + n, 0, 2);
        n += put_u(p + n, 7, 2);
        int pl;
        CHECK(request(GSX_CMD_GET_PARAMS, p, n, &pl) == GSX_OK, "GET_PARAMS");
        CHECK(gsx_get_u(out + pl + 6, 5) == 64 && gsx_get_u(out + pl + 6 + 30, 5) == 1, "GET_PARAMS values");
        n = put_u(p, P_BTN_NUMBER, 2);
        n += put_u(p + n, 5, 2);
        n += put_u(p + n, 2, 2);
        n += put_u(p + n, 20, 5);
        n += put_u(p + n, 200, 5);
        CHECK(request(GSX_CMD_SET_PARAMS, p, n, 0) == GSX_BAD_VALUE, "SET_PARAMS with 200 -> BAD_VALUE");
        CHECK(gm_cfg.btn_number[5] == 0, "SET_PARAMS all-or-nothing");
        n = put_u(p, P_BTN_NUMBER, 2);
        n += put_u(p + n, 5, 2);
        n += put_u(p + n, 2, 2);
        n += put_u(p + n, 20, 5);
        n += put_u(p + n, 21, 5);
        CHECK(request(GSX_CMD_SET_PARAMS, p, n, 0) == GSX_OK && gm_cfg.btn_number[6] == 21, "SET_PARAMS");
    }
    /* repeated seq: cached reply, not redone */
    {
        uint8_t buf[64];
        gsx_tx_t t;
        uint8_t p[9];
        int n = put_u(p, P_OCTAVE, 2);
        n += put_u(p + n, 0, 2);
        n += put_u(p + n, 1, 5);
        gsx_tx_begin(&t, buf, GSX_CMD_SET_PARAM, 100);
        for (int i = 0; i < n; i++)
            gsx_put(&t, p[i]);
        uint16_t len = gsx_tx_end(&t);
        for (int i = 0; i < len; i++) gm_midi_rx_byte(buf[i]);
        gm_cfg.octave = 0;
        out_n = 0;
        for (int i = 0; i < len; i++) gm_midi_rx_byte(buf[i]);
        CHECK(gm_cfg.octave == 0 && find_sysex(GSX_CMD_SET_PARAM | GSX_REPLY, 0) >= 0, "duplicate seq replayed");
    }

    /* COMMIT, reboot, load; many commits wrap the journal */
    CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK, "COMMIT");
    params_defaults();
    CHECK(gm_cfg.channel == 1, "defaults again");
    CHECK(config_store_load() == 1 && gm_cfg.channel == 2 && gm_cfg.transpose == -3 &&
          gm_cfg.btn_number[6] == 21, "profile loaded");
    for (int i = 0; i < 60; i++) {
        set_param(P_VEL_FIXED, 0, 10 + i);
        CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK, "COMMIT %d", i);
    }
    CHECK(erases > 8, "journal wrapped (%d erases)", erases);
    params_defaults();
    CHECK(config_store_load() == 1 && gm_cfg.vel_fixed == 69, "newest record wins (got %d)", (int)gm_cfg.vel_fixed);
    /* torn write after the newest record: still loads it */
    {
        uint32_t a;
        for (a = FLASH_DATA_BASE; a < FLASH_DATA_BASE + FLASH_DATA_SIZE; a += 4)
            if (fmc_read(a) == 0xFFFFFFFFu && fmc_read(a - 4) != 0xFFFFFFFFu)
                break;
        fmc_program(a, CONFIG_STORE_MAGIC);
        fmc_program(a + 4, 12345);
        params_defaults();
        CHECK(config_store_load() == 1 && gm_cfg.vel_fixed == 69, "torn record ignored");
        set_param(P_VEL_FIXED, 0, 77);
        CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK, "COMMIT after a torn record");
        params_defaults();
        CHECK(config_store_load() == 1 && gm_cfg.vel_fixed == 77, "and it loads");
    }

    CHECK(request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_OK, "FACTORY_RESET");
    CHECK(gm_cfg.channel == 1 && gm_cfg.vel_fixed == 100, "defaults after reset");
    CHECK(config_store_load() == 0, "nothing stored after reset");

    /* HELLO, FW_* in the app, REBOOT */
    {
        int pl;
        CHECK(request(GSX_CMD_HELLO, 0, 0, &pl) == GSX_OK, "HELLO");
        CHECK(out[pl] == 0 && gsx_get_u(out + pl + 1, 2) == 1, "role app, board 1");
        uint32_t caps = gsx_get_u(out + pl + 1 + 2 + 5 + 1 + 2, 2);
        CHECK(caps == (GSX_CAP_CONFIG | GSX_CAP_LOG | GSX_CAP_EVENT), "caps 0x%x", caps);
        CHECK(request(GSX_CMD_FW_END, 0, 0, 0) == GSX_BAD_STATE, "FW_* -> BAD_STATE on NUC");
        uint8_t m = 1;
        CHECK(request(GSX_CMD_REBOOT, &m, 1, 0) == GSX_OK, "REBOOT(1)");
        if (!setjmp(reboot_jb)) {
            run_ms(100);
            CHECK(0, "no reboot");
        } else {
            CHECK(reboot_to == 1, "rebooted to the bootloader");
        }
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}

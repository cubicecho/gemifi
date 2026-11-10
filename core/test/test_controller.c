/*
 * test_controller.c: host test of the core runtime (controller, params,
 * app_proto, settings, vpin, presets) with the Keystation device config
 * (and its presets), a simulated HAL (pins, key matrix, ADC, MIDI out) and a
 * RAM config store (fake_config_store.c).
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
#include "gm_presets.h"
#include "gm_safety.h"
#include "fake_config_store.h"
#include "gsx.h"

static int fails, checks;

/* The display's messages (linked with --wrap=gm_display_message). */
void __real_gm_display_message(const char *s);
static char shown[32];
void __wrap_gm_display_message(const char *s)
{
    strncpy(shown, s, sizeof shown - 1);
    __real_gm_display_message(s);
}
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

static uint32_t now_ms;
/* Electrical pin levels (outputs + inputs). A word per pin, like the NUC's
 * per-pin data registers: hal_pin_out_reg hands out the rows' words. */
static uint32_t level[64];
static uint8_t mode[64];
static uint8_t contact[8][8];        /* key matrix: [row][col] closed */
static int32_t adc[8];

uint32_t hal_millis(void) { return now_ms; }
static void update_port_b(void);
/* The scan settles between driving a row and reading it: the simulated
 * column port catches up with a row store then, as the real one does at
 * once. */
void hal_delay_us(uint32_t us) { (void)us; update_port_b(); }
void hal_pin_mode(uint16_t pin, uint8_t m) { mode[pin] = m; }

/* Keystation rows PA8 PA9 PC0..PC3 PC6 PC7, cols PB0..PB5 PB8 PB12 */
static const uint16_t row_pin[8] = { 8, 9, 32, 33, 34, 35, 38, 39 };
static const uint16_t col_pin[8] = { 16, 17, 18, 19, 20, 21, 24, 28 };

/* The rows' data registers for the fast row path (one store per edge, and
 * with the fast columns the scan's own loop, controller.c
 * matrix_scan_fast). GM_TEST_SLOW_ROWS=1 turns it off: rows by pin writes. */
static int slow_rows, row_writes;
volatile uint32_t *hal_pin_out_reg(uint16_t pin)
{
    for (int r = 0; r < 8; r++)
        if (row_pin[r] == pin)
            return slow_rows ? 0 : (volatile uint32_t *)&level[pin];
    return 0;
}
void hal_pin_write(uint16_t pin, int l)
{
    for (int r = 0; r < 8; r++)
        if (row_pin[r] == pin)
            row_writes++;
    level[pin] = (uint32_t)l;
    update_port_b();
}

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

/* The last pitch bend (status 0xE0 | channel 0) in out[], 0..16383, or -1. */
static int last_bend(void)
{
    int last = -1;
    for (int i = 0; i + 2 < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == 0xE0)
            last = out[i + 1] | out[i + 2] << 7;
    }
    return last;
}

/* Channel pressure messages (0xD0, 2 bytes) in out[]: how many, and the last value (-1: none). */
static int pressure(int *last)
{
    int n = 0;
    *last = -1;
    for (int i = 0; i + 1 < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == 0xD0) {
            *last = out[i + 1];
            n++;
        }
    }
    return n;
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
    run_ms(25);                     /* past the release debounce (BTN_STABLE) */
}

static void boot(void)
{
    gm_init(0x00010000u, 0);
    run_ms(2000);                    /* past the boot log */
    out_n = 0;
}

/* Physical unlock (sysex.md 4.9), opt-in: test_controller builds the
 * Keystation with `unlock: true` (Pitch down + Pitch up, buttons 4 and 5,
 * the boot_combo); test_controller_wtf builds it as shipped, unlock off. */
#if GM_UNLOCK
/* Hold the combo for the hold time, then let go. */
static void unlock_combo(void)
{
    button(4, 1);
    button(5, 1);
    run_ms(GM_UNLOCK_HOLD_MS + 50);
    button(4, 0);
    button(5, 0);
    run_ms(1000);                    /* the status LED's "unlocked" second */
}
#endif

/* A request that may need the unlock: on LOCKED, unlock and send it again. */
static int unlocked_request(uint8_t cmd, const uint8_t *payload, int n, int *pl)
{
    int st = request(cmd, payload, n, pl);
#if GM_UNLOCK
    if (st == GSX_LOCKED) {
        unlock_combo();
        st = request(cmd, payload, n, pl);
    }
#endif
    return st;
}

/* ---- presets ------------------------------------------------------------ */

static int preset_cmd(uint8_t cmd, uint32_t slot, int *pl)
{
    uint8_t p[2];
    put_u(p, slot, 2);
    return request(cmd, p, 2, pl);
}

static int preset_name(uint32_t slot, const char *name)
{
    uint8_t p[64];
    int n = put_u(p, slot, 2);
    int len = (int)strlen(name);
    p[n++] = (uint8_t)len;
    memcpy(p + n, name, (size_t)len);
    return request(GSX_CMD_PRESET_NAME, p, n + len, 0);
}

typedef struct {
    int slot, count, active, flags, revision, stored_revision, program;
    char name[20];
} pinfo_t;

static int preset_info(uint32_t slot, pinfo_t *in)
{
    int pl;
    int st = preset_cmd(GSX_CMD_PRESET_INFO, slot, &pl);
    if (st != GSX_OK)
        return st;
    const uint8_t *q = out + pl;
    in->slot = (int)gsx_get_u(q, 2);
    in->count = (int)gsx_get_u(q + 2, 2);
    in->active = (int)gsx_get_u(q + 4, 2);
    in->flags = q[6];
    in->revision = (int)gsx_get_u(q + 7, 2);
    in->stored_revision = (int)gsx_get_u(q + 9, 2);
    in->program = (int32_t)gsx_get_u(q + 11, 5);
    int n = q[16];
    memcpy(in->name, q + 17, (size_t)n);
    in->name[n] = 0;
    return GSX_OK;
}

/* PRESET_CHANGED in the output: 1 and its slot/dirty. */
static int preset_changed_msg(int *slot, int *dirty)
{
    int p = find_sysex(GSX_MSG_PRESET_CHANGED, 0);
    if (p < 0)
        return 0;
    *slot = (int)gsx_get_u(out + p, 2);
    *dirty = out[p + 2];
    return 1;
}

static int has_bytes(const uint8_t *b, int n)
{
    for (int i = 0; i + n <= out_n; i++)
        if (!memcmp(out + i, b, (size_t)n))
            return 1;
    return 0;
}

#if GM_WRITE_LAST_PRESET
/* write_to_flash.last_preset: the write waits for the delay and for the
 * controller to be idle (nothing held, nothing played for idle). Starts and
 * ends on slot 3, stored. */
static void test_write_idle(void)
{
    int32_t v;
    int w = fake_cs_writes;
    set_param(P_PRESET_ACTIVE, 0, 2);
    press_key(5, 500);              /* held through the delay */
    run_ms(12000);
    CHECK(fake_cs_writes == w, "key held: not written");
    release_key(5);
    run_ms(2000);
    CHECK(fake_cs_writes == w, "2 s after the release: not yet");
    run_ms(1100);
    CHECK(fake_cs_writes == w + 1 && fake_cs_get(0, P_PRESET_ACTIVE, 0, &v) && v == 2,
          "idle 3 s: preset.active 2 stored");

    set_param(P_PRESET_ACTIVE, 0, 1);
    button(2, 1);                   /* oct -: held */
    run_ms(12000);
    CHECK(fake_cs_writes == w + 1, "button held: not written");
    button(2, 0);
    run_ms(2900);
    CHECK(fake_cs_writes == w + 1, "under 3 s after the release: not yet");
    run_ms(200);
    CHECK(fake_cs_writes == w + 2 && !fake_cs_get(0, P_PRESET_ACTIVE, 0, 0),
          "then stored (preset.active 1 is the default: left out)");

    set_param(P_PRESET_ACTIVE, 0, 3);
    for (int i = 0; i < 24; i++) {  /* the volume knob turned for 12 s */
        adc[0] = (i & 1) ? 3000 : 1000;
        run_ms(500);
    }
    CHECK(fake_cs_writes == w + 2, "knob moving: not written");
    run_ms(2000);
    CHECK(fake_cs_writes == w + 2, "2 s after it stopped: not yet");
    out_n = 0;
    adc[0] -= 3;                    /* noise: too small to send */
    run_ms(1100);
    CHECK(count_msgs(0xB0) == 0 && fake_cs_writes == w + 3 && fake_cs_get(0, P_PRESET_ACTIVE, 0, &v) && v == 3,
          "idle 3 s (noise doesn't count): stored");

    set_param(P_PRESET_ACTIVE, 0, 2);
    run_ms(1000);
    set_param(P_PRESET_ACTIVE, 0, 3);
    run_ms(9500);
    CHECK(fake_cs_writes == w + 3, "the delay counts from the last switch");
    run_ms(600);
    CHECK(fake_cs_writes == w + 3, "back on the stored slot: nothing to write");

    set_param(P_PRESET_ACTIVE, 0, 1);
    CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK && fake_cs_writes == w + 4, "COMMIT stores it");
    run_ms(15000);
    CHECK(fake_cs_writes == w + 4, "and nothing is left pending");
    set_param(P_PRESET_ACTIVE, 0, 3);
    run_ms(13100);
    CHECK(fake_cs_writes == w + 5 && fake_cs_get(0, P_PRESET_ACTIVE, 0, &v) && v == 3, "slot 3 again");
}
#endif

static void test_presets(void)
{
    int d1, d2, slot, dirty;
    int32_t v;
    const param_def_t *act = params_find(P_BTN_ACTION);

    /* schema: actions 0..15 on a device without mcu:, scope flags */
    CHECK(act->option_count == 16 && act->max == 15 && ACT_PRESET_SELECT == 15 && ACT_MCU == 16,
          "button.action options 0..15 (got %d)", act->option_count);
    CHECK((act->flags & PF_PRESET) && (params_find(P_CHANNEL)->flags & PF_PRESET) &&
          (params_find(P_PRESET_PROGRAM)->flags & PF_PRESET) &&
          !(params_find(P_VEL_FAST)->flags & PF_PRESET) &&
          !(params_find(P_PRESET_ACTIVE)->flags & PF_PRESET) &&
          !(params_find(P_LOG_LEVEL)->flags & PF_PRESET), "PF_PRESET scope");
    CHECK(params_find(P_PRESET_ACTIVE)->max == GM_N_PRESETS && GM_N_PRESETS == 8, "preset.active 1..8");
    CHECK(set_param(P_PRESET_DIRTY, 0, 1) == GSX_BAD_PARAM, "preset.dirty is read-only");
    CHECK(gm_cfg.preset_active == 1 && fake_cs_count(0) == -1, "slot 1 after reset, nothing stored");

    /* switch releases held notes, CC-momentary, then loads slot 2 (Drums) */
    press_key(0, 500);
    button(0, 1);                    /* sustain held: CC 64 127 */
    out_n = 0;
    CHECK(set_param(P_PRESET_ACTIVE, 0, 2) == GSX_OK, "SET preset.active 2");
    CHECK(find_msg(0x80, &d1, 0) && d1 == 48, "switch: note off 48");
    CHECK(find_msg(0xB0, &d1, &d2) && d1 == 64 && d2 == 0, "switch: sustain released");
    CHECK(gm_cfg.channel == 10 && gm_cfg.vel_curve == CURVE_HARD && gm_cfg.btn_action[0] == ACT_NOTE &&
          gm_cfg.btn_number[0] == 36 && gm_cfg.btn_action[6] == ACT_NOTE && gm_cfg.btn_number[6] == 38,
          "slot 2 YAML values");
    CHECK(preset_changed_msg(&slot, &dirty) && slot == 2 && dirty == 0, "PRESET_CHANGED 2");
    CHECK(find_sysex(GSX_MSG_PARAM_CHANGED, 0) < 0, "no PARAM_CHANGED for the host's own switch");
    CHECK(count_msgs(0xC0) == 0, "no program change (none set)");
    release_key(0);
    CHECK(count_msgs(0x80) == 0, "held key's release: nothing more");
    out_n = 0;
    button(0, 0);
    CHECK(out_n == 0 || (count_msgs(0x80) == 0 && count_msgs(0x90) == 0 && count_msgs(0xB0) == 0),
          "held sustain's release after the switch is silent");
    out_n = 0;
    button(0, 1);
    CHECK(find_msg(0x99, &d1, 0) && d1 == 36, "sustain now plays note 36 on channel 10");
    button(0, 0);
    CHECK(find_msg(0x89, &d1, 0) && d1 == 36, "and its note off");
    CHECK(set_param(P_PRESET_ACTIVE, 0, 9) == GSX_BAD_VALUE, "preset.active 9 -> BAD_VALUE");
    set_param(P_CHANNEL, 0, 4);
    out_n = 0;
    CHECK(set_param(P_PRESET_ACTIVE, 0, 2) == GSX_OK && gm_cfg.channel == 10 && gm_cfg.preset_dirty == 0 &&
          preset_changed_msg(&slot, &dirty) && slot == 2 && dirty == 0,
          "selecting the active slot reloads it (edits dropped)");

    /* octave: live value, reset by the switch to the preset's value */
    button(3, 1); button(3, 0);
    CHECK(gm_cfg.octave == 1 && gm_cfg.preset_dirty == 0, "oct+ on slot 2 (not a host edit)");
    out_n = 0;
    set_param(P_PRESET_ACTIVE, 0, 1);
    CHECK(gm_cfg.octave == 0 && gm_cfg.channel == 1 && gm_cfg.btn_action[0] == ACT_CC_MOMENTARY,
          "back to slot 1: octave 0, defaults");

    /* dirty: host edits of per-preset params only */
    CHECK(set_param(P_VEL_FAST, 0, 30) == GSX_OK && gm_cfg.preset_dirty == 0, "global edit: not dirty");
    out_n = 0;
    CHECK(set_param(P_CHANNEL, 0, 3) == GSX_OK && gm_cfg.preset_dirty == 1, "channel edit: dirty");
    CHECK(preset_changed_msg(&slot, &dirty) && slot == 1 && dirty == 1, "PRESET_CHANGED dirty");
    CHECK(get_param(P_PRESET_DIRTY, 0, &v) == GSX_OK && v == 1, "GET preset.dirty");
    out_n = 0;
    set_param(P_BTN_NUMBER, 6, 21);
    CHECK(find_sysex(GSX_MSG_PRESET_CHANGED, 0) < 0, "dirty already set: no second PRESET_CHANGED");

    /* COMMIT: globals in slot 0, slot 1's diff vs its YAML values */
    out_n = 0;
    CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK && gm_cfg.preset_dirty == 0, "COMMIT clears dirty");
    CHECK(fake_cs_count(1) == 2 && fake_cs_get(1, P_CHANNEL, 0, &v) && v == 3 &&
          fake_cs_get(1, P_BTN_NUMBER, 6, &v) && v == 21, "slot 1 record = 2 changed values (%d)",
          fake_cs_count(1));
    CHECK(fake_cs_count(0) == 1 && fake_cs_get(0, P_VEL_FAST, 0, &v) && v == 30 &&
          !fake_cs_get(0, P_PRESET_ACTIVE, 0, 0) && !fake_cs_get(0, P_CHANNEL, 0, 0) &&
          !fake_cs_get(0, P_PRESET_DIRTY, 0, 0),
          "slot 0 = the globals that differ from their defaults (not preset.active 1, not dirty)");
    CHECK(fake_cs_revision(1) == GM_PRESETS_REVISION, "record revision");

    /* a COMMIT with nothing new writes nothing (flash wear) */
    int wc = fake_cs_writes;
    CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK && fake_cs_writes == wc, "unchanged COMMIT: no writes");
    set_param(P_VEL_FAST, 0, GM_VEL_FAST_DEFAULT);
    CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK && fake_cs_writes == wc + 1 && fake_cs_count(0) == 0 &&
          fake_cs_count(1) == 2, "globals back to defaults: an empty slot 0, slot 1 not rewritten");
    CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK && fake_cs_writes == wc + 1, "and again: no writes");
    set_param(P_VEL_FAST, 0, 30);
    CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK && fake_cs_writes == wc + 2 && fake_cs_count(0) == 1,
          "velocity.fast 30 again: only slot 0 rewritten");

    /* slot 2: a value equal to its YAML value is not stored */
    set_param(P_PRESET_ACTIVE, 0, 2);
    set_param(P_CHANNEL, 0, 5);
    set_param(P_BTN_NUMBER, 0, 40);
    set_param(P_PRESET_PROGRAM, 0, 5);
    set_param(P_CHANNEL, 0, 10);     /* back to the YAML value */
    CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK, "COMMIT slot 2");
    CHECK(fake_cs_count(2) == 2 && fake_cs_get(2, P_BTN_NUMBER, 0, &v) && v == 40 &&
          fake_cs_get(2, P_PRESET_PROGRAM, 0, &v) && v == 5 && !fake_cs_get(2, P_CHANNEL, 0, 0),
          "slot 2 record: button.number[0], preset.program (got %d)", fake_cs_count(2));

    /* boot: globals, then defaults -> YAML -> stored for the active slot:
     * the stored preset.active with write_to_flash.last_preset, else slot 1 */
    gm_cfg.vel_fast = 99;
    boot();
#if !GM_WRITE_LAST_PRESET
    CHECK(fake_cs_get(0, P_PRESET_ACTIVE, 0, &v) && v == 2 && gm_cfg.preset_active == 1 &&
          gm_cfg.vel_fast == 30 && gm_cfg.channel == 3 && gm_cfg.btn_number[6] == 21,
          "last_preset off: boot ignores the stored preset.active 2 (slot 1 + stored, globals)");
    set_param(P_PRESET_ACTIVE, 0, 2);
    out_n = 0;
#endif
    CHECK(gm_cfg.preset_active == 2 && gm_cfg.vel_fast == 30 && gm_cfg.channel == 10 &&
          gm_cfg.vel_curve == CURVE_HARD && gm_cfg.btn_number[0] == 40 && gm_cfg.btn_number[6] == 38 && gm_cfg.preset_program == 5 && gm_cfg.preset_dirty == 0,
          "boot loads slot 2: YAML + stored, globals");
    CHECK(count_msgs(0xC0) == 0, "no program change at boot");
    gm_init(0x00010000u, 1);         /* safe mode: store ignored */
    CHECK(gm_cfg.preset_active == 1 && gm_cfg.vel_fast == GM_VEL_FAST_DEFAULT && gm_cfg.channel == 1,
          "safe mode: defaults, slot 1");
    boot();

    /* unsaved edits are dropped by a switch; Program Change on select */
    set_param(P_BTN_NUMBER, 0, 50);
    set_param(P_PRESET_ACTIVE, 0, 1);
    CHECK(gm_cfg.channel == 3 && gm_cfg.btn_number[6] == 21, "slot 1 with its stored values");
    out_n = 0;
    set_param(P_PRESET_ACTIVE, 0, 2);
    CHECK(gm_cfg.btn_number[0] == 40, "slot 2 edit discarded");
    CHECK(has_bytes((const uint8_t[]){ 0xC9, 5 }, 2), "program change 5 on channel 10");

    /* actions 13-15 from buttons; PARAM_CHANGED, debounced preset.active */
    set_param(P_BTN_ACTION, 6, ACT_PRESET_NEXT);     /* mod button */
    out_n = 0;
    int w0 = fake_cs_writes;
    button(6, 1);
    CHECK(gm_cfg.preset_active == 3 && gm_cfg.btn_action[6] == ACT_CC_TOGGLE && gm_cfg.analog_cc[0] == 11,
          "preset_next -> slot 3 (DAW)");
    CHECK(find_sysex(GSX_MSG_PARAM_CHANGED, 0) >= 0 && preset_changed_msg(&slot, &dirty) && slot == 3,
          "PARAM_CHANGED + PRESET_CHANGED from the device");
    out_n = 0;
    button(6, 0);
    CHECK(count_msgs(0xB0) == 0, "held preset button's release: no CC 82");
#if GM_WRITE_LAST_PRESET
    CHECK(GM_WRITE_DELAY_MS == 10000u && GM_WRITE_IDLE_MS == 3000u, "write_to_flash delay 10 s, idle 3 s");
    run_ms(5000);
    CHECK(fake_cs_writes == w0, "preset.active not written yet");
    run_ms(5100);
    CHECK(fake_cs_writes == w0 + 1 && fake_cs_get(0, P_PRESET_ACTIVE, 0, &v) && v == 3 &&
          fake_cs_get(0, P_VEL_FAST, 0, &v) && v == 30, "preset.active stored after 10 s (globals kept)");
    run_ms(11000);
    CHECK(fake_cs_writes == w0 + 1, "written once");
    test_write_idle();
#else
    run_ms(25000);
    CHECK(fake_cs_writes == w0 && fake_cs_get(0, P_PRESET_ACTIVE, 0, &v) && v == 2,
          "last_preset off: a switch on the device never writes");
    set_param(P_PRESET_ACTIVE, 0, 3);
    run_ms(25000);
    CHECK(fake_cs_writes == w0, "nor one from the host");
#endif
    set_param(P_BTN_ACTION, 2, ACT_PRESET_PREV);
    set_param(P_BTN_ACTION, 3, ACT_PRESET_SELECT);
    set_param(P_BTN_NUMBER, 3, 1);
    button(2, 1); button(2, 0);
    CHECK(gm_cfg.preset_active == 2, "preset_prev -> 2");
    /* 3 presets in use (the YAML's list): stepping wraps over those, and
     * slots 4..8 are free until a preset is made there */
    CHECK(gm_cfg.preset_count == 3 && params_find(P_PRESET_COUNT)->flags == PF_READ_ONLY,
          "preset.count 3, read-only to a host");
    CHECK(set_param(P_PRESET_COUNT, 0, 5) == GSX_BAD_PARAM, "SET preset.count -> BAD_PARAM");
    set_param(P_PRESET_ACTIVE, 0, 1);
    set_param(P_BTN_ACTION, 2, ACT_PRESET_PREV);
    button(2, 1); button(2, 0);
    CHECK(gm_cfg.preset_active == 3, "preset_prev wraps 1 -> 3");
    set_param(P_BTN_ACTION, 3, ACT_PRESET_SELECT);
    set_param(P_BTN_NUMBER, 3, 2);
    button(3, 1); button(3, 0);
    CHECK(gm_cfg.preset_active == 2, "preset_select 2");
    CHECK(set_param(P_PRESET_ACTIVE, 0, 8) == GSX_BAD_VALUE && gm_cfg.preset_active == 2,
          "preset.active 8: a free slot -> BAD_VALUE");
    set_param(P_PRESET_ACTIVE, 0, 3);
    set_param(P_BTN_ACTION, 0, ACT_PRESET_SELECT);
    set_param(P_BTN_NUMBER, 0, 5);
    button(0, 1); button(0, 0);
    CHECK(gm_cfg.preset_active == 3, "preset_select 5 (free): ignored");
    set_param(P_BTN_ACTION, 0, ACT_PRESET_NEXT);
    button(0, 1); button(0, 0);
    CHECK(gm_cfg.preset_active == 1, "preset_next wraps 3 -> 1");

    /* PRESET_INFO */
    pinfo_t in;
    CHECK(preset_info(0, &in) == GSX_OK && in.slot == 1 && in.count == 8 && in.active == 1 &&
          in.flags == (PRESET_INFO_STORED | PRESET_INFO_YAML | PRESET_INFO_IN_USE) &&
          !strcmp(in.name, "Default") && in.program == -1, "PRESET_INFO 0 = active (flags %d, %s)", in.flags,
          in.name);
    CHECK(preset_info(2, &in) == GSX_OK && in.program == 5 && !strcmp(in.name, "Drums") &&
          in.flags == (PRESET_INFO_STORED | PRESET_INFO_YAML | PRESET_INFO_IN_USE), "PRESET_INFO 2");
    CHECK(preset_info(4, &in) == GSX_OK && in.flags == 0 && !strcmp(in.name, "Preset 4") &&
          in.stored_revision == 0, "PRESET_INFO 4: empty slot (flags %d)", in.flags);
    CHECK(preset_info(9, &in) == GSX_BAD_PARAM, "PRESET_INFO 9 -> BAD_PARAM");
    set_param(P_CHANNEL, 0, 4);
    CHECK(preset_info(1, &in) == GSX_OK && (in.flags & PRESET_INFO_DIRTY), "dirty flag");

    /* PRESET_NAME */
    CHECK(preset_name(4, "Lead") == GSX_OK && preset_info(4, &in) == GSX_OK && !strcmp(in.name, "Lead") &&
          in.flags == (PRESET_INFO_STORED | PRESET_INFO_IN_USE) && fake_cs_count(4) == 0,
          "naming free slot 4 makes it a preset: a copy of preset 1, named");
    CHECK(gm_cfg.preset_count == 4 && fake_cs_get(0, P_PRESET_COUNT, 0, &v) && v == 4 && gm_cfg.preset_dirty,
          "4 presets in use, stored; the active one's edits are still unsaved");
    CHECK(preset_name(2, "Kit") == GSX_OK && fake_cs_count(2) == 2 && !strcmp(fake_cs_name(2), "Kit"),
          "rename keeps the stored values");
    CHECK(preset_name(4, "seventeen chars!!") == GSX_BAD_VALUE, "name > 16 -> BAD_VALUE");
    CHECK(preset_name(4, "~Pad 2!") == GSX_OK, "printable ASCII");
    {
        uint8_t p[8];
        int n = put_u(p, 4, 2);
        p[n++] = 1;
        p[n++] = 0x09;
        CHECK(request(GSX_CMD_PRESET_NAME, p, n, 0) == GSX_BAD_VALUE, "control char -> BAD_VALUE");
        p[2] = 3;
        CHECK(request(GSX_CMD_PRESET_NAME, p, n, 0) == GSX_BAD_LEN, "name length mismatch -> BAD_LEN");
    }
    CHECK(preset_name(0, "x") == GSX_BAD_PARAM && preset_name(9, "x") == GSX_BAD_PARAM, "bad slot");
    CHECK(preset_name(4, "") == GSX_OK && fake_cs_count(4) == -1 && preset_info(4, &in) == GSX_OK &&
          !strcmp(in.name, "Preset 4"), "empty name: back to the YAML name");

    /* A switch shows the preset's name, read with its values (one read) */
    {
        static const struct { uint32_t slot; const char *name; } sw[] = {
            { 2, "Kit" }, { 3, "DAW" }, { 4, "Preset 4" }, { 1, "Default" },
        };
        for (uint32_t i = 0; i < sizeof sw / sizeof sw[0]; i++) {
            shown[0] = 0;
            int reads = fake_cs_reads;
            set_param(P_PRESET_ACTIVE, 0, (int32_t)sw[i].slot);
            CHECK(gm_cfg.preset_active == (int32_t)sw[i].slot && !strcmp(shown, sw[i].name) &&
                  fake_cs_reads - reads == 1, "switch to %u: shows '%s' (got '%s'), %d store reads (1)",
                  (unsigned)sw[i].slot, sw[i].name, shown, fake_cs_reads - reads);
        }
    }

    /* PRESET_SAVE (save as) from slot 3 with edits */
    set_param(P_PRESET_ACTIVE, 0, 3);
    set_param(P_ANALOG_CC, 0, 12);
    out_n = 0;
    CHECK(preset_cmd(GSX_CMD_PRESET_SAVE, 5, 0) == GSX_OK, "PRESET_SAVE 5");
    CHECK(gm_cfg.preset_active == 5 && gm_cfg.preset_dirty == 0 && preset_changed_msg(&slot, &dirty) &&
          slot == 5 && dirty == 0, "now on slot 5, clean");
    CHECK(gm_cfg.preset_count == 5, "saving into free slot 5 puts it in use");
    CHECK(fake_cs_get(5, P_ANALOG_CC, 0, &v) && v == 12 && fake_cs_get(5, P_BTN_ACTION, 6, &v) &&
          v == ACT_CC_TOGGLE && fake_cs_count(3) == -1 && fake_cs_get(0, P_PRESET_ACTIVE, 0, &v) && v == 5,
          "slot 5 = DAW + edit as a diff vs defaults; slot 3 untouched; active stored");
    CHECK(preset_cmd(GSX_CMD_PRESET_SAVE, 0, 0) == GSX_BAD_PARAM &&
          preset_cmd(GSX_CMD_PRESET_SAVE, 9, 0) == GSX_BAD_PARAM, "PRESET_SAVE bad slot");

    /* PRESET_RESET */
    out_n = 0;
    CHECK(preset_cmd(GSX_CMD_PRESET_RESET, 5, 0) == GSX_OK && fake_cs_count(5) == -1, "PRESET_RESET 5");
    CHECK(gm_cfg.analog_cc[0] == 7 && gm_cfg.btn_action[6] == ACT_CC_RAMP, "active slot 5 reloaded");
    CHECK(preset_cmd(GSX_CMD_PRESET_RESET, 9, 0) == GSX_BAD_PARAM, "PRESET_RESET 9");
    CHECK(preset_cmd(GSX_CMD_PRESET_RESET, 0, 0) == GSX_OK && fake_cs_count(1) == -1 &&
          fake_cs_count(2) == -1 && fake_cs_count(0) >= 0, "PRESET_RESET 0: all presets, not the globals");
    CHECK(gm_cfg.preset_count == 3 && gm_cfg.preset_active == 1 && !fake_cs_get(0, P_PRESET_COUNT, 0, 0),
          "PRESET_RESET 0: the YAML's 3 presets in use, slot 1 active");

    /* PRESET_DELETE: the later presets move up, the last slot is freed */
    preset_name(4, "Lead");
    set_param(P_PRESET_ACTIVE, 0, 2);
    out_n = 0;
    CHECK(preset_cmd(GSX_CMD_PRESET_DELETE, 2, 0) == GSX_OK && gm_cfg.preset_count == 3, "PRESET_DELETE 2");
    CHECK(preset_changed_msg(&slot, &dirty) && slot == 2 && dirty == 0, "PRESET_CHANGED");
    CHECK(preset_info(2, &in) == GSX_OK && !strcmp(in.name, "DAW") && preset_info(3, &in) == GSX_OK &&
          !strcmp(in.name, "Lead") && preset_info(4, &in) == GSX_OK && in.flags == 0 && fake_cs_count(4) == -1,
          "DAW moved to 2, Lead to 3, slot 4 free");
    CHECK(gm_cfg.preset_active == 2 && gm_cfg.btn_action[6] == ACT_CC_TOGGLE && gm_cfg.analog_cc[0] == 11 &&
          gm_cfg.channel == 1, "the deleted active preset hands over to DAW, now in its slot");
    set_param(P_PRESET_ACTIVE, 0, 3);
    CHECK(gm_cfg.channel == 1 && gm_cfg.btn_action[6] == ACT_CC_RAMP && gm_cfg.analog_cc[0] == 7,
          "slot 3: Lead's values (a copy of Default)");
    /* PRESET_MOVE: Default, DAW, Lead (active, made distinct) */
    set_param(P_ANALOG_CC, 0, 21);
    request(GSX_CMD_COMMIT, 0, 0, 0);
    {
        uint8_t p[4];
        put_u(p, 3, 2);
        put_u(p + 2, 1, 2);
        out_n = 0;
        CHECK(request(GSX_CMD_PRESET_MOVE, p, 4, 0) == GSX_OK, "PRESET_MOVE 3 -> 1");
        CHECK(preset_changed_msg(&slot, &dirty) && slot == 1 && dirty == 0, "PRESET_CHANGED: Lead, now slot 1");
        CHECK(gm_cfg.preset_active == 1 && gm_cfg.analog_cc[0] == 21, "the active Lead keeps playing, in slot 1");
        CHECK(preset_info(1, &in) == GSX_OK && !strcmp(in.name, "Lead") && preset_info(2, &in) == GSX_OK &&
              !strcmp(in.name, "Default") && preset_info(3, &in) == GSX_OK && !strcmp(in.name, "DAW"),
              "order: Lead, Default, DAW");
        set_param(P_PRESET_ACTIVE, 0, 3);
        CHECK(gm_cfg.analog_cc[0] == 11 && gm_cfg.btn_action[6] == ACT_CC_TOGGLE, "slot 3: DAW's values");
        set_param(P_PRESET_ACTIVE, 0, 2);
        CHECK(gm_cfg.analog_cc[0] == 7 && gm_cfg.channel == 1, "slot 2: Default's values");
        put_u(p, 1, 2);
        put_u(p + 2, 3, 2);
        CHECK(request(GSX_CMD_PRESET_MOVE, p, 4, 0) == GSX_OK && gm_cfg.preset_active == 1 &&
              gm_cfg.analog_cc[0] == 7, "PRESET_MOVE 1 -> 3: the active Default shifts up to 1");
        CHECK(preset_info(1, &in) == GSX_OK && !strcmp(in.name, "Default") && preset_info(2, &in) == GSX_OK &&
              !strcmp(in.name, "DAW") && preset_info(3, &in) == GSX_OK && !strcmp(in.name, "Lead"),
              "order: Default, DAW, Lead again");
        CHECK(!fake_cs_name(1) || !fake_cs_name(1)[0], "Default back in slot 1 stores no name of its own");
        put_u(p + 2, 4, 2);
        CHECK(request(GSX_CMD_PRESET_MOVE, p, 4, 0) == GSX_BAD_PARAM, "PRESET_MOVE to a free slot -> BAD_PARAM");
        CHECK(request(GSX_CMD_PRESET_MOVE, p, 2, 0) == GSX_BAD_LEN, "PRESET_MOVE 2 bytes -> BAD_LEN");
        put_u(p + 2, 1, 2);
        CHECK(request(GSX_CMD_PRESET_MOVE, p, 4, 0) == GSX_OK, "PRESET_MOVE to itself: OK, nothing moves");
    }
    set_param(P_PRESET_ACTIVE, 0, 3);
    CHECK(preset_cmd(GSX_CMD_PRESET_DELETE, 4, 0) == GSX_BAD_PARAM &&
          preset_cmd(GSX_CMD_PRESET_DELETE, 0, 0) == GSX_BAD_PARAM, "PRESET_DELETE a free slot or 0 -> BAD_PARAM");
    CHECK(preset_cmd(GSX_CMD_PRESET_DELETE, 1, 0) == GSX_OK && gm_cfg.preset_active == 2 &&
          preset_info(1, &in) == GSX_OK && !strcmp(in.name, "DAW") && preset_info(2, &in) == GSX_OK &&
          !strcmp(in.name, "Lead") && gm_cfg.preset_count == 2, "PRESET_DELETE 1: the active Lead moves up to 2");
    CHECK(preset_cmd(GSX_CMD_PRESET_DELETE, 2, 0) == GSX_OK && gm_cfg.preset_active == 1 &&
          gm_cfg.preset_count == 1 && fake_cs_get(0, P_PRESET_COUNT, 0, &v) && v == 1,
          "PRESET_DELETE the last one: back to the one before");
    CHECK(preset_cmd(GSX_CMD_PRESET_DELETE, 1, 0) == GSX_BAD_STATE, "the last preset stays");
    button(0, 1); button(0, 0);
    CHECK(gm_cfg.preset_active == 1, "preset_next with one preset: stays");
    boot();
    CHECK(gm_cfg.preset_count == 1 && gm_cfg.preset_active == 1 && preset_info(1, &in) == GSX_OK &&
          !strcmp(in.name, "DAW") && gm_cfg.analog_cc[0] == 11, "boot: 1 preset in use, DAW");

    /* a store from before preset.count: presets stored past it stay in use */
    CHECK(preset_cmd(GSX_CMD_PRESET_SAVE, 6, 0) == GSX_OK && gm_cfg.preset_count == 6, "save into 6");
    config_store_write(CS_SLOT_GLOBAL, GM_PRESETS_REVISION, 0, 0, 0);
    boot();
    CHECK(gm_cfg.preset_count == 6 && gm_cfg.preset_active == 1, "boot: slot 6 is stored, so 6 in use");

    /* FACTORY_RESET: store erased, slot 1 */
    CHECK(unlocked_request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_OK && gm_cfg.preset_active == 1 &&
          gm_cfg.vel_fast == GM_VEL_FAST_DEFAULT && fake_cs_count(0) == -1, "FACTORY_RESET");

    /* a failing store: COMMIT reports it, dirty stays */
    set_param(P_CHANNEL, 0, 2);
    fake_cs_fail = 1;
    CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_FLASH_ERR && gm_cfg.preset_dirty == 1, "COMMIT flash error");
    fake_cs_fail = 0;
    unlocked_request(GSX_CMD_FACTORY_RESET, 0, 0, 0);
}

/* ---- modes (the Keystation's edit mode, devices/keystation-mini32-mk3.yaml) ---- */

/* Keys by note at octave 0: the lowest key is C3 = 48. */
enum { K_C3 = 0, K_CS3 = 1, K_D3 = 2, K_E3 = 4, K_F3 = 5, K_G3 = 7, K_A3 = 9, K_B3 = 11, K_C4 = 12,
       K_CS4 = 13, K_D4 = 14, K_E4 = 16, K_E5 = 28 };
#define EDIT 1
#define EDIT_LED_LIT (level[13] == 0)        /* PA13, active-low */
#define OCT_DOWN_LIT (level[10] == 0)        /* PA10: Oct - colour A */
#define OCT_UP_LIT (level[11] == 0)          /* PA11: Oct + colour A */

/* PARAM_CHANGED for this param in the output. */
static int param_changed_msg(uint16_t id)
{
    for (int i = 0; i + GSX_OVERHEAD < out_n; i++)
        if (out[i] == 0xF0 && out[i + 1 + GSX_OFF_CMD] == GSX_MSG_PARAM_CHANGED &&
            gsx_get_u(out + i + 1 + GSX_OFF_PAYLOAD, 2) == id)
            return 1;
    return 0;
}

/* Tap a key: press (out = what the press sent), release. */
static void tap(int k)
{
    press_key(k, 500);
    int n = out_n;
    uint8_t keep[256];
    memcpy(keep, out, (size_t)(n < 256 ? n : 256));
    release_key(k);
    memcpy(out, keep, (size_t)(n < 256 ? n : 256));
    out_n = n < 256 ? n : 256;
}

static void edit(void)
{
    out_n = 0;
    button(EDIT, 1);
    button(EDIT, 0);
}

static void test_modes(void)
{
    int d1, d2, slot, dirty;
    CHECK(GM_N_MODES == 3, "3 modes (edit, preset_pick, channel_pick)");
    CHECK(gm_cfg.preset_active == 1 && gm_cfg.channel == 1 && gm_cfg.btn_action[EDIT] == ACT_NONE,
          "slot 1, Edit has no action of its own");

    /* entering the mode releases the held key; Edit itself sends nothing */
    press_key(K_C3, 500);
    CHECK(find_msg(0x90, &d1, 0) && d1 == 48, "C3 plays before the mode");
    run_ms(20);
    CHECK(!EDIT_LED_LIT, "Edit LED off while playing");
    out_n = 0;
    button(EDIT, 1);
    CHECK(find_msg(0x80, &d1, 0) && d1 == 48 && count_msgs(0xB0) == 0 && count_msgs(0x90) == 0,
          "Edit: Note Off for the held key, no CC");
    button(EDIT, 0);
    CHECK(count_msgs(0xB0) == 0, "Edit release: nothing");
    run_ms(20);
    CHECK(EDIT_LED_LIT, "Edit LED lit in the mode");
    release_key(K_C3);
    CHECK(count_msgs(0x80) == 0 && count_msgs(0x90) == 0, "the held key's release: nothing more");

    /* unbound keys are silent; buttons keep their actions */
    tap(K_B3);
    CHECK(out_n == 0, "unbound B3: nothing at all (got %d bytes)", out_n);
    out_n = 0;
    button(0, 1);
    CHECK(find_msg(0xB0, &d1, &d2) && d1 == 64 && d2 == 127, "sustain still sends CC 64 in the mode");
    button(0, 0);

    /* E3, then a white key from D4: preset 1..8; E4 = preset 2 */
    tap(K_E3);
    CHECK(count_msgs(0x90) == 0 && find_sysex(GSX_MSG_PRESET_CHANGED, 0) < 0, "E3: no note, no switch yet");
    int on = 0, off = 0;
    for (int i = 0; i < 60; i++) {
        run_ms(10);
        if (EDIT_LED_LIT)
            on++;
        else
            off++;
    }
    CHECK(on > 15 && off > 15, "Edit LED blinks in the sub-mode (%d on, %d off)", on, off);
    tap(K_E4);
    CHECK(gm_cfg.preset_active == 2 && gm_cfg.channel == 10, "E4 picks preset 2 (Drums)");
    CHECK(preset_changed_msg(&slot, &dirty) && slot == 2 && dirty == 0 && param_changed_msg(P_PRESET_ACTIVE),
          "PRESET_CHANGED 2 and PARAM_CHANGED preset.active");
    CHECK(count_msgs(0x90) == 0 && count_msgs(0x80) == 0, "no notes from the pick");

    /* then Oct - blinks twice, and both octave LEDs flash once */
    {
        int blinks = 0, was = 0, up_on = 0, both = 0;
        for (int i = 0; i < 150; i++) {
            run_ms(10);
            if (OCT_DOWN_LIT && !was)
                blinks++;
            was = OCT_DOWN_LIT;
            up_on += OCT_UP_LIT;
            both += OCT_DOWN_LIT && OCT_UP_LIT;
        }
        CHECK(blinks == 3 && both >= 30 && up_on == both, "preset 2 shown: 2 blinks + the end flash "
              "(%d on-periods, %d both, %d up)", blinks, both, up_on);
        CHECK(!OCT_DOWN_LIT && !OCT_UP_LIT && EDIT_LED_LIT, "then the octave LEDs are theirs again, Edit lit");
    }

    /* back in edit mode after the pick: D4 is unbound there, G3 is preset_next */
    tap(K_D4);
    CHECK(out_n == 0 && gm_cfg.preset_active == 2, "back in edit mode: D4 does nothing");
    tap(K_G3);
    CHECK(gm_cfg.preset_active == 3 && preset_changed_msg(&slot, &dirty) && slot == 3, "G3: next preset (3)");
    tap(K_F3);
    tap(K_F3);
    CHECK(gm_cfg.preset_active == 1, "F3 twice: previous preset (1)");

    /* A3, then C#4: channel 2 (a device-side edit: PARAM_CHANGED, the preset is dirty) */
    tap(K_A3);
    tap(K_CS4);
    CHECK(gm_cfg.channel == 2 && param_changed_msg(P_CHANNEL), "C#4 in channel_pick: channel 2");
    CHECK(preset_changed_msg(&slot, &dirty) && slot == 1 && dirty == 1 && gm_cfg.preset_dirty == 1,
          "channel change makes the preset dirty");
    tap(K_C4);
    CHECK(gm_cfg.channel == 2 && out_n == 0, "back in edit mode: C4 does nothing");

    /* octave and curve keys */
    tap(K_CS3);
    tap(K_CS3);
    CHECK(gm_cfg.octave == -2 && param_changed_msg(P_OCTAVE), "C#3 twice: octave -2");
    tap(K_C3);
    CHECK(gm_cfg.octave == 0, "C3: octave reset");
    tap(K_E5);
    CHECK(gm_cfg.vel_curve == CURVE_HARD && param_changed_msg(P_VEL_CURVE), "E5: velocity curve hard");

    /* Edit again: keys play again, on the new channel */
    edit();
    run_ms(20);
    CHECK(!EDIT_LED_LIT, "Edit LED off after the mode");
    press_key(K_C3, 500);
    CHECK(find_msg(0x91, &d1, 0) && d1 == 48, "C3 plays again, on channel 2");
    release_key(K_C3);
    CHECK(find_msg(0x81, &d1, 0) && d1 == 48, "and its Note Off");

    /* Edit leaves from a sub-mode too */
    edit();
    tap(K_E3);
    edit();
    press_key(K_E4, 500);
    CHECK(find_msg(0x91, &d1, 0) && d1 == 64, "Edit in preset_pick leaves the mode: E4 plays");
    release_key(K_E4);
    CHECK(gm_cfg.preset_active == 1, "no preset picked");

    /* a host can't give Edit an action while it is the mode button */
    set_param(P_BTN_ACTION, EDIT, ACT_CC_MOMENTARY);
    set_param(P_BTN_NUMBER, EDIT, 81);
    edit();
    CHECK(count_msgs(0xB0) == 0, "Edit with a CC action set by the host: still only the mode");
    tap(K_C3);
    CHECK(count_msgs(0x90) == 0, "in the mode");
    edit();
    unlocked_request(GSX_CMD_FACTORY_RESET, 0, 0, 0);
}

/* ---- velocity sweep ----------------------------------------------------
 * The key velocity as controller.c worked it out before the divide-free
 * time and the cached logs (0.29.0), to check the firmware still gives
 * exactly this over a sweep of key times, key-time settings, curves and
 * scan periods. */
static int32_t ref_log2_q8(uint32_t x)
{
    if (!x)
        return 0;
    int32_t msb = 31 - __builtin_clz(x);
    uint32_t frac = msb >= 8 ? (x >> (msb - 8)) & 0xFFu : (x << (8 - msb)) & 0xFFu;
    return msb * 256 + (int32_t)frac;
}

static int ref_velocity(uint32_t dt_ticks, uint32_t q4)
{
    if (gm_cfg.vel_curve == CURVE_FIXED)
        return (int)gm_cfg.vel_fixed;
    uint32_t t = q4 && dt_ticks > 0xFFFFFFFFu / q4 ? 0x0FFFFFFFu : (dt_ticks * q4) >> 4;
    int32_t fast = gm_cfg.vel_fast * 100, slow = gm_cfg.vel_slow * 100;
    if (slow <= fast)
        slow = fast + 1;
    int32_t v;
    if ((int32_t)t <= fast) {
        v = 127;
    } else if ((int32_t)t >= slow) {
        v = 1;
    } else {
        int32_t lf = ref_log2_q8((uint32_t)fast), ls = ref_log2_q8((uint32_t)slow);
        v = 127 - 126 * (ref_log2_q8(t) - lf) / (ls - lf);
    }
    if (gm_cfg.vel_curve == CURVE_SOFT)
        v = 127 - (127 - v) * (127 - v) / 127;
    else if (gm_cfg.vel_curve == CURVE_HARD)
        v = v * v / 127;
    return v < 1 ? 1 : v > 127 ? 127 : v;
}

/* Key 0 with its contacts n ticks apart: the firmware sees n + 1 (the
 * first contact is seen on the tick after it closes, the second on the
 * one it closes on). The velocity sent, -1 = no Note On. */
static int key_velocity(uint32_t n)
{
    int d1, d2;
    out_n = 0;
    contact[0][1] = 1;
    ticks(1);
    ticks(n);
    contact[0][0] = 1;
    ticks(1);
    run_ms(1);
    int v = find_msg(0x90, &d1, &d2) && d1 == 48 ? d2 : -1;
    release_key(0);
    return v;
}

static void test_velocity_sweep(void)
{
    static const int32_t times[][2] = {     /* velocity.fast, velocity.slow (0.1 ms) */
        { 12, 1200 }, { 1, 20 }, { 500, 2000 }, { 30, 600 }, { 100, 101 },
    };
    int bad = 0, n_keys = 0;
    for (uint32_t ti = 0; ti < sizeof times / sizeof times[0]; ti++) {
        set_param(P_VEL_FAST, 0, times[ti][0]);
        set_param(P_VEL_SLOW, 0, times[ti][1]);
        for (int32_t curve = CURVE_NORMAL; curve <= CURVE_HARD; curve++) {
            set_param(P_VEL_CURVE, 0, curve);
            for (uint32_t n = 0; n < 2600; n += n < 300 ? 1 : 7) {
                int got = key_velocity(n), want = ref_velocity(n + 1, gm_scan_period_us * 16u);
                n_keys++;
                if (got != want && bad++ < 5)
                    printf("  fast %d slow %d curve %d, %u ticks: velocity %d, want %d\n",
                           (int)times[ti][0], (int)times[ti][1], (int)curve, (unsigned)n + 1, got, want);
            }
        }
    }
    CHECK(bad == 0, "velocity sweep: %d keys as before, %d differ", n_keys, bad);
    /* A slowed scan (5 ms, 80000/16 us) and long key times: the time no
     * longer fits 32 bits in 1/16 us and saturates; the queue saturates
     * the ticks at 0xFFFF. */
    set_param(P_VEL_FAST, 0, 12);
    set_param(P_VEL_SLOW, 0, 1200);
    set_param(P_VEL_CURVE, 0, CURVE_SOFT);
    uint32_t period = gm_scan_period_us;
    static const uint32_t slow_scan[][2] = {    /* scan period (us), ticks */
        { 5000, 1 }, { 5000, 2 }, { 5000, 53000 }, { 5000, 53687 }, { 5000, 65534 }, { 5000, 70000 },
        { 4000, 65534 }, { 7, 65534 }, { 2, 3000 }, { 3, 2001 },
    };
    bad = 0;
    for (uint32_t i = 0; i < sizeof slow_scan / sizeof slow_scan[0]; i++) {
        gm_scan_period_us = slow_scan[i][0];
        uint32_t n = slow_scan[i][1];
        int got = key_velocity(n);
        int want = ref_velocity(n + 1 > 0xFFFFu ? 0xFFFFu : n + 1, slow_scan[i][0] * 16u);
        if (got != want && bad++ < 5)
            printf("  scan %u us, %u ticks: velocity %d, want %d\n", (unsigned)slow_scan[i][0],
                   (unsigned)n + 1, got, want);
    }
    gm_scan_period_us = period;
    CHECK(bad == 0, "velocity with slowed scans and long key times: %d differ", bad);
    unlocked_request(GSX_CMD_FACTORY_RESET, 0, 0, 0);
}

/* ---- safety: unlock, factory-reset combo, status LED, diag (gm_safety.h) ---- */

#if GM_UNLOCK
static int unlock_info(uint32_t *state, uint32_t *window, uint32_t *hold, char *names)
{
    uint8_t open = 0;
    int pl, st = request(GSX_CMD_UNLOCK, &open, 1, &pl);
    if (st != GSX_OK)
        return st;
    *state = out[pl];
    *window = out[pl + 1];
    *hold = gsx_get_u(out + pl + 2, 2);
    uint32_t n = out[pl + 4], k = 0, at = (uint32_t)pl + 5u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t len = out[at++];
        if (i)
            names[k++] = '|';
        memcpy(names + k, out + at, len);
        k += len;
        at += len;
    }
    names[k] = 0;
    return GSX_OK;
}

static int unlocked_msg(int *state)
{
    int p = find_sysex(GSX_MSG_UNLOCKED, 0);
    if (p >= 0 && state)
        *state = out[p];
    return p >= 0;
}

/* The Edit LED (PA13, active-low: level 0 = lit) over ms: how often it
 * changed. */
static int led_changes(uint32_t ms)
{
    int n = 0;
    uint32_t was = level[13];
    for (uint32_t i = 0; i < ms; i++) {
        run_ms(1);
        n += level[13] != was;
        was = level[13];
    }
    return n;
}
#endif

static void test_safety(void)
{
    int d1, d2;
    int32_t v;

#if GM_UNLOCK
    uint32_t state, window, hold;
    char names[80];
    int us = -1;

    boot();
    /* FACTORY_RESET locked: nothing happens, the window opens */
    CHECK(set_param(P_CHANNEL, 0, 5) == GSX_OK && request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK, "store something");
    CHECK(request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_LOCKED && gm_cfg.channel == 5 &&
          fake_cs_count(1) > 0, "FACTORY_RESET -> LOCKED, nothing reset");
    CHECK(unlock_info(&state, &window, &hold, names) == GSX_OK && state == 0 && window == 30 &&
          hold == GM_UNLOCK_HOLD_MS && strcmp(names, "Pitch down|Pitch up") == 0,
          "UNLOCK: locked, 30 s, hold 2 s, '%s'", names);
    {
        uint8_t bad = 2;
        CHECK(request(GSX_CMD_UNLOCK, &bad, 1, 0) == GSX_BAD_CMD && request(GSX_CMD_UNLOCK, 0, 0, 0) == GSX_BAD_LEN,
              "UNLOCK: open 2 -> BAD_CMD, no payload -> BAD_LEN");
    }
    CHECK(led_changes(1000) >= 6, "the status LED blinks fast in the window");
    /* too short a hold: still locked; the combo's buttons send nothing */
    out_n = 0;
    button(4, 1);
    button(5, 1);
    run_ms(1000);
    button(4, 0);
    button(5, 0);
    run_ms(100);
    CHECK(!find_msg(0xE0, &d1, &d2) && !unlocked_msg(0), "a short hold: no bend, not unlocked");
    CHECK(request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_LOCKED, "still locked");
    /* the whole hold: UNLOCKED, then the request goes through */
    out_n = 0;
    button(4, 1);
    button(5, 1);
    run_ms(GM_UNLOCK_HOLD_MS + 20);
    CHECK(unlocked_msg(&us) && us == 1, "UNLOCKED 1 after the hold");
    CHECK(!find_msg(0xE0, &d1, &d2), "the held combo sent no pitch bend");
    button(4, 0);
    button(5, 0);
    out_n = 0;
    run_ms(100);
    CHECK(!find_msg(0xE0, &d1, &d2), "nor its release");
    CHECK(unlock_info(&state, &window, &hold, names) == GSX_OK && state == 1 && window == 0, "UNLOCK: unlocked");
    CHECK(request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_OK && gm_cfg.channel == 1 && fake_cs_count(1) == -1,
          "FACTORY_RESET after the unlock");
    /* outside the window the combo is ordinary buttons again */
    out_n = 0;
    button(4, 1);
    run_ms(100);
    CHECK(find_msg(0xE0, &d1, &d2), "Pitch down bends again");
    button(4, 0);
    run_ms(500);
    /* idle for 5 min: locked again */
    out_n = 0;
    run_ms(300000);
    CHECK(unlocked_msg(&us) && us == 0, "UNLOCKED 0 after 5 min idle");
    CHECK(request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_LOCKED, "locked after idling");
    /* the window closes after 30 s */
    out_n = 0;
    run_ms(30100);
    CHECK(unlocked_msg(&us) && us == 0, "UNLOCKED 0: the window closed");
    CHECK(unlock_info(&state, &window, &hold, names) == GSX_OK && state == 0 && window == 0, "window closed");
    /* LOCKED doesn't complete a request: the same seq is done after the unlock */
    {
        uint8_t buf[32];
        gsx_tx_t t;
        gsx_tx_begin(&t, buf, GSX_CMD_FACTORY_RESET, ++seq & 0x7F);
        uint16_t len = gsx_tx_end(&t);
        out_n = 0;
        for (int i = 0; i < len; i++)
            gm_midi_rx_byte(buf[i]);
        int p = find_sysex(GSX_CMD_FACTORY_RESET | GSX_REPLY, 0);
        CHECK(p >= 0 && out[p] == GSX_LOCKED, "LOCKED");
        unlock_combo();
        out_n = 0;
        for (int i = 0; i < len; i++)
            gm_midi_rx_byte(buf[i]);
        p = find_sysex(GSX_CMD_FACTORY_RESET | GSX_REPLY, 0);
        CHECK(p >= 0 && out[p] == GSX_OK, "the same seq again after the unlock: done");
    }
    /* UNLOCK open 1 opens the window */
    boot();
    {
        uint8_t open = 1;
        int pl;
        CHECK(request(GSX_CMD_UNLOCK, &open, 1, &pl) == GSX_OK && out[pl] == 0 && out[pl + 1] == 30,
              "UNLOCK open 1: the window opens");
    }
    unlock_combo();
    CHECK(request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_OK, "unlocked before the request");
#else
    /* unlock off (no unlock: in the YAML): nothing waits for the device */
    boot();
    CHECK(set_param(P_CHANNEL, 0, 5) == GSX_OK && request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK, "store something");
    CHECK(request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_OK && gm_cfg.channel == 1 && fake_cs_count(1) == -1,
          "unlock off: FACTORY_RESET goes through at once");
    {
        uint8_t open = 0;
        CHECK(request(GSX_CMD_UNLOCK, &open, 1, 0) == GSX_BAD_CMD, "unlock off: UNLOCK -> BAD_CMD");
    }
    out_n = 0;
    button(4, 1);
    run_ms(100);
    CHECK(find_msg(0xE0, &d1, &d2), "unlock off: Pitch down bends");
    button(4, 0);
    run_ms(100);
#endif

    /* diag.* (sysex.md 4.10) */
    boot();
    CHECK(get_param(P_DIAG_RESET, 0, &v) == GSX_OK && v == GM_RESET_UNKNOWN, "diag.reset_reason (weak HAL: unknown)");
    CHECK(get_param(P_DIAG_SETTINGS, 0, &v) == GSX_OK && v == GM_SETTINGS_NONE, "diag.settings: none stored");
    CHECK(get_param(P_DIAG_UPTIME, 0, &v) == GSX_OK && v == (int32_t)(now_ms / 1000u), "diag.uptime %d", (int)v);
    CHECK(get_param(P_DIAG_SCAN_PERIOD, 0, &v) == GSX_OK && v == (int32_t)gm_device.scan_us, "diag.scan_period");
    CHECK(set_param(P_DIAG_UPTIME, 0, 0) == GSX_BAD_PARAM, "diag.* are read-only");
    gm_diag_scan_us(77);
    gm_diag_loop_us(1234);
    gm_diag_loop_us(5);
    run_ms(1);
    CHECK(get_param(P_DIAG_SCAN_MAX, 0, &v) == GSX_OK && v == 77 && get_param(P_DIAG_LOOP_MAX, 0, &v) == GSX_OK &&
          v == 1234, "diag.scan_max, diag.loop_max keep the longest");
    CHECK(unlocked_request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_OK && get_param(P_DIAG_LOOP_MAX, 0, &v) == GSX_OK &&
          v == 1234, "a FACTORY_RESET doesn't clear them");
    CHECK(set_param(P_CHANNEL, 0, 3) == GSX_OK && request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK, "store");
    boot();
    CHECK(get_param(P_DIAG_SETTINGS, 0, &v) == GSX_OK && v == GM_SETTINGS_LOADED, "diag.settings: loaded");
    /* a stored value this firmware refuses: diag.settings 2, blink code 2 */
    {
        cs_entry_t e = { P_LOG_LEVEL, 0, 9 };
        config_store_write(0, 0, 0, &e, 1);
    }
    gm_init(0x00010000u, 0);
    out_n = 0;
    run_ms(5);
    CHECK(get_param(P_DIAG_SETTINGS, 0, &v) == GSX_OK && v == GM_SETTINGS_REFUSED, "diag.settings: refused");
    int lit = 0;
    for (uint32_t i = 0; i < 2200; i++) {   /* one round: 2 x (200 on, 300 off), 1.2 s dark */
        run_ms(1);
        lit += level[13] == 0;
    }
    CHECK(lit >= 350 && lit <= 450, "blink code 2: the Edit LED lit %d ms of 2.2 s", lit);
    run_ms(10000);
    CHECK(level[13] == 1, "the code stops after 10 s");
    gm_init(0x00010000u, 1);
    run_ms(5);
    CHECK(get_param(P_DIAG_SETTINGS, 0, &v) == GSX_OK && v == GM_SETTINGS_SAFE, "diag.settings: safe mode");

    /* the factory-reset combo: Oct - + Oct + held at power-up for 5 s */
    boot();
    CHECK(set_param(P_CHANNEL, 0, 4) == GSX_OK && request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK, "store");
    level[3] = level[4] = 0;
    gm_init(0x00010000u, 0);
    out_n = 0;
    run_ms(3000);
    CHECK(gm_cfg.channel == 4 && fake_cs_count(1) > 0 && gm_cfg.octave == 0, "3 s: nothing yet, octave untouched");
    level[3] = level[4] = 1;            /* let go early */
    run_ms(3000);
    CHECK(gm_cfg.channel == 4 && fake_cs_count(1) > 0 && gm_cfg.octave == 0, "let go early: nothing wiped");
    level[3] = level[4] = 0;
    gm_init(0x00010000u, 0);
    run_ms(GM_RESET_HOLD_MS + 300);
    CHECK(gm_cfg.channel == 1 && fake_cs_count(1) == -1 && fake_cs_count(0) == -1, "held 5 s: settings wiped");
    CHECK(get_param(P_DIAG_SETTINGS, 0, &v) == GSX_OK && v == GM_SETTINGS_WIPED, "diag.settings: wiped by the combo");
    level[3] = level[4] = 1;
    run_ms(100);
    CHECK(gm_cfg.octave == 0, "the release does nothing either");
    /* pressed later than at power-up: ordinary buttons */
    run_ms(2000);
    CHECK(set_param(P_CHANNEL, 0, 6) == GSX_OK && request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_OK, "store");
    level[3] = 0;
    run_ms(50);
    level[4] = 0;
    run_ms(GM_RESET_HOLD_MS + 300);
    CHECK(fake_cs_count(1) > 0 && gm_cfg.channel == 6, "later on, Oct - + Oct + wipe nothing");
    level[3] = level[4] = 1;
    run_ms(100);
    unlocked_request(GSX_CMD_FACTORY_RESET, 0, 0, 0);
    boot();
}

/* ---- tests ------------------------------------------------------------ */

/* ---- layout and raw readings (sysex.md 4.7, 4.8) ------------------------ */

/* The next RAW message at or after *at in the output; its payload offset,
 * or -1. *at moves past it. */
static int next_raw(int *at, int *len)
{
    for (int i = *at; i + GSX_OVERHEAD < out_n; i++) {
        if (out[i] == 0xF0 && out[i + 1] == 0x7D && out[i + 1 + GSX_OFF_CMD] == GSX_MSG_RAW) {
            *len = (int)gsx_get_u(out + i + 1 + GSX_OFF_LEN, 2);
            *at = i + 1;
            return i + 1 + GSX_OFF_PAYLOAD;
        }
    }
    return -1;
}

static int count_raw(void)
{
    int at = 0, len, n = 0;
    while (next_raw(&at, &len) >= 0)
        n++;
    return n;
}

static int monitor(uint8_t kinds, int *pl)
{
    return request(GSX_CMD_MONITOR, &kinds, 1, pl);
}

static void test_layout(void)
{
    /* GET_LAYOUT: windows of the blob, as GET_SCHEMA */
    uint8_t got[GM_LAYOUT_BYTES + 64];
    uint32_t off = 0, total = 0;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t p[6];
        put_u(p, off, 4);
        put_u(p + 4, 64, 2);
        int pl, len;
        CHECK(request(GSX_CMD_GET_LAYOUT, p, 6, &pl) == GSX_OK, "GET_LAYOUT at %u", off);
        total = gsx_get_u(out + pl, 4);
        CHECK(gsx_get_u(out + pl + 4, 4) == off, "GET_LAYOUT offset echo");
        find_sysex(GSX_CMD_GET_LAYOUT | GSX_REPLY, &len);
        int n = gsx_unpack(out + pl + 8, (uint32_t)(len - 9), got + off);
        if (n <= 0)
            break;
        off += (uint32_t)n;
    }
    CHECK(total == GM_LAYOUT_BYTES && off == total && memcmp(got, gm_layout, total) == 0,
          "GET_LAYOUT reads the whole layout (%u of %u bytes)", off, total);
    CHECK(got[0] == 1 && got[3] == 9 && got[4] == 0, "layout v1 with 9 items (7 buttons, the knob, the keys)");
    /* the last item: the keys, made by the builder under the controls */
    const uint8_t *k = got + total - 10;
    CHECK(k[0] == 9 && k[1] == GSX_KIND_MATRIX && k[3] == 8 && k[8] == 32 && k[9] == 48,
          "keys item: 32 keys from note 48");
    {
        uint8_t p[6];
        put_u(p, total, 4);
        put_u(p + 4, 64, 2);
        int pl, len;
        CHECK(request(GSX_CMD_GET_LAYOUT, p, 6, &pl) == GSX_OK, "GET_LAYOUT at total");
        find_sysex(GSX_CMD_GET_LAYOUT | GSX_REPLY, &len);
        CHECK(len == 9, "GET_LAYOUT at total: no data");
        put_u(p + 4, 0, 2);
        CHECK(request(GSX_CMD_GET_LAYOUT, p, 6, 0) == GSX_BAD_LEN, "GET_LAYOUT max 0");
        CHECK(request(GSX_CMD_GET_LAYOUT, p, 5, 0) == GSX_BAD_LEN, "GET_LAYOUT short");
    }
}

static void test_monitor(void)
{
    int pl;
    CHECK(monitor(0x7F, &pl) == GSX_OK, "MONITOR");
    CHECK(out[pl] == (GSX_MON_BUTTONS | GSX_MON_MATRIX | GSX_MON_ANALOG),
          "MONITOR: buttons, key matrix and analog (no encoders or pads here): 0x%x", out[pl]);
    CHECK(gsx_get_u(out + pl + 1, 2) == GM_MONITOR_INTERVAL_MS && gsx_get_u(out + pl + 3, 2) == GM_MONITOR_TIMEOUT_MS,
          "MONITOR interval and timeout");
    {
        uint8_t two[2] = { 1, 0 };
        CHECK(request(GSX_CMD_MONITOR, two, 2, 0) == GSX_BAD_LEN, "MONITOR with 2 bytes");
    }
    adc[0] = 1234;
    level[1 + 2] = 0;               /* Oct - held (button 2), not yet debounced */
    contact[3][1] = 1;              /* key 3's first contact: row 3, column 1 */
    ticks(2);
    out_n = 0;
    run_ms(4 * GM_MONITOR_INTERVAL_MS);
    int at = 0, len, seen = 0;
    for (int p; (p = next_raw(&at, &len)) >= 0;) {
        if (out[p] == EV_BUTTON) {
            CHECK(len == 1 + 2 + 1 + 1 && gsx_get_u(out + p + 1, 2) == 0 && out[p + 3] == 7,
                  "RAW buttons: first 0, 7 buttons, one byte");
            CHECK(out[p + 4] == 1u << 2, "RAW buttons: Oct - pressed (0x%x)", out[p + 4]);
            seen |= 1;
        } else if (out[p] == EV_KEY) {
            CHECK(len == 1 + 3 + 10 && out[p + 1] == 0 && out[p + 2] == 8 && out[p + 3] == 8,
                  "RAW key matrix: rows 0..7, 8 columns, 10 bytes");
            int b = 3 * 8 + 1, ones = 0;
            for (int i = 0; i < 64; i++)
                ones += (out[p + 4 + i / 7] >> (i % 7)) & 1;
            CHECK(((out[p + 4 + b / 7] >> (b % 7)) & 1) && ones == 1, "RAW key matrix: row 3 column 1 closed");
            seen |= 2;
        } else if (out[p] == EV_ANALOG) {
            CHECK(len == 1 + 2 + 1 + 2 && out[p + 3] == 1 && gsx_get_u(out + p + 4, 2) == 1234,
                  "RAW analog: the volume knob's reading, 1234 (got %u)", gsx_get_u(out + p + 4, 2));
            seen |= 4;
        }
    }
    CHECK(seen == 7, "RAW: every kind in turn (seen 0x%x)", seen);
    CHECK(count_raw() >= 3 && count_raw() <= 5, "RAW: one per %u ms (%d in %u ms)", GM_MONITOR_INTERVAL_MS,
          count_raw(), 4 * GM_MONITOR_INTERVAL_MS);
    level[1 + 2] = 1;
    contact[3][1] = 0;
    run_ms(20);

    /* only the kinds asked for */
    CHECK(monitor(GSX_MON_ANALOG | GSX_MON_ENCODERS, &pl) == GSX_OK && out[pl] == GSX_MON_ANALOG,
          "MONITOR analog + encoders: analog only");
    out_n = 0;
    run_ms(3 * GM_MONITOR_INTERVAL_MS);
    at = 0;
    int only = 1;
    for (int p; (p = next_raw(&at, &len)) >= 0;)
        only &= out[p] == EV_ANALOG;
    CHECK(only && count_raw() >= 2, "RAW: analog only");

    /* it stops by itself without a renewal */
    run_ms(GM_MONITOR_TIMEOUT_MS);
    out_n = 0;
    run_ms(5 * GM_MONITOR_INTERVAL_MS);
    CHECK(count_raw() == 0, "RAW stops %u ms after the last MONITOR", GM_MONITOR_TIMEOUT_MS);

    /* renewed in time it goes on; MONITOR 0 stops it */
    monitor(GSX_MON_BUTTONS, 0);
    for (int i = 0; i < 4; i++) {
        run_ms(2000);
        monitor(GSX_MON_BUTTONS, 0);
    }
    out_n = 0;
    run_ms(5 * GM_MONITOR_INTERVAL_MS);
    CHECK(count_raw() >= 4, "RAW goes on while it is renewed");
    CHECK(monitor(0, &pl) == GSX_OK && out[pl] == 0, "MONITOR 0");
    out_n = 0;
    run_ms(5 * GM_MONITOR_INTERVAL_MS);
    CHECK(count_raw() == 0, "MONITOR 0 stops it");
    adc[0] = 0;
}

int main(int argc, char **argv)
{
    slow_cols = getenv("GM_TEST_SLOW_COLS") != NULL;
    slow_rows = getenv("GM_TEST_SLOW_ROWS") != NULL;
    printf("matrix scan: %s column path, %s rows%s\n", slow_cols ? "per-pin" : "fast (port runs)",
           slow_rows ? "pin writes" : "register stores",
           !slow_cols && !slow_rows ? " (matrix_scan_fast)" : "");
    for (int i = 1; i <= 7; i++)
        level[i] = 1;                /* buttons released */
    adc[0] = 2048;

    gm_init(0x00010000u, 0);
    run_ms(2000);
    CHECK((find_sysex(GSX_MSG_BOOTED, 0) >= 0) == GM_HOST_FB,
          "BOOTED at boot exactly when the device has host feedback (sysex.md 4.6.3)");
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
    {
        /* every window (the walk stops at its end) is that part of the whole */
        static const uint32_t maxes[] = { 1, 2, 13, 63, 64 };
        int bad = 0;
        for (uint32_t m = 0; m < sizeof maxes / sizeof maxes[0]; m++)
            for (uint32_t off = 0; off <= ssize + 1u; off++) {
                uint8_t data[64];
                uint32_t want = off >= ssize ? 0 : ssize - off < maxes[m] ? ssize - off : maxes[m];
                uint32_t n = params_schema_read(off, data, maxes[m]);
                if (n != want || (n && memcmp(data, schema + off, n) != 0))
                    bad++;
            }
        CHECK(bad == 0 && params_schema_size() == ssize, "every schema window matches the whole (%d differ)", bad);
        /* a download in order (each read resumes where the last stopped),
         * then backwards (each falls back to the walk from byte 0) */
        bad = 0;
        for (uint32_t m = 0; m < sizeof maxes / sizeof maxes[0]; m++) {
            for (uint32_t off = 0; off < ssize; off += maxes[m]) {
                uint8_t data[64];
                uint32_t want = ssize - off < maxes[m] ? ssize - off : maxes[m];
                if (params_schema_read(off, data, maxes[m]) != want || memcmp(data, schema + off, want))
                    bad++;
            }
            for (uint32_t off = ssize; off-- > 0;) {
                uint8_t data[64];
                uint32_t want = ssize - off < maxes[m] ? ssize - off : maxes[m];
                if (params_schema_read(off, data, maxes[m]) != want || memcmp(data, schema + off, want))
                    bad++;
            }
        }
        CHECK(bad == 0, "schema windows in order and backwards match the whole (%d differ)", bad);
    }

    /* keys and velocity */
    int d1, d2;
    col_reads = 0;
    row_writes = 0;
    run_ms(5);
    CHECK(slow_cols ? col_reads > 0 : col_reads == 0, "scan used the %s column path (%d pin reads)",
          slow_cols ? "per-pin" : "fast", col_reads);
    CHECK(slow_rows ? row_writes > 0 : row_writes == 0, "scan drove the rows by %s (%d pin writes)",
          slow_rows ? "pin writes" : "register stores", row_writes);
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
    /* held half-way for 65536 + 3 ticks (8 s): the 16-bit start time
     * doesn't wrap into a fast key (kt0_age) */
    press_key(7, (65536 + 3) * (int)gm_device.scan_us);
    CHECK(find_msg(0x90, &d1, &d2) && d1 == 55 && d2 == 1, "8 s half press: note 55 vel 1 (got %d %d)", d1, d2);
    release_key(7);
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

    /* sustain as CC toggle 80 (Edit is the mode button: test_modes) */
    set_param(P_BTN_ACTION, 0, ACT_CC_TOGGLE);
    set_param(P_BTN_NUMBER, 0, 80);
    out_n = 0;
    button(0, 1); button(0, 0);
    CHECK(find_msg(0xB0, &d1, &d2) && d1 == 80 && d2 == 127, "toggle on");
    out_n = 0;
    button(0, 1); button(0, 0);
    CHECK(find_msg(0xB0, &d1, &d2) && d1 == 80 && d2 == 0, "toggle off");

    /* host feedback (F1) over USB-MIDI events: the toggle follows the
     * host's echo of CC 80, the sustain LED the host's CC 64 for 2 s */
    {
        uint8_t ev[4] = { 0x0B, 0xB0, 80, 127 };
        gm_midi_rx_usb(ev);
        run_ms(20);
        CHECK(level[12] == 0, "toggle's LED lit by the host's CC 80 = 127");
        out_n = 0;
        button(0, 1); button(0, 0);
        CHECK(find_msg(0xB0, &d1, &d2) && d1 == 80 && d2 == 0, "so the next press sends 0");
        set_param(P_BTN_ACTION, 0, ACT_CC_MOMENTARY);
        set_param(P_BTN_NUMBER, 0, 64);
        uint8_t ev2[4] = { 0x0B, 0xB0, 64, 127 };
        gm_midi_rx_usb(ev2);
        run_ms(20);
        CHECK(level[12] == 0, "sustain LED lit by the host");
        run_ms(2100);
        CHECK(level[12] == 1, "local again 2 s later (host_then_local)");
        uint8_t ev3[4] = { 0x09, 0x90, 60, 100 };    /* a note from the host: nothing to do */
        out_n = 0;
        gm_midi_rx_usb(ev3);
        run_ms(20);
        CHECK(out_n == 0, "host note: no output");
    }

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

    /* the knob as a pitch wheel (analog.mode): its reading at boot (2048) is the centre */
    CHECK(set_param(P_ANALOG_MODE, 0, AN_MODE_BEND) == GSX_OK, "SET analog.mode = pitch bend");
    out_n = 0;
    adc[0] = 4095;
    run_ms(50);
    CHECK(last_bend() == 16383 && count_msgs(0xB0) == 0, "wheel up: bend 16383, no CC (got %d)", last_bend());
    out_n = 0;
    adc[0] = 2048;
    run_ms(50);
    CHECK(last_bend() == 8192, "back to the centre: bend 8192 exactly (got %d)", last_bend());
    out_n = 0;
    adc[0] = 2048 + 12;              /* inside the dead zone (16 raw counts) */
    run_ms(50);
    CHECK(count_msgs(0xE0) == 0, "dead zone: no bend");
    out_n = 0;
    adc[0] = 0;
    run_ms(50);
    CHECK(last_bend() == 0, "wheel down: bend 0 (got %d)", last_bend());
    adc[0] = 1024;
    run_ms(50);
    out_n = 0;
    adc[0] = 1022;                   /* jitter, back toward the last value sent */
    run_ms(50);
    CHECK(count_msgs(0xE0) == 0, "bend hysteresis");
    {
        int b;
        adc[0] = 1024;
        run_ms(50);
        out_n = 0;
        adc[0] = 1100;
        run_ms(50);
        b = last_bend();
        CHECK(b > 0 && b < 8192, "halfway down: 0 < bend < 8192 (got %d)", b);
    }
    adc[0] = 2048;
    run_ms(50);
    CHECK(set_param(P_ANALOG_MODE, 0, AN_MODE_CC) == GSX_OK, "analog.mode back to CC");
    out_n = 0;
    adc[0] = 4095;
    run_ms(50);
    CHECK(find_msg(0xB0, &d1, 0) && d1 == 7 && count_msgs(0xE0) == 0, "CC again, no bend");

    /* the knob as an aftertouch strip: channel pressure 0..127, no CC */
    CHECK(set_param(P_ANALOG_MODE, 0, AN_MODE_PRESSURE) == GSX_OK, "SET analog.mode = channel pressure");
    out_n = 0;
    adc[0] = 32;                     /* adc_min: released */
    run_ms(50);
    {
        int last, n = pressure(&last);
        CHECK(n > 0 && last == 0 && count_msgs(0xB0) == 0, "released: pressure 0, no CC (%d, %d)", n, last);
        out_n = 0;
        adc[0] = 4064;               /* adc_max: pressed hard */
        run_ms(50);
        n = pressure(&last);
        CHECK(last == 127 && n > 1, "pressed: pressure rises to 127 (got %d, %d messages)", last, n);
        out_n = 0;
        adc[0] = 4050;               /* jitter near the top */
        run_ms(50);
        CHECK(pressure(&last) == 0, "pressure hysteresis");
        out_n = 0;
        adc[0] = 32;
        run_ms(50);
        n = pressure(&last);
        CHECK(n > 0 && last == 0, "let go: back to 0 (got %d)", last);
    }
    CHECK(set_param(P_ANALOG_MODE, 0, 3) == GSX_BAD_VALUE, "analog.mode 3 -> BAD_VALUE");
    CHECK(set_param(P_ANALOG_MODE, 0, AN_MODE_CC) == GSX_OK, "analog.mode back to CC");

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
        /* The longest request fits the receive buffer (app_proto.c
         * RX_PAYLOAD): 16 values past the 7 buttons is BAD_PARAM, not
         * BAD_LEN; one byte more doesn't fit. */
        n = put_u(p, P_BTN_NUMBER, 2);
        n += put_u(p + n, 0, 2);
        n += put_u(p + n, 16, 2);
        for (int i = 0; i < 16; i++)
            n += put_u(p + n, 1, 5);
        CHECK(n == 86 && request(GSX_CMD_SET_PARAMS, p, n, 0) == GSX_BAD_PARAM, "SET_PARAMS with 16 values fits");
        uint8_t q[87] = { 0 };
        memcpy(q, p, 86);
        CHECK(request(GSX_CMD_SET_PARAMS, q, 87, 0) == GSX_BAD_LEN, "SET_PARAMS one byte longer -> BAD_LEN");
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

    /* the rest of the old config test, now via presets */
    CHECK(gm_cfg.preset_dirty == 1, "host edits made preset 1 dirty");
    CHECK(unlocked_request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_OK, "FACTORY_RESET");
    CHECK(gm_cfg.channel == 1 && gm_cfg.vel_fixed == 100 && gm_cfg.preset_dirty == 0,
          "defaults after reset");

    test_presets();
    test_modes();
    test_velocity_sweep();
    test_layout();
    test_monitor();
    test_safety();

    /* HELLO, FW_* in the app, REBOOT */
    {
        int pl;
        CHECK(request(GSX_CMD_HELLO, 0, 0, &pl) == GSX_OK, "HELLO");
        CHECK(out[pl] == 0 && gsx_get_u(out + pl + 1, 2) == 1, "role app, board 1");
        uint32_t caps = gsx_get_u(out + pl + 1 + 2 + 5 + 1 + 2, 2);
        CHECK(caps == (GSX_CAP_CONFIG | GSX_CAP_LOG | GSX_CAP_EVENT | GSX_CAP_PRESETS | GSX_CAP_FEEDBACK |
                       GSX_CAP_LAYOUT | GSX_CAP_MONITOR | (GM_UNLOCK ? GSX_CAP_UNLOCK : 0)), "caps 0x%x", caps);
        int node_at = pl + 1 + 2 + 5 + 1 + 2 + 2 + 14;
        size_t node_len = strlen(gm_device.node_name);
        CHECK(out[node_at] == node_len && memcmp(out + node_at + 1, gm_device.node_name, node_len) == 0,
              "HELLO ends with the node name '%s'", gm_device.node_name);
        CHECK(request(GSX_CMD_FW_END, 0, 0, 0) == GSX_BAD_STATE, "FW_* -> BAD_STATE on NUC");
        {
            uint8_t chunk[4 + 293] = { 0 };     /* a whole 256-byte FW_DATA chunk, packed */
            CHECK(request(GSX_CMD_FW_DATA, chunk, sizeof(chunk), 0) == GSX_BAD_STATE,
                  "FW_DATA longer than the receive buffer -> BAD_STATE all the same");
        }
        uint8_t m = 1;
#if GM_UNLOCK
        CHECK(request(GSX_CMD_REBOOT, &m, 1, 0) == GSX_LOCKED, "REBOOT(1) locked");
#endif
        CHECK(unlocked_request(GSX_CMD_REBOOT, &m, 1, 0) == GSX_OK, "REBOOT(1) after the unlock");
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

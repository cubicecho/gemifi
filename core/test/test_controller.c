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
#include "fake_config_store.h"
#include "gsx.h"

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
          gm_cfg.btn_number[0] == 36 && gm_cfg.btn_number[1] == 38, "slot 2 YAML values");
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
    CHECK(fake_cs_get(0, P_VEL_FAST, 0, &v) && v == 30 && fake_cs_get(0, P_PRESET_ACTIVE, 0, &v) &&
          v == 1 && !fake_cs_get(0, P_CHANNEL, 0, 0) && !fake_cs_get(0, P_PRESET_DIRTY, 0, 0),
          "slot 0 = globals (incl. preset.active, not dirty)");
    CHECK(fake_cs_revision(1) == GM_PRESETS_REVISION, "record revision");

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

    /* boot: globals, then defaults -> YAML -> stored for the active slot */
    gm_cfg.vel_fast = 99;
    boot();
    CHECK(gm_cfg.preset_active == 2 && gm_cfg.vel_fast == 30 && gm_cfg.channel == 10 &&
          gm_cfg.vel_curve == CURVE_HARD && gm_cfg.btn_number[0] == 40 && gm_cfg.btn_number[1] == 38 &&
          gm_cfg.btn_number[6] == 1 && gm_cfg.preset_program == 5 && gm_cfg.preset_dirty == 0,
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
    set_param(P_BTN_ACTION, 1, ACT_PRESET_NEXT);     /* edit button */
    out_n = 0;
    int w0 = fake_cs_writes;
    button(1, 1);
    CHECK(gm_cfg.preset_active == 3 && gm_cfg.btn_action[1] == ACT_CC_MOMENTARY && gm_cfg.analog_cc[0] == 11,
          "preset_next -> slot 3 (DAW)");
    CHECK(find_sysex(GSX_MSG_PARAM_CHANGED, 0) >= 0 && preset_changed_msg(&slot, &dirty) && slot == 3,
          "PARAM_CHANGED + PRESET_CHANGED from the device");
    out_n = 0;
    button(1, 0);
    CHECK(count_msgs(0xB0) == 0, "held preset button's release: no CC 81");
    run_ms(5000);
    CHECK(fake_cs_writes == w0, "preset.active not written yet");
    run_ms(5100);
    CHECK(fake_cs_writes == w0 + 1 && fake_cs_get(0, P_PRESET_ACTIVE, 0, &v) && v == 3 &&
          fake_cs_get(0, P_VEL_FAST, 0, &v) && v == 30, "preset.active stored after 10 s (globals kept)");
    run_ms(11000);
    CHECK(fake_cs_writes == w0 + 1, "written once");
    set_param(P_BTN_ACTION, 2, ACT_PRESET_PREV);
    set_param(P_BTN_ACTION, 3, ACT_PRESET_SELECT);
    set_param(P_BTN_NUMBER, 3, 1);
    button(2, 1); button(2, 0);
    CHECK(gm_cfg.preset_active == 2, "preset_prev -> 2");
    set_param(P_PRESET_ACTIVE, 0, 1);
    set_param(P_BTN_ACTION, 2, ACT_PRESET_PREV);
    button(2, 1); button(2, 0);
    CHECK(gm_cfg.preset_active == 8, "preset_prev wraps 1 -> 8");
    set_param(P_BTN_ACTION, 3, ACT_PRESET_SELECT);
    set_param(P_BTN_NUMBER, 3, 2);
    button(3, 1); button(3, 0);
    CHECK(gm_cfg.preset_active == 2, "preset_select 2");
    set_param(P_PRESET_ACTIVE, 0, 8);
    set_param(P_BTN_ACTION, 0, ACT_PRESET_SELECT);
    set_param(P_BTN_NUMBER, 0, 9);
    button(0, 1); button(0, 0);
    CHECK(gm_cfg.preset_active == 8, "preset_select 9: ignored");
    set_param(P_BTN_ACTION, 0, ACT_PRESET_NEXT);
    button(0, 1); button(0, 0);
    CHECK(gm_cfg.preset_active == 1, "preset_next wraps 8 -> 1");

    /* PRESET_INFO */
    pinfo_t in;
    CHECK(preset_info(0, &in) == GSX_OK && in.slot == 1 && in.count == 8 && in.active == 1 &&
          in.flags == (PRESET_INFO_STORED | PRESET_INFO_YAML) && !strcmp(in.name, "Default") &&
          in.program == -1, "PRESET_INFO 0 = active (flags %d, %s)", in.flags, in.name);
    CHECK(preset_info(2, &in) == GSX_OK && in.program == 5 && !strcmp(in.name, "Drums") &&
          in.flags == (PRESET_INFO_STORED | PRESET_INFO_YAML), "PRESET_INFO 2");
    CHECK(preset_info(4, &in) == GSX_OK && in.flags == 0 && !strcmp(in.name, "Preset 4") &&
          in.stored_revision == 0, "PRESET_INFO 4: empty slot (flags %d)", in.flags);
    CHECK(preset_info(9, &in) == GSX_BAD_PARAM, "PRESET_INFO 9 -> BAD_PARAM");
    set_param(P_CHANNEL, 0, 4);
    CHECK(preset_info(1, &in) == GSX_OK && (in.flags & PRESET_INFO_DIRTY), "dirty flag");

    /* PRESET_NAME */
    CHECK(preset_name(4, "Lead") == GSX_OK && preset_info(4, &in) == GSX_OK && !strcmp(in.name, "Lead") &&
          in.flags == PRESET_INFO_STORED && fake_cs_count(4) == 0, "rename slot 4");
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

    /* PRESET_SAVE (save as) from slot 3 with edits */
    set_param(P_PRESET_ACTIVE, 0, 3);
    set_param(P_ANALOG_CC, 0, 12);
    out_n = 0;
    CHECK(preset_cmd(GSX_CMD_PRESET_SAVE, 5, 0) == GSX_OK, "PRESET_SAVE 5");
    CHECK(gm_cfg.preset_active == 5 && gm_cfg.preset_dirty == 0 && preset_changed_msg(&slot, &dirty) &&
          slot == 5 && dirty == 0, "now on slot 5, clean");
    CHECK(fake_cs_get(5, P_ANALOG_CC, 0, &v) && v == 12 && fake_cs_get(5, P_BTN_ACTION, 1, &v) &&
          v == ACT_CC_MOMENTARY && fake_cs_count(3) == -1 && fake_cs_get(0, P_PRESET_ACTIVE, 0, &v) && v == 5,
          "slot 5 = DAW + edit as a diff vs defaults; slot 3 untouched; active stored");
    CHECK(preset_cmd(GSX_CMD_PRESET_SAVE, 0, 0) == GSX_BAD_PARAM &&
          preset_cmd(GSX_CMD_PRESET_SAVE, 9, 0) == GSX_BAD_PARAM, "PRESET_SAVE bad slot");

    /* PRESET_RESET */
    out_n = 0;
    CHECK(preset_cmd(GSX_CMD_PRESET_RESET, 5, 0) == GSX_OK && fake_cs_count(5) == -1, "PRESET_RESET 5");
    CHECK(gm_cfg.analog_cc[0] == 7 && gm_cfg.btn_action[1] == ACT_CC_TOGGLE, "active slot 5 reloaded");
    CHECK(preset_cmd(GSX_CMD_PRESET_RESET, 9, 0) == GSX_BAD_PARAM, "PRESET_RESET 9");
    CHECK(preset_cmd(GSX_CMD_PRESET_RESET, 0, 0) == GSX_OK && fake_cs_count(1) == -1 &&
          fake_cs_count(2) == -1 && fake_cs_count(0) >= 0, "PRESET_RESET 0: all presets, not the globals");

    /* FACTORY_RESET: store erased, slot 1 */
    CHECK(request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_OK && gm_cfg.preset_active == 1 &&
          gm_cfg.vel_fast == GM_VEL_FAST_DEFAULT && fake_cs_count(0) == -1, "FACTORY_RESET");

    /* a failing store: COMMIT reports it, dirty stays */
    set_param(P_CHANNEL, 0, 2);
    fake_cs_fail = 1;
    CHECK(request(GSX_CMD_COMMIT, 0, 0, 0) == GSX_FLASH_ERR && gm_cfg.preset_dirty == 1, "COMMIT flash error");
    fake_cs_fail = 0;
    request(GSX_CMD_FACTORY_RESET, 0, 0, 0);
}

/* ---- tests ------------------------------------------------------------ */

int main(int argc, char **argv)
{
    slow_cols = getenv("GM_TEST_SLOW_COLS") != NULL;
    printf("matrix scan: %s column path\n", slow_cols ? "per-pin" : "fast (port runs)");
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

    /* host feedback (F1) over USB-MIDI events: the edit toggle follows the
     * host's echo of CC 80, the sustain LED the host's CC 64 for 2 s */
    {
        uint8_t ev[4] = { 0x0B, 0xB0, 80, 127 };
        gm_midi_rx_usb(ev);
        run_ms(20);
        CHECK(level[13] == 0, "edit LED lit by the host's CC 80 = 127");
        out_n = 0;
        button(1, 1); button(1, 0);
        CHECK(find_msg(0xB0, &d1, &d2) && d1 == 80 && d2 == 0, "so the next press sends 0");
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

    /* the rest of the old config test, now via presets */
    CHECK(gm_cfg.preset_dirty == 1, "host edits made preset 1 dirty");
    CHECK(request(GSX_CMD_FACTORY_RESET, 0, 0, 0) == GSX_OK, "FACTORY_RESET");
    CHECK(gm_cfg.channel == 1 && gm_cfg.vel_fixed == 100 && gm_cfg.preset_dirty == 0,
          "defaults after reset");

    test_presets();

    /* HELLO, FW_* in the app, REBOOT */
    {
        int pl;
        CHECK(request(GSX_CMD_HELLO, 0, 0, &pl) == GSX_OK, "HELLO");
        CHECK(out[pl] == 0 && gsx_get_u(out + pl + 1, 2) == 1, "role app, board 1");
        uint32_t caps = gsx_get_u(out + pl + 1 + 2 + 5 + 1 + 2, 2);
        CHECK(caps == (GSX_CAP_CONFIG | GSX_CAP_LOG | GSX_CAP_EVENT | GSX_CAP_PRESETS | GSX_CAP_FEEDBACK), "caps 0x%x", caps);
        int node_at = pl + 1 + 2 + 5 + 1 + 2 + 2 + 14;
        size_t node_len = strlen(gm_device.node_name);
        CHECK(out[node_at] == node_len && memcmp(out + node_at + 1, gm_device.node_name, node_len) == 0,
              "HELLO ends with the node name '%s'", gm_device.node_name);
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

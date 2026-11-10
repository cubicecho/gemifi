/*
 * app_proto.c: app-side request handling (protocol/sysex.md 4, 4.3, 6).
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_proto.h"
#include "gm_hal.h"
#include "gm_device.h"
#include "gsx.h"
#include "params.h"
#include "gm_presets.h"
#include "gm_settings.h"
#include "gm_feedback.h"
#include "gm_vpin.h"
#include "gm_display.h"
#include "gm_safety.h"
#include "gm_device_config.h"

#ifndef GM_N_DISPLAYS
#define GM_N_DISPLAYS 0                 /* a config generated before displays */
#endif
#ifndef GM_UNLOCK
#define GM_UNLOCK 0                     /* physical unlock (gm_safety.h) */
#endif
#if GM_N_DISPLAYS > 0
extern const gm_display_t gm_displays[GM_N_DISPLAYS];   /* generated config */
#endif
#ifndef GM_LAYOUT_BYTES
#define GM_LAYOUT_BYTES 0               /* no layout: in the YAML: no GET_LAYOUT */
#endif

#define REBOOT_DELAY_MS 50u
#define BOOTED_DELAY_MS 1500u        /* BOOTED once USB is normally up (sysex.md 4.6.3) */
#define SCHEMA_WINDOW   64u
#define PARAMS_RUN      16u
/* A whole frame with an n-byte payload: F0, body, F7. */
#define FRAME(n)        (GSX_OVERHEAD + 2u + (n))

uint8_t app_log_limit = LOG_DEBUG + 1;

static uint32_t fw_version;
static uint8_t has_config;
/* The longest request or notification: SET_PARAMS with PARAMS_RUN values
 * (id, index, n, then 5 bytes each: 86 bytes of payload). Notifications
 * need at least 72 (sysex.md 4.6.2: FB_LEDS with 64 LEDs, FB_TEXT with 64
 * characters), PRESET_NAME 3 + 16. Anything longer is answered BAD_LEN
 * (gsx_parse), a notification dropped. */
#define RX_PAYLOAD      (6u + PARAMS_RUN * 5u)
_Static_assert(RX_PAYLOAD >= 72u, "notifications of 72 bytes (sysex.md 4.6.2)");
_Static_assert(RX_PAYLOAD >= 3u + PRESET_NAME_MAX, "PRESET_NAME with the longest name");
_Static_assert(RX_PAYLOAD >= 17u, "FW_BEGIN");
#if HAL_APP_FW_UPLOAD
static const hal_fw_ops_t *fw;
static uint8_t fw_active;
/* FW_DATA: offset + up to 256 B packed (293 bytes), unpacked in place. */
static uint8_t rx_buf[GSX_OVERHEAD + 4 + 300];
_Static_assert(4u + 300u >= RX_PAYLOAD, "rx_buf");
#else
/* The bootloader takes uploads (gm_hal.h HAL_APP_FW_UPLOAD): no FW_DATA. */
#define fw ((const hal_fw_ops_t *)0)
static uint8_t rx_buf[GSX_OVERHEAD + RX_PAYLOAD];
#endif
static gsx_rx_t rx;
/* The largest reply: GET_PARAMS (6 + 16 values), or GET_COMPONENT (kind,
 * index, count, three lstrings, up to 6 bytes of extras: an analog's
 * takeover, touch, adc_min and adc_max). */
#define REPLY_PARAMS    (6u + PARAMS_RUN * 5u + 8u)
#define REPLY_COMPONENT (1u + 2u + 2u + 3u * (1u + GSX_LSTRING_MAX) + 6u)
#define REPLY_MAX0      (REPLY_PARAMS > REPLY_COMPONENT ? REPLY_PARAMS : REPLY_COMPONENT)
/* UNLOCK: state, window, hold, n, the names (the builder's GM_UNLOCK_REPLY) */
#if GM_UNLOCK && GM_UNLOCK_REPLY > REPLY_MAX0
#define REPLY_MAX       GM_UNLOCK_REPLY
#else
#define REPLY_MAX       REPLY_MAX0
#endif
static uint8_t tx_buf[GSX_OVERHEAD + 1 + REPLY_MAX];
static uint16_t tx_len;
static uint32_t last_req;            /* 0x10000 | seq << 8 | cmd, 0 = none */
static uint8_t uid[12];
static uint8_t unsol_seq;
static uint8_t reboot_mode;          /* 0 none, 1 app, 2 bootloader */
static uint32_t reboot_at;
#if GM_HOST_FB
static uint8_t booted_due;           /* BOOTED not sent yet */
static uint32_t booted_at;
#endif

__attribute__((weak)) void app_on_param_set(uint16_t id, uint16_t index)
{
    (void)id;
    (void)index;
}

void app_proto_uid_hex(char out[25])
{
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < 24; i++) {
        uint32_t byte = uid[(i / 8) * 4 + 3 - ((i / 2) & 3)];
        out[i] = hex[(i & 1) ? (byte & 0xF) : (byte >> 4)];
    }
    out[24] = 0;
}

void app_proto_init(uint32_t version, int config)
{
    fw_version = version;
    has_config = (uint8_t)config;
#if HAL_APP_FW_UPLOAD
    fw = hal_fw_ops();
#endif
    hal_uid(uid);
    rx.buf = rx_buf;
    rx.cap = sizeof(rx_buf);
    gsx_rx_reset(&rx);
#if GM_HOST_FB
    booted_due = 1;
    booted_at = hal_millis() + BOOTED_DELAY_MS;
#endif
}

__attribute__((weak)) uint16_t hal_extra_caps(void)
{
    return 0;
}

static uint16_t caps(void)
{
    uint16_t c = GSX_CAP_LOG | GSX_CAP_EVENT | hal_extra_caps();
    if (has_config)
        c |= GSX_CAP_CONFIG | GSX_CAP_PRESETS;
#if GM_HOST_FB
    if (has_config)
        c |= GSX_CAP_FEEDBACK;
#endif
    if (fw)
        c |= GSX_CAP_FW_UPLOAD;
#if GM_LAYOUT_BYTES
    if (has_config)
        c |= GSX_CAP_LAYOUT;
#endif
#if GM_MONITOR
    if (has_config)
        c |= GSX_CAP_MONITOR;
#endif
#if GM_UNLOCK
    c |= GSX_CAP_UNLOCK;
#endif
    return c;
}

/* ---- unsolicited messages --------------------------------------------- */

void app_send_unsolicited(uint8_t cmd, const uint8_t *payload, uint32_t n)
{
    uint8_t buf[FRAME(96)];
    if (n > 96u)
        return;
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, cmd, unsol_seq++ & 0x7F);
    for (uint32_t i = 0; i < n; i++)
        gsx_put(&t, payload[i]);
    hal_midi_send(buf, gsx_tx_end(&t));
}

void app_log(uint8_t level, const char *a, const char *b)
{
    if (level >= app_log_limit)
        return;
    uint8_t buf[FRAME(1 + 4 + 64)];
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, GSX_MSG_LOG, unsol_seq++ & 0x7F);
    gsx_put(&t, level);
    gsx_put_u(&t, hal_millis(), 4);
    uint32_t n = 0;
    for (const char *s = a; s && *s && n < 64; s++, n++)
        gsx_put(&t, (uint8_t)*s);
    for (const char *s = b; s && *s && n < 64; s++, n++)
        gsx_put(&t, (uint8_t)*s);
    hal_midi_send(buf, gsx_tx_end(&t));
}

void app_log_hex(uint8_t level, const char *a, uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    char s[9];
    for (int i = 0; i < 8; i++)
        s[i] = hex[(v >> (28 - 4 * i)) & 0xF];
    s[8] = 0;
    app_log(level, a, s);
}

void app_event(uint8_t type, uint32_t index, uint32_t value)
{
    uint8_t buf[FRAME(1 + 2 + 2 + 4)];
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, GSX_MSG_EVENT, unsol_seq++ & 0x7F);
    gsx_put(&t, type);
    gsx_put_u(&t, index, 2);
    gsx_put_u(&t, value, 2);
    gsx_put_u(&t, hal_millis(), 4);
    hal_midi_send(buf, gsx_tx_end(&t));
}

void app_param_changed(uint16_t id, uint16_t index)
{
    int32_t v;
    if (params_get(id, index, &v) != GSX_OK)
        return;
    uint8_t buf[FRAME(2 + 2 + 5)];
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, GSX_MSG_PARAM_CHANGED, unsol_seq++ & 0x7F);
    gsx_put_u(&t, id, 2);
    gsx_put_u(&t, index, 2);
    gsx_put_u(&t, (uint32_t)v, 5);
    hal_midi_send(buf, gsx_tx_end(&t));
}

void app_preset_changed(uint32_t slot, uint32_t dirty)
{
    uint8_t buf[FRAME(2 + 1)];
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, GSX_MSG_PRESET_CHANGED, unsol_seq++ & 0x7F);
    gsx_put_u(&t, slot, 2);
    gsx_put(&t, (uint8_t)(dirty ? 1 : 0));
    hal_midi_send(buf, gsx_tx_end(&t));
}

#if GM_UNLOCK
void app_unlocked(uint8_t state)
{
    uint8_t buf[FRAME(1)];
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, GSX_MSG_UNLOCKED, unsol_seq++ & 0x7F);
    gsx_put(&t, state);
    hal_midi_send(buf, gsx_tx_end(&t));
}
#else
void app_unlocked(uint8_t state)
{
    (void)state;
}
#endif

/* ---- requests --------------------------------------------------------- */

static void identity_reply(void)
{
    /* Universal Identity Reply: F0 7E <dev> 06 02 <mfr> <family u14>
     * <model u14> <version 4 bytes> F7. Manufacturer = our SysEx ID's
     * first byte (0x7D, sysex.md 1), family = 'g' 'm', model = board_id. */
    uint32_t v = fw_version;
    uint16_t board = gm_device.board_id;
    const uint8_t msg[] = {
        0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x7D, 0x67, 0x6D,
        board & 0x7F, (board >> 7) & 0x7F,
        (v >> 24) & 0x7F, (v >> 16) & 0x7F, v & 0x7F, 0x00, 0xF7,
    };
    hal_midi_send(msg, sizeof(msg));
}

/* HELLO `node` (sysex.md 4.1.1): u8 len, then the node name. Left out when
 * there is none; the builder has already checked the name's characters. */
static void put_node_name(gsx_tx_t *t)
{
    const char *s = gm_device.node_name;
    uint32_t n = 0;
    if (s == 0)
        return;
    while (s[n] != 0 && n < GSX_NODE_NAME_MAX)
        n++;
    if (n == 0)
        return;
    gsx_put(t, (uint8_t)n);
    for (uint32_t i = 0; i < n; i++)
        gsx_put(t, (uint8_t)s[i]);
}

/* HELLO's reply fields after the status; BOOTED sends the same. */
static void put_hello(gsx_tx_t *t)
{
    gsx_put(t, 0);                          /* role: app */
    gsx_put_u(t, gm_device.board_id, 2);
    gsx_put_u(t, fw_version, 5);
    gsx_put(t, GSX_PROTO_VERSION);
    gsx_put_u(t, fw ? fw->max_chunk : 0, 2);
    gsx_put_u(t, caps(), 2);
    gsx_put_packed(t, uid, sizeof(uid));
    put_node_name(t);
}

static int cmd_hello(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 0)
        return GSX_BAD_LEN;
    put_hello(t);
    return GSX_OK;
}

#if GM_HOST_FB
/* BOOTED (sysex.md 4.6.3): a HELLO reply without the status, sent once so a
 * DAW script knows its FB_* states are gone. Only with caps bit 6. */
static void send_booted(void)
{
    if ((caps() & GSX_CAP_FEEDBACK) == 0)
        return;
    uint8_t buf[FRAME(1 + 2 + 5 + 1 + 2 + 2 + 14 + 1 + GSX_NODE_NAME_MAX)];
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, GSX_MSG_BOOTED, unsol_seq++ & 0x7F);
    put_hello(&t);
    hal_midi_send(buf, gsx_tx_end(&t));
}
#endif

/* GET_SCHEMA and GET_LAYOUT: a window of a blob, `total` bytes long, read
 * by read() (sysex.md 4.3, 4.7). */
static int cmd_window(const gsx_frame_t *f, gsx_tx_t *t, uint32_t total,
                      uint32_t (*read)(uint32_t off, uint8_t *dst, uint32_t max))
{
    if (f->len != 6)
        return GSX_BAD_LEN;
    uint32_t off = gsx_get_u(f->payload, 4);
    uint32_t max = gsx_get_u(f->payload + 4, 2);
    if (max == 0 || max > SCHEMA_WINDOW)
        return GSX_BAD_LEN;
    uint8_t data[SCHEMA_WINDOW];
    uint32_t n = read(off, data, max);
    gsx_put_u(t, total, 4);
    gsx_put_u(t, off, 4);
    gsx_put_packed(t, data, n);
    return GSX_OK;
}

#if GM_LAYOUT_BYTES
static uint32_t layout_read(uint32_t off, uint8_t *dst, uint32_t max)
{
    uint32_t n = 0;
    for (; n < max && off + n < GM_LAYOUT_BYTES; n++)
        dst[n] = gm_layout[off + n];
    return n;
}
#endif

static int cmd_get_param(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 4)
        return GSX_BAD_LEN;
    uint16_t id = (uint16_t)gsx_get_u(f->payload, 2);
    uint16_t index = (uint16_t)gsx_get_u(f->payload + 2, 2);
    int32_t v;
    int st = params_get(id, index, &v);
    if (st != GSX_OK)
        return st;
    gsx_put_u(t, id, 2);
    gsx_put_u(t, index, 2);
    gsx_put_u(t, (uint32_t)v, 5);
    return GSX_OK;
}

static int cmd_set_param(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 9)
        return GSX_BAD_LEN;
    uint16_t id = (uint16_t)gsx_get_u(f->payload, 2);
    uint16_t index = (uint16_t)gsx_get_u(f->payload + 2, 2);
    int32_t v = (int32_t)gsx_get_u(f->payload + 4, 5);
    if (!presets_value_ok(id, v))           /* preset.active on a free slot */
        return GSX_BAD_VALUE;
    int st = params_set(id, index, v, 1);
    if (st != GSX_OK)
        return st;
    presets_host_set(id, index);
    app_on_param_set(id, index);
    if (id == P_EVENTS && v == 1)
        gm_layers_report();             /* EVENT 8 for each mode on, every time (sysex.md 4.6) */
    gsx_put_u(t, id, 2);
    gsx_put_u(t, index, 2);
    gsx_put_u(t, (uint32_t)v, 5);
    return GSX_OK;
}

/* Shared range check for GET/SET_PARAMS. */
static const param_def_t *run_def(const uint8_t *p, uint16_t *id, uint16_t *index,
                                  uint16_t *n, int *st)
{
    *id = (uint16_t)gsx_get_u(p, 2);
    *index = (uint16_t)gsx_get_u(p + 2, 2);
    *n = (uint16_t)gsx_get_u(p + 4, 2);
    const param_def_t *d = params_find(*id);
    if (*n == 0 || *n > PARAMS_RUN) {
        *st = GSX_BAD_LEN;
        return 0;
    }
    if (!d || (uint32_t)*index + *n > d->count) {
        *st = GSX_BAD_PARAM;
        return 0;
    }
    *st = GSX_OK;
    return d;
}

static int cmd_get_params(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 6)
        return GSX_BAD_LEN;
    uint16_t id, index, n;
    int st;
    const param_def_t *d = run_def(f->payload, &id, &index, &n, &st);
    if (!d)
        return st;
    gsx_put_u(t, id, 2);
    gsx_put_u(t, index, 2);
    gsx_put_u(t, n, 2);
    for (uint32_t i = 0; i < n; i++)
        gsx_put_u(t, (uint32_t)d->values[index + i], 5);
    return GSX_OK;
}

static int cmd_set_params(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len < 6)
        return GSX_BAD_LEN;
    uint16_t id, index, n;
    int st;
    const param_def_t *d = run_def(f->payload, &id, &index, &n, &st);
    if (!d)
        return st;
    if (f->len != 6u + 5u * n)
        return GSX_BAD_LEN;
    if (d->flags & PF_READ_ONLY)
        return GSX_BAD_PARAM;
    const uint8_t *vp = f->payload + 6;
    for (uint32_t i = 0; i < n; i++) {           /* all or nothing */
        int32_t v = (int32_t)gsx_get_u(vp + 5 * i, 5);
        if (!params_value_ok(d, v) || !presets_value_ok(id, v))
            return GSX_BAD_VALUE;
    }
    params_gen++;
    for (uint32_t i = 0; i < n; i++) {
        d->values[index + i] = (int32_t)gsx_get_u(vp + 5 * i, 5);
        presets_host_set(id, (uint16_t)(index + i));
        app_on_param_set(id, (uint16_t)(index + i));
        if (id == P_EVENTS && d->values[index + i] == 1)
            gm_layers_report();
    }
    gsx_put_u(t, id, 2);
    gsx_put_u(t, index, 2);
    gsx_put_u(t, n, 2);
    return GSX_OK;
}

/* ---- presets (research/core/presets-design.md §10.4) -------------------- */

static int cmd_preset(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len < 2)
        return GSX_BAD_LEN;
    uint32_t slot = gsx_get_u(f->payload, 2);
    switch (f->cmd) {
    case GSX_CMD_PRESET_INFO: {
        if (f->len != 2)
            return GSX_BAD_LEN;
        preset_info_t in;
        int st = presets_info(slot, &in);
        if (st != GSX_OK)
            return st;
        gsx_put_u(t, in.slot, 2);
        gsx_put_u(t, GM_N_PRESETS, 2);
        gsx_put_u(t, presets_active(), 2);
        gsx_put(t, in.flags);
        gsx_put_u(t, GM_PRESETS_REVISION, 2);
        gsx_put_u(t, in.stored_revision, 2);
        gsx_put_u(t, (uint32_t)in.program, 5);
        uint32_t n = 0;
        while (in.name[n])
            n++;
        gsx_put(t, (uint8_t)n);
        for (uint32_t i = 0; i < n; i++)
            gsx_put(t, (uint8_t)in.name[i]);
        return GSX_OK;
    }
    case GSX_CMD_PRESET_SAVE:
        if (f->len != 2)
            return GSX_BAD_LEN;
        return slot ? presets_save(slot) : GSX_BAD_PARAM;
    case GSX_CMD_PRESET_RESET:
        if (f->len != 2)
            return GSX_BAD_LEN;
        return presets_reset(slot);
    case GSX_CMD_PRESET_DELETE:
        if (f->len != 2)
            return GSX_BAD_LEN;
        return presets_delete(slot);
    case GSX_CMD_PRESET_MOVE:
        if (f->len != 4)
            return GSX_BAD_LEN;
        return presets_move(slot, gsx_get_u(f->payload + 2, 2));
    default: {                              /* PRESET_NAME */
        if (f->len < 3)
            return GSX_BAD_LEN;
        uint32_t n = f->payload[2];
        if (f->len != 3u + n)
            return GSX_BAD_LEN;
        if (slot < 1u || slot > GM_N_PRESETS)
            return GSX_BAD_PARAM;
        return presets_rename(slot, (const char *)f->payload + 3, n);
    }
    }
}

/* ---- firmware upload in the app (A/B platforms) ----------------------- */

#if HAL_APP_FW_UPLOAD
static int cmd_fw(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (!fw)
        return GSX_BAD_STATE;               /* reboot to the bootloader first */
    switch (f->cmd) {
    case GSX_CMD_FW_BEGIN: {
        if (f->len != 17)
            return GSX_BAD_LEN;
        uint32_t size = gsx_get_u(f->payload, 5);
        uint32_t crc = gsx_get_u(f->payload + 5, 5);
        uint16_t board = (uint16_t)gsx_get_u(f->payload + 10, 2);
        uint32_t ver = gsx_get_u(f->payload + 12, 5);
        if (board != gm_device.board_id)
            return GSX_BAD_BOARD;
        if (size == 0 || (size & 3u))
            return GSX_BAD_ADDR;
        if (gm_unlock_check())
            return GSX_LOCKED;              /* sysex.md 4.9 */
        if (fw_active)
            fw->abort();
        int st = fw->begin(size, crc, board, ver);
        fw_active = (st == GSX_OK);
        if (st == GSX_OK)
            gsx_put_u(t, fw->max_chunk, 2);
        return st;
    }
    case GSX_CMD_FW_DATA: {
        if (!fw_active)
            return GSX_BAD_STATE;
        if (f->len < 5)
            return GSX_BAD_LEN;
        uint32_t off = gsx_get_u(f->payload, 4);
        /* In place: output byte 7g+i goes where input byte 8g+1+i was,
         * after it has been read. f->payload points into rx_buf. */
        uint8_t *data = (uint8_t *)(uintptr_t)(f->payload + 4);
        int n = gsx_unpack(data, f->len - 4u, data);
        if (n <= 0 || n > (int)fw->max_chunk || (n & 3))
            return GSX_BAD_LEN;
        int st = fw->data(off, data, (uint32_t)n);
        if (st == GSX_OK)
            gsx_put_u(t, off, 4);
        return st;
    }
    case GSX_CMD_FW_END: {
        if (!fw_active)
            return GSX_BAD_STATE;
        if (f->len != 0)
            return GSX_BAD_LEN;
        fw_active = 0;
        return fw->end();
    }
    default:                                /* FW_ABORT */
        if (fw_active)
            fw->abort();
        fw_active = 0;
        return GSX_OK;
    }
}
#endif

/* ---- DAW-script messages (sysex.md 4.6) ---------------------------------- */

#if GM_HOST_FB || GM_UNLOCK
static void put_lstring(gsx_tx_t *t, const char *s)
{
    uint32_t n = 0;
    while (s && s[n] && n < GSX_LSTRING_MAX)
        n++;
    gsx_put(t, (uint8_t)n);
    for (uint32_t i = 0; i < n; i++)
        gsx_put(t, (uint8_t)s[i]);
}
#endif

#if GM_UNLOCK
/* UNLOCK (sysex.md 4.9): what to ask the user. */
static int cmd_unlock(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 1)
        return GSX_BAD_LEN;
    if (f->payload[0] > 1)
        return GSX_BAD_CMD;
    uint32_t window;
    gsx_put(t, (uint8_t)gm_unlock_state(f->payload[0], &window));
    gsx_put(t, (uint8_t)window);
    gsx_put_u(t, GM_UNLOCK_HOLD_MS, 2);
    gsx_put(t, GM_UNLOCK_N);
    for (uint32_t i = 0; i < gm_device.n_buttons && i < 8u * sizeof(GM_UNLOCK_COMBO); i++)
        if (((GM_UNLOCK_COMBO) >> i) & 1u)
            put_lstring(t, gm_device.buttons[i].name);
    return GSX_OK;
}
#endif

#if GM_HOST_FB

static int cmd_get_component(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 3)
        return GSX_BAD_LEN;
    uint32_t kind = f->payload[0], index = gsx_get_u(f->payload + 1, 2), count = 0;
    const gm_comp_t *c = 0;
    switch (kind) {
    case GSX_KIND_BUTTON:
        count = gm_device.n_buttons;
        c = gm_comp_buttons;
        break;
    case GSX_KIND_MATRIX:
        count = gm_device.matrix ? 1u : 0u;
        c = gm_comp_matrix;
        break;
    case GSX_KIND_ANALOG:
        count = gm_device.n_analogs;
        c = gm_comp_analogs;
        break;
    case GSX_KIND_ENCODER:
        count = gm_device.n_encoders;
        c = gm_comp_encoders;
        break;
    case GSX_KIND_LED:
        count = gm_device.n_leds;
        c = gm_comp_leds;
        break;
    case GSX_KIND_DISPLAY:
        count = GM_N_DISPLAYS;
        c = gm_comp_displays;
        break;
#if GM_N_PADS > 0
    case GSX_KIND_PAD:                          /* the pads, then their twins */
        count = GM_N_PADS + GM_PAD_TWINS;
        c = gm_comp_pads;
        break;
#endif
    default:                                    /* GSX_KIND_METER: none yet */
        break;
    }
    if (index >= count)
        return GSX_BAD_PARAM;
    c += index;
    gsx_put(t, (uint8_t)kind);
    gsx_put_u(t, index, 2);
    gsx_put_u(t, count, 2);
    put_lstring(t, c->id);
    put_lstring(t, c->name);
    put_lstring(t, c->role);
    switch (kind) {
    case GSX_KIND_BUTTON: {
        int8_t led = gm_device.buttons[index].led;
        gsx_put_u(t, led >= 0 ? (uint32_t)led : GSX_NONE_INDEX, 2);
        gsx_put(t, gm_device.buttons[index].layer);     /* layer: 0 always, n = mode n - 1's twin */
        break;
    }
    case GSX_KIND_MATRIX:
        gsx_put_u(t, gm_device.matrix->n_keys, 2);
        gsx_put(t, gm_device.matrix->first_note);
        break;
    case GSX_KIND_ANALOG:
#if GM_ANALOG_PICKUP
        gsx_put(t, gm_analog_pickup[index] ? 1 : 0);    /* takeover */
#else
        gsx_put(t, 0);
#endif
#if GM_ANALOG_TOUCH
        gsx_put(t, gm_analog_touch[index]);             /* touch */
#else
        gsx_put(t, 0);
#endif
        gsx_put_u(t, gm_device.analogs[index].raw_min, 2);   /* adc_min, adc_max (the input tester) */
        gsx_put_u(t, gm_device.analogs[index].raw_max, 2);
        break;
    case GSX_KIND_ENCODER: {
#if GM_RINGS
        const gm_ring_t *r = &gm_encoder_ring[index];
        gsx_put(t, r->n);
        gsx_put_u(t, r->center == GM_RING_NO_CENTER ? GSX_NONE_INDEX : r->center, 2);
#else
        gsx_put(t, 0);
        gsx_put_u(t, GSX_NONE_INDEX, 2);
#endif
        uint32_t mode = (uint32_t)gm_cfg.enc_mode[index];
        if (mode == ENC_REL_SIGN)
            mode = GSX_ENC_MODE_SIGN;
        else if (mode > ENC_REL_SIGN)
            mode = 0;                                   /* the 14-bit / (N)RPN modes: absolute */
#if GM_MCU
        if (gm_encoder_mcu[index] != GM_MCU_NONE)
            mode = GSX_ENC_MODE_MCU;                    /* a V-Pot ignores encoder.mode */
#endif
        gsx_put(t, (uint8_t)mode);
        gsx_put(t, gm_device.encoders[index].layer);
        break;
    }
    case GSX_KIND_LED:
        gsx_put(t, !vpin_is_none(gm_device.leds[index].pin3) ? 3 :
                   vpin_is_none(gm_device.leds[index].pin2) ? 1 : 2);
#if GM_LED_FB
        gsx_put(t, gm_led_fb[index].owner);            /* as led_level() in controller.c */
#else
        gsx_put(t, GM_LED_OWNER);
#endif
        gsx_put(t, gm_device.leds[index].layer);
        break;
#if GM_N_PADS > 0
    case GSX_KIND_PAD: {
        int8_t led = gm_pads[index < GM_N_PADS ? index : 0].led;
        uint8_t layer = 0;
#if GM_PAD_TWINS
        if (index >= GM_N_PADS) {
            led = gm_pad_twins[index - GM_N_PADS].led;
            layer = gm_pad_twins[index - GM_N_PADS].layer;
        }
#endif
        gsx_put_u(t, led >= 0 ? (uint32_t)led : GSX_NONE_INDEX, 2);
        gsx_put(t, layer);
        break;
    }
#endif
#if GM_N_DISPLAYS > 0
    case GSX_KIND_DISPLAY:                      /* 14-segment: one row of characters */
        gsx_put(t, gm_displays[index].n_chars);
        gsx_put(t, 1);
        gsx_put(t, 1);
        break;
#endif
    default:
        break;
    }
    return GSX_OK;
}

/* A notification: no reply, whatever happens (sysex.md 4.6.2). */
static void notify(const gsx_frame_t *f)
{
    const uint8_t *p = f->payload;
    uint32_t n = f->len;
    switch (f->cmd) {
    case GSX_FB_LED:
        if (n == 3)
            gm_led_script(gsx_get_u(p, 2), p[2]);
        break;
    case GSX_FB_LED_RGB:
        if (n == 6)
            gm_led_script_rgb(gsx_get_u(p, 2), p[2], p[3], p[4], p[5] & 1u);
        break;
    case GSX_FB_LEDS:
        if (n >= 4 && p[2] >= 1 && p[2] <= 64 && n == 3u + p[2]) {
            uint32_t first = gsx_get_u(p, 2);
            for (uint32_t i = 0; i < p[2]; i++)
                gm_led_script(first + i, p[3 + i]);
        }
        break;
    case GSX_FB_VALUE:
        if (n == 5) {
            uint32_t i = gsx_get_u(p + 1, 2), v = gsx_get_u(p + 3, 2);
            if (p[0] == GSX_KIND_ANALOG && i < gm_device.n_analogs)
                gm_analog_host(i, v >> 7);
            else if (p[0] == GSX_KIND_ENCODER)
                gm_encoder_host(i, v);
        }
        break;
    case GSX_FB_RING:
        if (n == 6 && p[2] <= 3 && gsx_get_u(p, 2) < gm_device.n_encoders)
            gm_ring_draw(gsx_get_u(p, 2), p[2], (p[5] & 2u) ? -1 : (int32_t)gsx_get_u(p + 3, 2), p[5] & 1u);
        break;
    case GSX_FB_TEXT:
        if (n >= 4)
            gm_host_on_text(gsx_get_u(p, 2), p[2], p[3], p + 4, n - 4u);
        break;
    case GSX_FB_RELEASE:
        if (n == 1)
            gm_host_release(p[0]);
        break;
    default:
        break;
    }
}
#endif

static int dispatch(const gsx_frame_t *f, gsx_tx_t *t)
{
    switch (f->cmd) {
    case GSX_CMD_HELLO:
        return cmd_hello(f, t);
    case GSX_CMD_REBOOT:
        if (f->len != 1)
            return GSX_BAD_LEN;
        if (f->payload[0] > 1)
            return GSX_BAD_CMD;
        if (f->payload[0] == 1 && gm_unlock_check())
            return GSX_LOCKED;              /* sysex.md 4.9 */
        reboot_mode = f->payload[0] + 1;
        reboot_at = hal_millis() + REBOOT_DELAY_MS;
        return GSX_OK;
    case GSX_CMD_FW_BEGIN:
    case GSX_CMD_FW_DATA:
    case GSX_CMD_FW_END:
    case GSX_CMD_FW_ABORT:
#if HAL_APP_FW_UPLOAD
        return cmd_fw(f, t);
#else
        return GSX_BAD_STATE;               /* reboot to the bootloader first */
#endif
    }
    if (!has_config)
        return GSX_BAD_CMD;
    switch (f->cmd) {
    case GSX_CMD_GET_SCHEMA:
        return cmd_window(f, t, params_schema_size(), params_schema_read);
#if GM_LAYOUT_BYTES
    case GSX_CMD_GET_LAYOUT:
        return cmd_window(f, t, GM_LAYOUT_BYTES, layout_read);
#endif
#if GM_MONITOR
    case GSX_CMD_MONITOR:
        if (f->len != 1)
            return GSX_BAD_LEN;
        gsx_put(t, (uint8_t)gm_monitor(f->payload[0]));
        gsx_put_u(t, GM_MONITOR_INTERVAL_MS, 2);
        gsx_put_u(t, GM_MONITOR_TIMEOUT_MS, 2);
        return GSX_OK;
#endif
    case GSX_CMD_GET_PARAM:
        return cmd_get_param(f, t);
    case GSX_CMD_SET_PARAM:
        return cmd_set_param(f, t);
    case GSX_CMD_GET_PARAMS:
        return cmd_get_params(f, t);
    case GSX_CMD_SET_PARAMS:
        return cmd_set_params(f, t);
    case GSX_CMD_COMMIT:
        if (f->len != 0)
            return GSX_BAD_LEN;
        return presets_commit();
    case GSX_CMD_FACTORY_RESET: {
        if (f->len != 0)
            return GSX_BAD_LEN;
        if (gm_unlock_check())
            return GSX_LOCKED;              /* sysex.md 4.9 */
        int st = presets_factory_reset();
        gm_diag_refresh();                  /* params_defaults() cleared diag.* */
        app_on_param_set(0, 0);
        return st;
    }
    case GSX_CMD_PRESET_INFO:
    case GSX_CMD_PRESET_SAVE:
    case GSX_CMD_PRESET_RESET:
    case GSX_CMD_PRESET_NAME:
    case GSX_CMD_PRESET_DELETE:
    case GSX_CMD_PRESET_MOVE:
        return cmd_preset(f, t);
#if GM_HOST_FB
    case GSX_CMD_GET_COMPONENT:
        return cmd_get_component(f, t);
#endif
#if GM_UNLOCK
    case GSX_CMD_UNLOCK:
        return cmd_unlock(f, t);
#endif
    }
    return GSX_BAD_CMD;
}

/* Replies are queued like any other message, not flushed: hal_midi_poll
 * in the main loop sends them within the pass. Waiting for them held the
 * main loop until the host had taken every packet (up to 200 ms when it
 * doesn't read, tens of ms per reply on a serial or BLE link). */
static void handle_frame(void)
{
    /* Universal Identity Request: F0 7E <dev> 06 01 F7 */
    if (rx.state == 1 && rx.n == 4 && rx_buf[0] == 0x7E && rx_buf[2] == 0x06 &&
        rx_buf[3] == 0x01) {
        identity_reply();
        return;
    }
    gsx_frame_t f;
    int pst = gsx_parse(&rx, &f);
    if (pst == GSX_NOT_OURS)
        return;
#if GM_HOST_FB
    if (f.cmd >= GSX_NOTIFY_FIRST && f.cmd <= GSX_NOTIFY_LAST) {
        if (pst == GSX_OK && has_config)
            notify(&f);
        return;                             /* never answered, not even an error */
    }
#endif
#if !HAL_APP_FW_UPLOAD
    /* An FW_DATA chunk doesn't fit rx_buf here: BAD_STATE all the same, as
     * for any FW_* (reboot to the bootloader first). */
    if (pst == GSX_BAD_LEN && rx.state == 2 && f.cmd >= GSX_CMD_FW_BEGIN && f.cmd <= GSX_CMD_FW_ABORT)
        pst = GSX_BAD_STATE;
#endif
    if (pst != GSX_OK) {
        uint8_t err[GSX_OVERHEAD + 4];
        gsx_tx_t t;
        gsx_tx_begin(&t, err, f.cmd | GSX_REPLY, f.seq);
        gsx_put(&t, (uint8_t)pst);
        hal_midi_send(err, gsx_tx_end(&t));
        return;
    }
    uint32_t req = 0x10000u | (uint32_t)f.seq << 8 | f.cmd;
    gm_unlock_activity();
    if (req != last_req) {
        gsx_tx_t t;
        gsx_tx_begin(&t, tx_buf, f.cmd | GSX_REPLY, f.seq);
        uint16_t status_at = t.n;
        gsx_put(&t, 0);
        int st = dispatch(&f, &t);
        if (st != GSX_OK)
            t.n = status_at + 1u;
        tx_buf[status_at] = (uint8_t)st;
        tx_len = gsx_tx_end(&t);
        /* LOCKED doesn't complete the request (sysex.md 1): its resend
         * after the unlock is done, not answered from here. */
        last_req = st == GSX_LOCKED ? 0 : req;
    }
    hal_midi_send(tx_buf, tx_len);
}

void app_proto_rx_byte(uint8_t b)
{
    if (gsx_rx_byte(&rx, b)) {
        handle_frame();
        gsx_rx_reset(&rx);
    }
}

void app_proto_poll(void)
{
#if GM_HOST_FB
    if (booted_due && (int32_t)(hal_millis() - booted_at) >= 0) {
        booted_due = 0;
        send_booted();
    }
#endif
    if (reboot_mode && (int32_t)(hal_millis() - reboot_at) >= 0) {
#if HAL_APP_FW_UPLOAD
        if (fw_active)
            fw->abort();
#endif
        hal_midi_flush(REBOOT_DELAY_MS);    /* the REBOOT reply, if still queued */
        hal_reboot(reboot_mode == 2);
    }
}

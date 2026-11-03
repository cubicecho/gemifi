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

#define REBOOT_DELAY_MS 50u
#define SCHEMA_WINDOW   64u
#define PARAMS_RUN      16u
/* A whole frame with an n-byte payload: F0, body, F7. */
#define FRAME(n)        (GSX_OVERHEAD + 2u + (n))

uint8_t app_log_limit = LOG_DEBUG + 1;

static uint32_t fw_version;
static uint8_t has_config;
static const hal_fw_ops_t *fw;
static uint8_t fw_active;
static uint8_t data_buf[256];        /* unpacked FW_DATA chunk */
static uint8_t rx_buf[GSX_OVERHEAD + 4 + 300];   /* FW_DATA: 256 B packed */
static gsx_rx_t rx;
static uint8_t tx_buf[GSX_OVERHEAD + 1 + 6 + PARAMS_RUN * 5 + 8];
static uint16_t tx_len;
static uint32_t last_req;            /* 0x10000 | seq << 8 | cmd, 0 = none */
static uint8_t uid[12];
static uint8_t unsol_seq;
static uint8_t reboot_mode;          /* 0 none, 1 app, 2 bootloader */
static uint32_t reboot_at;

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
    fw = hal_fw_ops();
    hal_uid(uid);
    rx.buf = rx_buf;
    rx.cap = sizeof(rx_buf);
    gsx_rx_reset(&rx);
}

static uint16_t caps(void)
{
    uint16_t c = GSX_CAP_LOG | GSX_CAP_EVENT;
    if (has_config)
        c |= GSX_CAP_CONFIG | GSX_CAP_PRESETS;
    if (fw)
        c |= GSX_CAP_FW_UPLOAD;
    return c;
}

/* ---- unsolicited messages --------------------------------------------- */

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

static int cmd_hello(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 0)
        return GSX_BAD_LEN;
    gsx_put(t, 0);                          /* role: app */
    gsx_put_u(t, gm_device.board_id, 2);
    gsx_put_u(t, fw_version, 5);
    gsx_put(t, GSX_PROTO_VERSION);
    gsx_put_u(t, fw ? fw->max_chunk : 0, 2);
    gsx_put_u(t, caps(), 2);
    gsx_put_packed(t, uid, sizeof(uid));
    return GSX_OK;
}

static int cmd_get_schema(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 6)
        return GSX_BAD_LEN;
    uint32_t off = gsx_get_u(f->payload, 4);
    uint32_t max = gsx_get_u(f->payload + 4, 2);
    if (max == 0 || max > SCHEMA_WINDOW)
        return GSX_BAD_LEN;
    uint8_t data[SCHEMA_WINDOW];
    uint32_t n = params_schema_read(off, data, max);
    gsx_put_u(t, params_schema_size(), 4);
    gsx_put_u(t, off, 4);
    gsx_put_packed(t, data, n);
    return GSX_OK;
}

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
    int st = params_set(id, index, v, 1);
    if (st != GSX_OK)
        return st;
    presets_host_set(id, index);
    app_on_param_set(id, index);
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
        if (v < d->min || v > d->max)
            return GSX_BAD_VALUE;
    }
    for (uint32_t i = 0; i < n; i++) {
        d->values[index + i] = (int32_t)gsx_get_u(vp + 5 * i, 5);
        presets_host_set(id, (uint16_t)(index + i));
        app_on_param_set(id, (uint16_t)(index + i));
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
        int n = gsx_unpack(f->payload + 4, f->len - 4u, data_buf);
        if (n <= 0 || n > (int)fw->max_chunk || (n & 3))
            return GSX_BAD_LEN;
        int st = fw->data(off, data_buf, (uint32_t)n);
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
        reboot_mode = f->payload[0] + 1;
        reboot_at = hal_millis() + REBOOT_DELAY_MS;
        return GSX_OK;
    case GSX_CMD_FW_BEGIN:
    case GSX_CMD_FW_DATA:
    case GSX_CMD_FW_END:
    case GSX_CMD_FW_ABORT:
        return cmd_fw(f, t);
    }
    if (!has_config)
        return GSX_BAD_CMD;
    switch (f->cmd) {
    case GSX_CMD_GET_SCHEMA:
        return cmd_get_schema(f, t);
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
        int st = presets_factory_reset();
        app_on_param_set(0, 0);
        return st;
    }
    case GSX_CMD_PRESET_INFO:
    case GSX_CMD_PRESET_SAVE:
    case GSX_CMD_PRESET_RESET:
    case GSX_CMD_PRESET_NAME:
        return cmd_preset(f, t);
    }
    return GSX_BAD_CMD;
}

static void send_now(const uint8_t *p, uint32_t n)
{
    hal_midi_send(p, n);
    hal_midi_flush(200);
}

static void handle_frame(void)
{
    /* Universal Identity Request: F0 7E <dev> 06 01 F7 */
    if (rx.state == 1 && rx.n == 4 && rx_buf[0] == 0x7E && rx_buf[2] == 0x06 &&
        rx_buf[3] == 0x01) {
        identity_reply();
        hal_midi_flush(200);
        return;
    }
    gsx_frame_t f;
    int pst = gsx_parse(&rx, &f);
    if (pst == GSX_NOT_OURS)
        return;
    if (pst != GSX_OK) {
        uint8_t err[GSX_OVERHEAD + 4];
        gsx_tx_t t;
        gsx_tx_begin(&t, err, f.cmd | GSX_REPLY, f.seq);
        gsx_put(&t, (uint8_t)pst);
        send_now(err, gsx_tx_end(&t));
        return;
    }
    uint32_t req = 0x10000u | (uint32_t)f.seq << 8 | f.cmd;
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
        last_req = req;
    }
    send_now(tx_buf, tx_len);
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
    if (reboot_mode && (int32_t)(hal_millis() - reboot_at) >= 0) {
        if (fw_active)
            fw->abort();
        hal_reboot(reboot_mode == 2);
    }
}

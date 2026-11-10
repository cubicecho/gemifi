/*
 * app_proto.h: the app side of protocol/sysex.md, shared by gemifi apps.
 *
 * Handles HELLO, REBOOT (via the SRAM mailbox), the Universal Identity
 * Request, the config commands (GET_SCHEMA, GET/SET_PARAM(S), COMMIT,
 * FACTORY_RESET, PRESET_*; see params.h and gm_presets.h), UNLOCK (with
 * physical unlock, gm_safety.h) and answers FW_* with BAD_STATE. Sends
 * LOG, EVENT, PARAM_CHANGED, PRESET_CHANGED, UNLOCKED and, with host
 * feedback, BOOTED (once, 1.5 s after app_proto_init).
 *
 * Platform independent: it talks to the chip only through gm_hal.h (and
 * the store through gm_presets.h). FW_* go to hal_fw_ops() when the platform has one (A/B
 * chips, sysex.md 4 "Who takes uploads"), else get BAD_STATE.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_APP_PROTO_H
#define GEMIFI_APP_PROTO_H

#include <stdint.h>
#include "gsx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_ERROR 0
#define LOG_WARN  1
#define LOG_INFO  2
#define LOG_DEBUG 3

#define EV_BUTTON  1
#define EV_KEY     2
#define EV_ANALOG  3
#define EV_ENCODER 4
#define EV_TOUCH   5    /* analog index; 1 touched, 0 released */
#define EV_PAD     7    /* pad twins only: value = peak reading x 4 on a hit, 0 on release */
#define EV_LAYER   8    /* mode index; 1 its layer turned on, 0 off (core/README.md "Cycles") */

/* has_config: the app has a params.h table (and config_store). Caps and
 * the upload role come from that and hal_fw_ops(). */
void app_proto_init(uint32_t fw_version, int has_config);

/* Feed every received MIDI byte (the controller does this for you). */
void app_proto_rx_byte(uint8_t b);

/* Does a pending REBOOT once its reply has had time to go out. */
void app_proto_poll(void);

/* The chip UID as 24 hex digits + NUL (USB serial-number string). */
void app_proto_uid_hex(char out[25]);

/* Messages below this level are dropped (0 = no logs, 4 = everything). */
extern uint8_t app_log_limit;

void app_log(uint8_t level, const char *a, const char *b);
void app_log_hex(uint8_t level, const char *a, uint32_t v);
void app_event(uint8_t type, uint32_t index, uint32_t value);
void app_param_changed(uint16_t id, uint16_t index);
/* Unsolicited PRESET_CHANGED (slot u14, dirty byte). */
void app_preset_changed(uint32_t slot, uint32_t dirty);

/* Unsolicited UNLOCKED (sysex.md 4.9): 1 unlocked, 0 locked again or the
 * unlock window closed. */
void app_unlocked(uint8_t state);
/* An unsolicited message (cmd 0x60-0x7F) with a payload of 7-bit bytes,
 * framed with the running device seq. */
void app_send_unsolicited(uint8_t cmd, const uint8_t *payload, uint32_t n);

/* Raw readings (sysex.md 4.8): MONITOR and RAW. On by default; a build can
 * set GM_MONITOR 0 to leave them out. The controller (controller.c) streams
 * them from gm_poll. */
#ifndef GM_MONITOR
#define GM_MONITOR 1
#endif
#define GM_MONITOR_INTERVAL_MS 20u      /* one RAW message per 20 ms at most */
#define GM_MONITOR_TIMEOUT_MS  5000u    /* stops this long after the last MONITOR */
/* MONITOR: start, renew or (kinds 0) stop the stream. Returns the kinds
 * (GSX_MON_*) the device has of those asked for, which it now sends. */
uint32_t gm_monitor(uint32_t kinds);

/* Optional hook (weak, default does nothing): a param changed through
 * SET_PARAM(S) or FACTORY_RESET. id 0 = everything changed. */
void app_on_param_set(uint16_t id, uint16_t index);

#ifdef __cplusplus
}
#endif

#endif

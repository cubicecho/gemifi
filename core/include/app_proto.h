/*
 * app_proto.h: the app side of protocol/sysex.md, shared by gemifi apps.
 *
 * Handles HELLO, REBOOT (via the SRAM mailbox), the Universal Identity
 * Request, the config commands (GET_SCHEMA, GET/SET_PARAM(S), COMMIT,
 * FACTORY_RESET, PRESET_*; see params.h and gm_presets.h) and answers FW_*
 * with BAD_STATE. Sends LOG, EVENT, PARAM_CHANGED and PRESET_CHANGED.
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

/* Optional hook (weak, default does nothing): a param changed through
 * SET_PARAM(S) or FACTORY_RESET. id 0 = everything changed. */
void app_on_param_set(uint16_t id, uint16_t index);

#ifdef __cplusplus
}
#endif

#endif

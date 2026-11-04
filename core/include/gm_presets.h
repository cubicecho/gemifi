/*
 * gm_presets.h: presets (research/core/presets-design.md §3, §10.3, §10.4).
 *
 * Only the active preset is in RAM (gm_cfg, with the globals). Preset n's
 * values = the param defaults for the PF_PRESET params, then gm_presets[n-1]
 * (the YAML overrides, flash), then slot n's stored record (config_store.h:
 * the values that differ from its YAML values). Names and info are read from
 * the store on demand; nothing is kept per slot.
 *
 * Slots are 1..GM_N_PRESETS. Functions returning int return a GSX status.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_PRESETS_H
#define GEMIFI_GM_PRESETS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRESET_NAME_MAX 16u

/* PRESET_INFO flags (sysex.md) */
#define PRESET_INFO_STORED 0x01u     /* the slot has stored edits (or a name) */
#define PRESET_INFO_DIRTY  0x02u     /* active slot with unsaved host edits   */
#define PRESET_INFO_YAML   0x04u     /* defined in the device YAML            */

typedef struct {
    uint16_t slot;
    uint8_t flags;                   /* PRESET_INFO_*                         */
    uint16_t stored_revision;        /* 0 if no stored edits                  */
    int32_t program;                 /* -1 none                               */
    char name[PRESET_NAME_MAX + 1];
} preset_info_t;

/* gm_init: params_defaults(), then (unless safe) the stored globals (slot
 * 0, including preset.active), then the active slot. safe = ignore the
 * store when loading (it is still written by COMMIT). */
void presets_boot(int safe);

/* gm_poll: writes preset.active once it has been stable for 10 s. */
void presets_poll(uint32_t now_ms);

/* Write preset.active now if it is still waiting for those 10 s (before a
 * power-off). Unsaved edits stay unsaved: those are only stored by COMMIT. */
void presets_flush(void);

/* Switch (device or host): release everything held, load the slot, send
 * its Program Change, PRESET_CHANGED. Selecting the active slot reloads
 * it (drops unsaved edits, sysex.md §4.5). GSX_BAD_PARAM for a bad slot. */
int presets_select(uint32_t slot);
void presets_step(int dir);          /* +1 next, -1 previous (wraps) */
uint32_t presets_active(void);

/* app_proto: the host changed (id, index) with SET_PARAM(S) (or a mode key
 * changed a per-preset param on the device): a PF_PRESET
 * param sets dirty, preset.active switches. */
void presets_host_set(uint16_t id, uint16_t index);

int presets_commit(void);                        /* COMMIT */
int presets_save(uint32_t slot);                 /* PRESET_SAVE */
int presets_reset(uint32_t slot);                /* PRESET_RESET, 0 = all */
int presets_rename(uint32_t slot, const char *name, uint32_t len);   /* PRESET_NAME */
int presets_info(uint32_t slot, preset_info_t *out);   /* PRESET_INFO, 0 = active */
int presets_factory_reset(void);                 /* FACTORY_RESET */

#ifdef __cplusplus
}
#endif

#endif

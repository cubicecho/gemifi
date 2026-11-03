/*
 * params.h: the app's config profile (protocol/sysex.md 4.3, 4.4).
 *
 * The app describes each param with a param_def_t; its value(s) live in the
 * app's own RAM (`values`), so app code reads them directly. This module
 * validates GET/SET_PARAM, applies defaults and serialises the schema blob
 * the editor reads with GET_SCHEMA. Plain C, no hardware access, so it also
 * builds for the host tests.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_PARAMS_H
#define GEMIFI_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARAMS_SCHEMA_VERSION 1

/* param types (sysex.md 4.4) */
#define PT_INT  0
#define PT_BOOL 1
#define PT_ENUM 2
#define PT_NOTE 3

/* param flags */
#define PF_READ_ONLY         (1u << 0)
#define PF_VOLATILE          (1u << 1)   /* not stored by COMMIT        */
#define PF_PER_INDEX_DEFAULT (1u << 2)   /* set by params.c, not by hand */
#define PF_INDEX_LABELS      (1u << 3)   /* set by params.c, not by hand */
#define PF_HAS_UNIT          (1u << 4)   /* set by params.c, not by hand */
#define PF_PRESET            (1u << 5)   /* per preset (gm_presets.h)    */

typedef struct {
    uint16_t id;                     /* 1..0x3FFF, never reused          */
    uint8_t type;
    uint8_t group;
    uint16_t count;                  /* 1 = plain param, <= 0x3FFF       */
    uint8_t flags;                   /* PF_READ_ONLY / PF_VOLATILE / PF_PRESET */
    uint8_t option_count;            /* PT_ENUM: number of options       */
    int32_t min, max;
    int32_t def;                     /* default when defs is NULL        */
    const int32_t *defs;             /* per-index defaults, or NULL      */
    const char *name;                /* "midi.channel"                   */
    const char *label;               /* "MIDI channel"                   */
    const char *unit;                /* "ms", or NULL                    */
    const char *const *index_labels; /* count entries, or NULL           */
    const char *const *options;      /* PT_ENUM: option_count entries    */
    int32_t *values;                 /* count entries in app RAM         */
} param_def_t;

typedef struct {
    const char *const *groups;
    uint8_t group_count;
    uint16_t param_count;
    const param_def_t *params;
} param_table_t;

/* Provided by the app. */
extern const param_table_t app_params;

void params_defaults(void);
/* Default of index i (the per-index default, or def). */
int32_t params_default_at(const param_def_t *p, uint32_t i);
const param_def_t *params_find(uint16_t id);

/* Each returns a GSX status: GSX_OK, GSX_BAD_PARAM or GSX_BAD_VALUE.
 * params_set refuses READ_ONLY params only when from_host is set. */
int params_get(uint16_t id, uint16_t index, int32_t *out);
int params_set(uint16_t id, uint16_t index, int32_t value, int from_host);

/* Schema blob: total size, and a window of it (returns bytes copied). */
uint32_t params_schema_size(void);
uint32_t params_schema_read(uint32_t offset, uint8_t *dst, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif

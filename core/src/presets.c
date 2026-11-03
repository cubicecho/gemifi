/*
 * presets.c: preset load, switch, dirty tracking and storage
 * (research/core/presets-design.md §3, §10.3; gm_presets.h).
 *
 * RAM: a few bytes. Records are built on the stack (one cs_entry_t per
 * param value at most) only while writing.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_display.h"
#include "gm_presets.h"
#include "gm_settings.h"
#include "gm_device.h"
#include "gm_controller.h"
#include "gm_hal.h"
#include "params.h"
#include "config_store.h"
#include "app_proto.h"
#include "gsx.h"

#define ACTIVE_STABLE_MS 10000u

/* Upper bound on the values one record can hold: every param value. The
 * per-component arrays, plus 16 for the single-value params, plus the pad
 * notes and the 8 pad settings (pad.*) on a device with pads. put() and
 * collect_cb() never write past it. */
#define PAD_ENTRIES (GM_N_PADS > 0 ? (uint32_t)GM_N_PADS + 8u : 0u)
#define MAX_ENTRIES (2u * GM_ARR(GM_N_BUTTONS) + GM_ARR(GM_N_ANALOGS) + \
                     2u * GM_ARR(GM_N_ENCODERS) + 16u + PAD_ENTRIES)

static uint8_t cur;                  /* active slot, 1..GM_N_PRESETS */
static uint8_t no_store;             /* safe mode: loads ignore the store */
static uint8_t active_pending;       /* preset.active not written yet */
static uint32_t active_t;

static void preset_changed(void);

/* ---- helpers ------------------------------------------------------------ */

static int is_preset(const param_def_t *p)
{
    return (p->flags & PF_PRESET) != 0;
}

/* What COMMIT stores at all. */
static int storable(const param_def_t *p)
{
    return !(p->flags & (PF_VOLATILE | PF_READ_ONLY));
}

static const gm_preset_def_t *def_of(uint32_t slot)
{
    return &gm_presets[slot - 1u];
}

/* Slot's YAML value of a PF_PRESET param: default, then the override. */
static int32_t yaml_value(uint32_t slot, const param_def_t *p, uint32_t i)
{
    const gm_preset_def_t *d = def_of(slot);
    int32_t v = params_default_at(p, i);
    if (p->id == P_PRESET_PROGRAM)
        v = d->program;
    for (uint32_t k = 0; k < d->n_vals; k++)
        if (d->vals[k].id == p->id && d->vals[k].index == i)
            return d->vals[k].value;
    return v;
}

static int yaml_defined(uint32_t slot)
{
#ifdef GM_N_YAML_PRESETS
    return slot <= GM_N_YAML_PRESETS;
#else
    /* Empty slots: no overrides, no program, named "Preset N". */
    const gm_preset_def_t *d = def_of(slot);
    if (slot == 1 || d->n_vals || d->program != GM_PROGRAM_NONE)
        return 1;
    const char *n = d->name;
    static const char pfx[] = "Preset ";
    for (uint32_t i = 0; i < sizeof(pfx) - 1u; i++)
        if (!n || n[i] != pfx[i])
            return 1;
    uint32_t v = 0;
    for (n += sizeof(pfx) - 1u; *n >= '0' && *n <= '9'; n++)
        v = v * 10u + (uint32_t)(*n - '0');
    return *n || v != slot;
#endif
}

/* Store callback: apply one stored value if it belongs to this scope. */
static void apply_cb(const cs_entry_t *e, void *ctx)
{
    int want_preset = *(const int *)ctx;
    const param_def_t *p = params_find(e->id);
    if (p && storable(p) && is_preset(p) == want_preset)
        params_set(e->id, e->index, e->value, 0);
}

typedef struct {
    cs_entry_t *e;
    uint32_t n;
    int want_preset;
} collect_t;

/* Store callback: copy the stored entries (of params this firmware has). */
static void collect_cb(const cs_entry_t *e, void *ctx)
{
    collect_t *c = ctx;
    const param_def_t *p = params_find(e->id);
    if (c->n < MAX_ENTRIES && p && storable(p) && is_preset(p) == c->want_preset &&
        e->index < p->count)
        c->e[c->n++] = *e;
}

static void put(cs_entry_t *e, uint32_t *n, uint16_t id, uint32_t i, int32_t v)
{
    if (*n < MAX_ENTRIES) {
        e[*n].id = id;
        e[*n].index = (uint16_t)i;
        e[*n].value = v;
        (*n)++;
    }
}

static void set_dirty(int d)
{
    if (gm_cfg.preset_dirty == d)
        return;
    gm_cfg.preset_dirty = d;
    preset_changed();
}

static void preset_changed(void)
{
    app_preset_changed(cur, (uint32_t)gm_cfg.preset_dirty);
}

/* ---- loading ------------------------------------------------------------ */

static void load_slot(uint32_t slot)
{
    for (uint32_t k = 0; k < app_params.param_count; k++) {
        const param_def_t *p = &app_params.params[k];
        if (is_preset(p))
            for (uint32_t i = 0; i < p->count; i++)
                p->values[i] = params_default_at(p, i);
    }
    const gm_preset_def_t *d = def_of(slot);
    gm_cfg.preset_program = d->program;     /* also without the param (1 slot) */
    for (uint32_t k = 0; k < d->n_vals; k++)
        params_set(d->vals[k].id, d->vals[k].index, d->vals[k].value, 0);
    if (!no_store) {
        int want_preset = 1;
        config_store_read((uint8_t)slot, 0, apply_cb, &want_preset);
    }
    cur = (uint8_t)slot;
    gm_cfg.preset_active = (int32_t)slot;
    gm_cfg.preset_dirty = 0;
}

void presets_boot(int safe)
{
    no_store = (uint8_t)(safe != 0);
    params_defaults();
    if (!no_store) {
        int want_preset = 0;
        config_store_read(CS_SLOT_GLOBAL, 0, apply_cb, &want_preset);
    }
    uint32_t slot = (uint32_t)gm_cfg.preset_active;
    if (slot < 1u || slot > GM_N_PRESETS)
        slot = 1;
    load_slot(slot);
    active_pending = 0;
}

uint32_t presets_active(void)
{
    return cur;
}

/* ---- switching ---------------------------------------------------------- */

static void do_select(uint32_t slot, int from_host)
{
    gm_release_all();
    load_slot(slot);
    if (gm_cfg.preset_program >= 0) {
        uint8_t m[2] = { (uint8_t)(0xC0 | ((gm_cfg.channel - 1) & 0x0F)),
                         (uint8_t)(gm_cfg.preset_program & 0x7F) };
        hal_midi_send(m, 2);
        gm_display_midi(m[0], m[1], 0);
    }
    if (!from_host)
        app_param_changed(P_PRESET_ACTIVE, 0);   /* the host knows its own SETs */
    preset_changed();
    active_pending = 1;
    active_t = hal_millis();
}

static int select_from(uint32_t slot, int from_host)
{
    if (slot < 1u || slot > GM_N_PRESETS)
        return GSX_BAD_PARAM;
    do_select(slot, from_host);             /* the active one: reload, drop edits */
    return GSX_OK;
}

int presets_select(uint32_t slot)
{
    return select_from(slot, 0);
}

void presets_step(int dir)
{
    uint32_t n = GM_N_PRESETS;
    presets_select(dir > 0 ? cur % n + 1u : (cur + n - 2u) % n + 1u);
}

void presets_host_set(uint16_t id, uint16_t index)
{
    (void)index;
    if (id == P_PRESET_ACTIVE) {
        /* params_set already wrote the new value; cur is still the old slot */
        uint32_t slot = (uint32_t)gm_cfg.preset_active;
        gm_cfg.preset_active = cur;
        select_from(slot, 1);
        return;
    }
    const param_def_t *p = params_find(id);
    if (p && is_preset(p) && storable(p))
        set_dirty(1);
}

/* ---- storing ------------------------------------------------------------ */

/* The stored name of a slot ("" if none), and whether it has a record. */
static int stored_info(uint32_t slot, cs_slot_info_t *info)
{
    info->name[0] = 0;
    info->flags = 0;
    info->revision = 0;
    int has = config_store_read((uint8_t)slot, info, 0, 0);
    if (!has || !(info->flags & CS_F_NAME))
        info->name[0] = 0;
    return has;
}

/* Write slot's record: the live per-preset values that differ from slot's
 * YAML values, keeping its stored name. An empty record with no name drops
 * the slot's stored record instead (or writes nothing if it has none). */
static int write_preset(uint32_t slot)
{
    cs_entry_t e[MAX_ENTRIES];
    uint32_t n = 0;
    for (uint32_t k = 0; k < app_params.param_count; k++) {
        const param_def_t *p = &app_params.params[k];
        if (!is_preset(p) || !storable(p))
            continue;
        for (uint32_t i = 0; i < p->count; i++)
            if (p->values[i] != yaml_value(slot, p, i))
                put(e, &n, p->id, i, p->values[i]);
    }
    cs_slot_info_t info;
    int has = stored_info(slot, &info);
    if (!n && !info.name[0])
        return has ? config_store_erase_slot((uint8_t)slot) : GSX_OK;
    return config_store_write((uint8_t)slot, GM_PRESETS_REVISION,
                              info.name[0] ? info.name : 0, e, n);
}

/* Write slot 0: every global value. */
static int write_globals(void)
{
    cs_entry_t e[MAX_ENTRIES];
    uint32_t n = 0;
    for (uint32_t k = 0; k < app_params.param_count; k++) {
        const param_def_t *p = &app_params.params[k];
        if (is_preset(p) || !storable(p))
            continue;
        for (uint32_t i = 0; i < p->count; i++)
            put(e, &n, p->id, i, p->values[i]);
    }
    int st = config_store_write(CS_SLOT_GLOBAL, GM_PRESETS_REVISION, 0, e, n);
    if (st == GSX_OK)
        active_pending = 0;
    return st;
}

/* Just preset.active into slot 0, keeping the other stored globals (not
 * the live ones: those are only stored by COMMIT). */
static void write_active(void)
{
    cs_entry_t e[MAX_ENTRIES];
    collect_t c = { e, 0, 0 };
    cs_slot_info_t info;
    int has = config_store_read(CS_SLOT_GLOBAL, &info, collect_cb, &c);
    uint32_t k;
    for (k = 0; k < c.n; k++)
        if (e[k].id == P_PRESET_ACTIVE)
            break;
    active_pending = 0;
    if (k < c.n) {
        if (e[k].value == (int32_t)cur)
            return;
        e[k].value = (int32_t)cur;
    } else {
        if (cur == 1u)
            return;                         /* the default: nothing to store */
        put(e, &c.n, P_PRESET_ACTIVE, 0, (int32_t)cur);
    }
    if (config_store_write(CS_SLOT_GLOBAL, has ? info.revision : GM_PRESETS_REVISION,
                           0, e, c.n) != GSX_OK)
        app_log(LOG_ERROR, "preset.active", " not stored");
}

void presets_poll(uint32_t now)
{
    if (active_pending && now - active_t >= ACTIVE_STABLE_MS)
        write_active();
}

int presets_save(uint32_t slot)
{
    if (slot < 1u || slot > GM_N_PRESETS)
        return GSX_BAD_PARAM;
    int st = write_preset(slot);
    if (st != GSX_OK)
        return st;
    int changed = slot != cur;
    cur = (uint8_t)slot;
    gm_cfg.preset_active = (int32_t)slot;
    st = write_globals();
    if (st != GSX_OK)
        return st;
    if (changed || gm_cfg.preset_dirty) {
        gm_cfg.preset_dirty = 0;
        preset_changed();
    }
    return GSX_OK;
}

int presets_commit(void)
{
    return presets_save(cur);
}

int presets_reset(uint32_t slot)
{
    if (slot > GM_N_PRESETS)
        return GSX_BAD_PARAM;
    uint32_t a = slot ? slot : 1u, b = slot ? slot : GM_N_PRESETS;
    int st = GSX_OK;
    for (uint32_t s = a; s <= b && st == GSX_OK; s++)
        if (config_store_read((uint8_t)s, 0, 0, 0))
            st = config_store_erase_slot((uint8_t)s);
    if (slot == 0 || slot == cur)
        do_select(cur, 1);                  /* reload the active one */
    return st;
}

int presets_rename(uint32_t slot, const char *name, uint32_t len)
{
    if (slot < 1u || slot > GM_N_PRESETS)
        return GSX_BAD_PARAM;
    if (len > PRESET_NAME_MAX)
        return GSX_BAD_VALUE;
    char nm[PRESET_NAME_MAX + 1];
    for (uint32_t i = 0; i < len; i++) {
        if (name[i] < 0x20 || name[i] > 0x7E)
            return GSX_BAD_VALUE;
        nm[i] = name[i];
    }
    nm[len] = 0;
    cs_entry_t e[MAX_ENTRIES];
    collect_t c = { e, 0, 1 };
    cs_slot_info_t info;
    int has = config_store_read((uint8_t)slot, &info, collect_cb, &c);
    if (!len && !c.n)
        return has ? config_store_erase_slot((uint8_t)slot) : GSX_OK;
    return config_store_write((uint8_t)slot, has ? info.revision : GM_PRESETS_REVISION,
                              len ? nm : 0, e, c.n);
}

typedef struct {
    int32_t program;
} prog_t;

static void program_cb(const cs_entry_t *e, void *ctx)
{
    if (e->id == P_PRESET_PROGRAM && e->index == 0)
        ((prog_t *)ctx)->program = e->value;
}

int presets_info(uint32_t slot, preset_info_t *out)
{
    if (slot == 0)
        slot = cur;
    if (slot > GM_N_PRESETS)
        return GSX_BAD_PARAM;
    const gm_preset_def_t *d = def_of(slot);
    cs_slot_info_t info;
    prog_t pr = { d->program };
    info.flags = 0;
    info.name[0] = 0;
    int has = config_store_read((uint8_t)slot, &info, program_cb, &pr);
    out->slot = (uint16_t)slot;
    out->flags = (uint8_t)((has ? PRESET_INFO_STORED : 0) |
                           (yaml_defined(slot) ? PRESET_INFO_YAML : 0) |
                           (slot == cur && gm_cfg.preset_dirty ? PRESET_INFO_DIRTY : 0));
    out->stored_revision = has ? info.revision : 0;
    out->program = slot == cur ? gm_cfg.preset_program : pr.program;
    const char *n = has && (info.flags & CS_F_NAME) && info.name[0] ? info.name : d->name;
    uint32_t i = 0;
    for (; n && n[i] && i < PRESET_NAME_MAX; i++)
        out->name[i] = n[i];
    out->name[i] = 0;
    return GSX_OK;
}

int presets_factory_reset(void)
{
    int st = config_store_erase();
    gm_release_all();
    params_defaults();
    load_slot(1);
    active_pending = 0;
    preset_changed();
    return st;
}

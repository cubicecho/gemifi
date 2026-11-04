/*
 * presets.c: preset load, switch, dirty tracking and storage
 * (research/core/presets-design.md §3, §10.3; gm_presets.h).
 *
 * RAM: one record of entries (rec[], one cs_entry_t per param value at
 * most) that every save builds its record in, and a few bytes. Moving a
 * preset also holds it on the stack while the others move.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_display.h"
#include "gm_din.h"
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
 * per-component arrays (the layer twins' layer.* included), plus 16 for the
 * single-value params, plus the pad notes and the 8 pad settings (pad.*) on
 * a device with pads, plus layer.enabled. put() and collect_cb() never
 * write past it. */
#define PAD_ENTRIES (GM_N_PADS > 0 ? (uint32_t)GM_N_PADS + 8u : 0u)
#define LAYER_ENTRIES (GM_LAYER_PARAMS ? (uint32_t)GM_N_MODES : 0u)
#define MAX_ENTRIES (2u * GM_ARR(GM_N_BUTTONS) + GM_ARR(GM_N_ANALOGS) + \
                     2u * GM_ARR(GM_N_ENCODERS) + 16u + PAD_ENTRIES + LAYER_ENTRIES)

/* The record a save builds: write_values(), write_globals(), write_state()
 * and presets_rename() build theirs here, check it with stored_same() and
 * write it. Static: as an array on each one's stack it took 1.9 KB on the
 * Code 61, and two of them were live at once under presets_rename() and
 * presets_move() (with write_state() or write_values()). The saves never
 * nest: each is done with rec[] when it returns, and none is called while
 * another builds (the store callbacks only read it). */
static cs_entry_t rec[MAX_ENTRIES];

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

/* What COMMIT stores at all. preset.count is read-only to a host but the
 * device's own state, so it is stored with the globals. */
static int storable(const param_def_t *p)
{
    return !(p->flags & PF_VOLATILE) && (!(p->flags & PF_READ_ONLY) || p->id == P_PRESET_COUNT);
}

/* Presets in use: slots 1..used(); the ones after are free (sysex.md §4.5).
 * Deleting moves the later presets up, so the free ones are always last. */
static uint32_t used(void)
{
    int32_t n = gm_cfg.preset_count;
    return n < 1 ? 1u : n > (int32_t)GM_N_PRESETS ? GM_N_PRESETS : (uint32_t)n;
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
    /* A store written before preset.count (or by an older firmware) may
     * hold presets past it: they stay in use. */
    if (!no_store)
        for (uint32_t s = GM_N_PRESETS; s > used(); s--)
            if (config_store_read((uint8_t)s, 0, 0, 0)) {
                gm_cfg.preset_count = (int32_t)s;
                break;
            }
    uint32_t slot = (uint32_t)gm_cfg.preset_active;
    if (slot < 1u || slot > used())
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
        gm_din_local(m, 2);
        gm_display_midi(m[0], m[1], 0);
    }
    if (!from_host)
        app_param_changed(P_PRESET_ACTIVE, 0);   /* the host knows its own SETs */
    preset_info_t info;
    if (presets_info(slot, &info) == GSX_OK)
        gm_display_message(info.name);          /* over the Program Change, if any */
    preset_changed();
    active_pending = 1;
    active_t = hal_millis();
}

static int select_from(uint32_t slot, int from_host)
{
    if (slot < 1u || slot > used())
        return GSX_BAD_PARAM;               /* free slots are not presets yet */
    do_select(slot, from_host);             /* the active one: reload, drop edits */
    return GSX_OK;
}

int presets_select(uint32_t slot)
{
    return select_from(slot, 0);
}

void presets_step(int dir)
{
    uint32_t n = used();
    presets_select(dir > 0 ? cur % n + 1u : (cur + n - 2u) % n + 1u);
}

int presets_value_ok(uint16_t id, int32_t value)
{
    return id != P_PRESET_ACTIVE || (value >= 1 && (uint32_t)value <= used());
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

typedef struct {
    const cs_entry_t *e;
    uint32_t n;
    uint32_t seen;
    int diff;
} same_t;

/* Store callback: is this stored entry one of c->e, with the same value? */
static void same_cb(const cs_entry_t *e, void *ctx)
{
    same_t *c = ctx;
    c->seen++;
    for (uint32_t k = 0; k < c->n; k++)
        if (c->e[k].id == e->id && c->e[k].index == e->index) {
            if (c->e[k].value != e->value)
                c->diff = 1;
            return;
        }
    c->diff = 1;
}

/* Does slot's stored record already hold exactly these entries, under this
 * revision and name (NULL: none)? No record counts as an empty one. Then a
 * save writes nothing (flash wear: COMMIT rewrites both records). */
static int stored_same(uint32_t slot, uint16_t revision, const char *name, const cs_entry_t *e, uint32_t n)
{
    same_t c = { e, n, 0, 0 };
    cs_slot_info_t info;
    info.flags = 0;
    info.name[0] = 0;
    if (!config_store_read((uint8_t)slot, &info, same_cb, &c))
        return !n && !name;
    const char *a = name ? name : "", *b = (info.flags & CS_F_NAME) ? info.name : "";
    uint32_t i = 0;
    while (a[i] && a[i] == b[i])
        i++;
    return !c.diff && c.seen == n && info.revision == revision && a[i] == b[i];
}

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

/* Write slot's record under `name` (NULL: none): the per-preset values that
 * differ from slot's YAML values. The values are the live ones, or with
 * yaml_of > 0 that slot's YAML values (a copy of a firmware preset). An
 * empty record with no name drops the slot's stored record instead (or
 * writes nothing if it has none). */
static int write_values(uint32_t slot, const char *name, uint32_t yaml_of)
{
    cs_entry_t *e = rec;
    uint32_t n = 0;
    for (uint32_t k = 0; k < app_params.param_count; k++) {
        const param_def_t *p = &app_params.params[k];
        if (!is_preset(p) || !storable(p))
            continue;
        for (uint32_t i = 0; i < p->count; i++) {
            int32_t v = yaml_of ? yaml_value(yaml_of, p, i) : p->values[i];
            if (v != yaml_value(slot, p, i))
                put(e, &n, p->id, i, v);
        }
    }
    if (name && !name[0])
        name = 0;
    if (!n && !name)
        return config_store_read((uint8_t)slot, 0, 0, 0) ? config_store_erase_slot((uint8_t)slot) : GSX_OK;
    if (stored_same(slot, GM_PRESETS_REVISION, name, e, n))
        return GSX_OK;
    return config_store_write((uint8_t)slot, GM_PRESETS_REVISION, name, e, n);
}

/* Write slot's record from the live values, keeping its stored name. */
static int write_preset(uint32_t slot)
{
    cs_slot_info_t info;
    stored_info(slot, &info);
    return write_values(slot, info.name, 0);
}

/* Write slot 0: the global values that differ from their defaults (a boot
 * loads the defaults first; preset.active 1 is the default too). */
static int write_globals(void)
{
    cs_entry_t *e = rec;
    uint32_t n = 0;
    for (uint32_t k = 0; k < app_params.param_count; k++) {
        const param_def_t *p = &app_params.params[k];
        if (is_preset(p) || !storable(p))
            continue;
        for (uint32_t i = 0; i < p->count; i++)
            if (p->values[i] != params_default_at(p, i))
                put(e, &n, p->id, i, p->values[i]);
    }
    int st = stored_same(CS_SLOT_GLOBAL, GM_PRESETS_REVISION, 0, e, n) ? GSX_OK :
             config_store_write(CS_SLOT_GLOBAL, GM_PRESETS_REVISION, 0, e, n);
    if (st == GSX_OK)
        active_pending = 0;
    return st;
}

/* Set (id, value) in a record being built: an entry equal to the param's
 * default is left out, as write_globals() does. */
static void set_entry(cs_entry_t *e, uint32_t *n, uint16_t id, int32_t value)
{
    const param_def_t *p = params_find(id);
    uint32_t k;
    for (k = 0; k < *n; k++)
        if (e[k].id == id)
            break;
    int is_default = !p || value == params_default_at(p, 0);
    if (k < *n) {
        if (is_default)
            e[k] = e[--*n];
        else
            e[k].value = value;
    } else if (!is_default) {
        put(e, n, id, 0, value);
    }
}

/* preset.active and preset.count into slot 0, keeping the other stored
 * globals (not the live ones: those are only stored by COMMIT). A record
 * that would not change is not written (flash wear). */
static int write_state(void)
{
    cs_entry_t *e = rec;
    collect_t c = { e, 0, 0 };
    cs_slot_info_t info;
    int has = config_store_read(CS_SLOT_GLOBAL, &info, collect_cb, &c);
    uint16_t revision = has ? info.revision : GM_PRESETS_REVISION;
    set_entry(e, &c.n, P_PRESET_ACTIVE, (int32_t)cur);
#if GM_N_PRESETS > 1
    set_entry(e, &c.n, P_PRESET_COUNT, (int32_t)used());
#endif
    active_pending = 0;
    if (stored_same(CS_SLOT_GLOBAL, revision, 0, e, c.n))
        return GSX_OK;
    int st = config_store_write(CS_SLOT_GLOBAL, revision, 0, e, c.n);
    if (st != GSX_OK)
        app_log(LOG_ERROR, "presets", " state not stored");
    return st;
}

void presets_poll(uint32_t now)
{
    if (active_pending && now - active_t >= ACTIVE_STABLE_MS)
        (void)write_state();
}

void presets_flush(void)
{
    if (active_pending)
        (void)write_state();
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
    if (slot > used())
        gm_cfg.preset_count = (int32_t)slot;    /* a free slot: now in use */
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
    if (slot == 0) {
        /* The firmware's presets: the YAML's list is what is in use. */
        const param_def_t *p = params_find(P_PRESET_COUNT);
        gm_cfg.preset_count = p ? params_default_at(p, 0) : 1;
        if (cur > used())
            cur = 1;
        if (st == GSX_OK)
            st = write_state();
    }
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
    if (slot > used()) {
        /* Naming a free slot makes it a new preset: a copy of preset 1's
         * firmware values, under that name. */
        int st = write_values(slot, nm, 1);
        if (st != GSX_OK)
            return st;
        gm_cfg.preset_count = (int32_t)slot;
        return write_state();
    }
    cs_entry_t *e = rec;
    collect_t c = { e, 0, 1 };
    cs_slot_info_t info;
    int has = config_store_read((uint8_t)slot, &info, collect_cb, &c);
    int st;
    if (!len && !c.n)
        st = has ? config_store_erase_slot((uint8_t)slot) : GSX_OK;
    else
        st = config_store_write((uint8_t)slot, has ? info.revision : GM_PRESETS_REVISION,
                                len ? nm : 0, e, c.n);
    preset_info_t now;
    if (st == GSX_OK && slot == cur && presets_info(slot, &now) == GSX_OK)
        gm_display_message(now.name);       /* the active preset's new name, at once */
    return st;
}

/* The name slot `from` shows, to store when it moves to slot `to` (NULL when
 * `to` would show it anyway): its stored name, else its YAML name. An empty
 * slot's "Preset N" is not carried along: it becomes `to`'s own. */
static const char *moved_name(uint32_t from, uint32_t to, cs_slot_info_t *info)
{
    const char *n = stored_info(from, info) && info->name[0] ? info->name :
                    yaml_defined(from) ? def_of(from)->name : 0;
    const char *own = def_of(to)->name;
    uint32_t i = 0;
    while (n && own && n[i] && n[i] == own[i])
        i++;
    return n && own && n[i] == own[i] ? 0 : n;
}

int presets_delete(uint32_t slot)
{
    uint32_t n = used();
    if (slot < 1u || slot > n)
        return GSX_BAD_PARAM;
    if (n == 1u)
        return GSX_BAD_STATE;               /* the last preset stays */
    uint32_t was = cur;
    gm_release_all();
    /* Each later preset moves up one slot: loaded, then stored as the next
     * slot's record (values that differ from that slot's YAML values). */
    int st = GSX_OK;
    for (uint32_t s = slot; s < n && st == GSX_OK; s++) {
        cs_slot_info_t info;
        const char *name = moved_name(s + 1u, s, &info);
        load_slot(s + 1u);
        st = write_values(s, name, 0);
    }
    if (st == GSX_OK && config_store_read((uint8_t)n, 0, 0, 0))
        st = config_store_erase_slot((uint8_t)n);
    if (st != GSX_OK) {
        load_slot(was);                     /* as it was; the store may be half moved */
        return st;
    }
    gm_cfg.preset_count = (int32_t)(n - 1u);
    /* The active preset keeps playing, one slot up if it moved; the deleted
     * one hands over to the preset now in its slot (or the one before). */
    uint32_t next = was == slot ? (slot <= n - 1u ? slot : n - 1u) : was > slot ? was - 1u : was;
    do_select(next, 1);
    return write_state();
}

/* Move one preset to slot `to`: the ones between shift one slot towards
 * `from`. The moving preset is held as its values that differ from the param
 * defaults (and its name) while the others are moved, each by loading it and
 * storing it as its new slot's record. */
int presets_move(uint32_t from, uint32_t to)
{
    uint32_t n = used();
    if (from < 1u || from > n || to < 1u || to > n)
        return GSX_BAD_PARAM;
    if (from == to)
        return GSX_OK;
    uint32_t was = cur;
    gm_release_all();
    cs_entry_t held[MAX_ENTRIES];        /* not rec[]: write_values() builds there meanwhile */
    uint32_t n_held = 0;
    char name[PRESET_NAME_MAX + 1];
    cs_slot_info_t info;
    const char *nm = moved_name(from, to, &info);
    uint32_t i = 0;
    for (; nm && nm[i] && i < PRESET_NAME_MAX; i++)
        name[i] = nm[i];
    name[i] = 0;
    load_slot(from);
    for (uint32_t k = 0; k < app_params.param_count; k++) {
        const param_def_t *p = &app_params.params[k];
        if (is_preset(p) && storable(p))
            for (uint32_t j = 0; j < p->count; j++)
                if (p->values[j] != params_default_at(p, j))
                    put(held, &n_held, p->id, j, p->values[j]);
    }
    int st = GSX_OK;
    int step = from < to ? 1 : -1;
    for (uint32_t s = from; s != to && st == GSX_OK; s += (uint32_t)step) {
        uint32_t src = s + (uint32_t)step;
        const char *mn = moved_name(src, s, &info);
        load_slot(src);
        st = write_values(s, mn, 0);
    }
    if (st == GSX_OK) {
        /* The held preset into `to`: defaults, then its values. */
        for (uint32_t k = 0; k < app_params.param_count; k++) {
            const param_def_t *p = &app_params.params[k];
            if (is_preset(p))
                for (uint32_t j = 0; j < p->count; j++)
                    p->values[j] = params_default_at(p, j);
        }
        for (uint32_t k = 0; k < n_held; k++)
            params_set(held[k].id, held[k].index, held[k].value, 0);
        st = write_values(to, name[0] ? name : 0, 0);
    }
    /* The active preset keeps playing, wherever it is now. */
    uint32_t next = was;
    if (was == from)
        next = to;
    else if (from < to && was > from && was <= to)
        next = was - 1u;
    else if (from > to && was >= to && was < from)
        next = was + 1u;
    if (st != GSX_OK) {
        load_slot(was);                     /* the store may be half moved */
        return st;
    }
    load_slot(next);
    preset_changed();
    if (next != was)
        app_param_changed(P_PRESET_ACTIVE, 0);
    return write_state();
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
                           (slot <= used() ? PRESET_INFO_IN_USE : 0) |
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

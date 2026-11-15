"""key_matrix: the key matrix (`key_matrix:`), a control: key_matrix.c scans it and plays the keys, this file
validates its YAML and writes its C (README.md here). The builder (gemifi_device.py) calls validate() with
the other controls, and c_defines(), c_param_defaults() and c_tables() where gm_device_config.h / .c have
them.
"""
from components.api import NO_PIN, Hub, LList
from components.hub import c_comment

MATRIX_KEYS = ["type", "rows", "columns", "contacts", "key_map", "keys", "first_note", "settle", "velocity", "daw"]
VELOCITY_KEYS = ["fast", "slow"]
RENAMED_MATRIX = {"kind": "renamed: use `type:` (switch | velocity)", "cols": "renamed: use `columns:`",
                  "settle_us": "renamed: use `settle:` with a unit, e.g. `settle: 1us`"}
RENAMED_VELOCITY = {"fast_ms": "renamed: use `fast:` with a unit, e.g. `fast: 1.6ms`",
                    "slow_ms": "renamed: use `slow:` with a unit, e.g. `slow: 120ms`"}


def in_device(dev):
    """Built when the device has a key matrix (GM_HAS_MATRIX 1)."""
    return dev.matrix is not None


# key_matrix rows and columns: the core scans at most 32 rows (GM_MATRIX_ROWS) and keeps each row's
# columns in a uint32_t (core/src/controller.c), until it is widened for larger devices.
MATRIX_MAX = 32


def validate(v, dev, data):
    """key_matrix: (at most one) -> dev.matrix."""
    if data.get("key_matrix") is None:
        return
    p = "key_matrix"
    d = v.mapping(data["key_matrix"], p, data.line_of("key_matrix"), MATRIX_KEYS, renamed=RENAMED_MATRIX)
    if d is None:
        return
    kind = d.get("type")
    if kind not in ("switch", "velocity"):
        if not (kind is None and "kind" in d):
            v.err(f"{p}.type", d.line_of("type"), f"{'is required' if kind is None else repr(kind) + ' is unknown'} "
                                                   "(switch: one contact per key, velocity: two contacts per key)")
        kind = "switch"
    why = ("the matrix is scanned from the timer interrupt, so only MCU pins, sn74hc595 or decoder rows and "
           "sn74hc165 columns")
    pins = {"rows": v.matrix_rows(d, p, why, {"sn74hc595"} | v.hub_keys("row"))}
    lst = d.get("columns")
    if not isinstance(lst, list) or not lst:
        if "cols" not in d:
            v.err(f"{p}.columns", d.line_of("columns") if "columns" in d else d.line, "needs a list of pins")
        lst = LList()
    if len(lst) > MATRIX_MAX:
        v.err(f"{p}.columns", d.line_of("columns"), f"{len(lst)} columns: at most {MATRIX_MAX} (the core "
                                                    "keeps a row's columns in 32 bits)")
    # MCU columns are a "column" line, so a switch_matrix can share them (allow_other_uses)
    pins["columns"] = [v.pin(x, f"{p}.columns[{i}]", lst.line_of(i), "in", hubs={"sn74hc165"}, why=why,
                             line_kind=None if v.names_hub(x) else "column", owner="key_matrix") or NO_PIN
                       for i, x in enumerate(lst)]
    nr, nc = len(pins["rows"]), len(pins["columns"])
    if nr > MATRIX_MAX:
        v.err(f"{p}.rows", d.line_of("rows"), f"{nr} rows: at most {MATRIX_MAX} (the core scans up to "
                                              f"{MATRIX_MAX} rows; {MATRIX_MAX} x {MATRIX_MAX} is enough for 88 keys)")
    contacts = d.get("contacts", "columns")
    if contacts not in ("columns", "rows"):
        v.err(f"{p}.contacts", d.line_of("contacts"), f"{contacts!r} is unknown (columns: a key's two contacts "
                                                      "are columns 2p, 2p+1; rows: rows 2p, 2p+1)")
        contacts = "columns"
    if "contacts" in d and kind != "velocity":
        v.err(f"{p}.contacts", d.line_of("contacts"), "only for type: velocity (two contacts per key)")
        contacts = "columns"
    by_rows = kind == "velocity" and contacts == "rows"
    if kind == "switch":
        lrows, lcols = nr, nc
    elif by_rows:
        lrows, lcols = nr // 2, nc
        if nr % 2:
            v.err(f"{p}.rows", d.line_of("rows"), f"contacts: rows needs an even number of rows "
                                                  f"(two contacts per key), got {nr}")
    else:
        lrows, lcols = nr, nc // 2
        if nc % 2:
            v.err(f"{p}.columns", d.line_of("columns"), f"type velocity needs an even number of columns "
                                                        f"(two contacts per key), got {nc}")
    cap = lrows * lcols
    keys = v.integer(d, "keys", p, 1, 128, default=min(cap, 128) or 1)
    first = v.integer(d, "first_note", p, 0, 127, default=48)
    if keys is not None and nr and nc and keys > cap and d.get("key_map") is None:
        v.err(f"{p}.keys", d.line_of("keys"), f"{keys} keys don't fit a {nr} x {nc} {kind} matrix "
                                              f"(at most {cap})")
    if keys is not None and first is not None and first + keys > 128:
        v.err(f"{p}.keys", d.line_of("keys"), f"first_note {first} + {keys} keys goes past note 127")
    settle = v.period(d, "settle", p, 0, 255, default=1, example="1us")
    vel_fast, vel_slow = validate_velocity(v, d, p, kind)
    key_map = validate_key_map(v, d, p, lrows, lcols, keys or 0, by_rows, kind)
    dev.matrix = Hub(kind=kind, rows=pins["rows"], cols=pins["columns"], keys=keys or 0,
                     first_note=first or 0, settle_us=settle, vel_fast=vel_fast, vel_slow=vel_slow,
                     daw=v.daw_role(dev, d, p), contacts=contacts, key_map=key_map)


def validate_key_map(v, d, p, lrows, lcols, keys, by_rows, kind):
    """key_map: one list per matrix row (per row pair with contacts: rows), one key number per column
    (per column pair with contacts: columns), or null where there is no key. -> flat list, row-major,
    255 = none; or None when not given."""
    if d.get("key_map") is None:
        return None
    p = f"{p}.key_map"
    km = v.seq(d["key_map"], p, d.line_of("key_map"))
    what_r = "row pair" if by_rows else "row"
    what_c = "column pair" if kind == "velocity" and not by_rows else "column"
    if len(km) != lrows:
        v.err(p, d.line_of("key_map"), f"needs one list per {what_r}: {lrows}, got {len(km)}")
    flat, where = [], {}
    for r in range(lrows):
        row = km[r] if r < len(km) else LList()
        rp = f"{p}[{r}]"
        if not isinstance(row, list):
            v.err(rp, km.line_of(r), f"must be a list of {lcols} key numbers (null = no key)")
            row = LList()
        elif len(row) != lcols:
            v.err(rp, km.line_of(r), f"needs {lcols} entries, one per {what_c} (null = no key), got {len(row)}")
        for c in range(lcols):
            x = row[c] if c < len(row) else None
            if x is None:
                flat.append(255)
                continue
            if isinstance(x, bool) or not isinstance(x, int) or not 0 <= x < keys:
                v.err(f"{rp}[{c}]", row.line_of(c) if isinstance(row, LList) else km.line_of(r),
                      f"must be a key number 0..{keys - 1} or null (got {x!r})")
                flat.append(255)
                continue
            if x in where:
                v.err(f"{rp}[{c}]", row.line_of(c), f"key {x} is already at key_map{where[x]}")
            where.setdefault(x, f"[{r}][{c}]")
            flat.append(x)
    missing = [k for k in range(keys) if k not in where]
    if missing and len(km) == lrows:
        v.warn(p, d.line_of("key_map"), f"keys not in the map never play: {', '.join(map(str, missing[:8]))}"
                                        f"{' ...' if len(missing) > 8 else ''}")
    return flat


# Default key times for velocity 127 and 1, in 0.1 ms (the velocity.fast /
# velocity.slow params; their ranges are in core/src/settings.c).
VEL_FAST_RANGE = (1, 500)
VEL_SLOW_RANGE = (20, 2000)


def validate_velocity(v, d, path, kind):
    """key_matrix.velocity: { fast, slow } (time periods) -> (fast, slow) in 0.1 ms, None where not given."""
    p = f"{path}.velocity"
    vd = v.mapping(d.get("velocity"), p, d.line_of("velocity"), VELOCITY_KEYS,
                   renamed=RENAMED_VELOCITY) if "velocity" in d else None
    if vd is None:
        return None, None
    if kind != "velocity":
        v.err(p, d.line_of("velocity"), "only for type: velocity (a switch matrix has no key times)")
        return None, None
    fast = v.period(vd, "fast", p, VEL_FAST_RANGE[0] * 100, VEL_FAST_RANGE[1] * 100, step_us=100,
                    example="1.6ms")
    slow = v.period(vd, "slow", p, VEL_SLOW_RANGE[0] * 100, VEL_SLOW_RANGE[1] * 100, step_us=100,
                    example="120ms")
    if (fast or 12) >= (slow or 1200):
        v.err(p, d.line_of("velocity"), f"fast ({(fast or 12) / 10:g}ms) must be shorter than slow "
                                        f"({(slow or 1200) / 10:g}ms)")
        return None, None
    return fast, slow


MATRIX_FAST_COLS = 16   # controller.c FAST_COLS: columns read as port runs
MATRIX_MAX_ROWS = 32    # controller.c: rows scanned


def matrix_col_runs(dev, m):
    """How many col_run_t entries controller.c's gm_init makes for the key matrix: runs of columns
    c, c+1, ... on consecutive bits of one input register. NUC1xx and STM32F1 have a register per
    port of 16 pins (pin = port * 16 + bit); elsewhere, or for hub pins, assume one per column."""
    cols = m.cols[:MATRIX_FAST_COLS]
    if dev.platform.is_esp:
        return len(cols)
    runs, last = 0, None            # last: (port, next bit, next column) that would extend the run
    for c, pn in enumerate(cols):
        if pn.hub != 0:
            continue                # no register: the scan reads pin by pin
        port, bit = divmod(pn.num, 16)
        if last != (port, bit, c):
            runs += 1
        last = (port, bit + 1, c + 1)
    return runs


def c_defines(dev):
    """The key-matrix scan's table sizes in controller.c (core/README.md "Generated code")."""
    m = dev.matrix
    if not m:
        return []
    dec = any(0 < pn.hub <= len(dev.hubs) and dev.hubs[pn.hub - 1].type == "decoder" for pn in m.rows)
    return [f"#define GM_MATRIX_ROWS {min(len(m.rows), MATRIX_MAX_ROWS)} /* key-matrix rows scanned (at most 32) */",
            f"#define GM_MATRIX_COL_RUNS {matrix_col_runs(dev, m)} /* runs of adjacent column bits on one port */",
            f"#define GM_MATRIX_DECODER {1 if dec else 0} /* rows on a decoder: one-store row selects */"]


def c_param_defaults(dev):
    """The velocity.fast / velocity.slow defaults its velocity: gives (core/src/settings.c)."""
    m = dev.matrix
    h = []
    if m and m.vel_fast is not None:
        h.append(f"#define GM_VEL_FAST_DEFAULT {m.vel_fast} /* {m.vel_fast / 10:g} ms */")
    if m and m.vel_slow is not None:
        h.append(f"#define GM_VEL_SLOW_DEFAULT {m.vel_slow} /* {m.vel_slow / 10:g} ms */")
    return h


def c_tables(dev):
    """The key matrix's rows, columns, key map and gm_matrix_t (gm_device.h); nothing without one."""
    m = dev.matrix
    if not m:
        return []
    c = []
    for nm, lst in (("rows", m.rows), ("cols", m.cols)):
        c.append(f"static const gm_pin_t matrix_{nm}[{len(lst)}] = {{")
        c += [f"    {x.c()}, /* {c_comment(x.desc)} */" for x in lst]
        c += ["};"]
    kind = "GM_MATRIX_KIND_VELOCITY" if m.kind == "velocity" else "GM_MATRIX_KIND_SWITCH"
    if m.key_map:
        c.append(f"static const uint8_t matrix_key_map[{len(m.key_map)}] = {{")
        per = max(1, len(m.cols) if m.contacts == "rows" or m.kind == "switch" else len(m.cols) // 2)
        for i in range(0, len(m.key_map), per):
            c.append("    " + ", ".join(str(x) for x in m.key_map[i:i + per]) + ",")
        c += ["};"]
    contacts = "GM_MATRIX_CONTACTS_ROWS" if m.contacts == "rows" else "GM_MATRIX_CONTACTS_COLS"
    c += ["static const gm_matrix_t matrix = {",
          f"    .rows = matrix_rows, .n_rows = {len(m.rows)},",
          f"    .cols = matrix_cols, .n_cols = {len(m.cols)},",
          f"    .kind = {kind}, .n_keys = {m.keys}, .first_note = {m.first_note}, "
          f".settle_us = {m.settle_us},",
          f"    .contacts = {contacts}, .key_map = {'matrix_key_map' if m.key_map else '0'},",
          "};", ""]
    return c

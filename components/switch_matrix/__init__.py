"""switch_matrix: a matrix of switches scanned from the scan interrupt, a hub type on the hub base
(components/hub). The YAML and the C: README.md here. Its rows and columns are resolved after every hub
is known (gemifi_device.py resolve_switch_matrices: rows may name a decoder declared further down).
"""
from components.hub import HubType, c_comment


class SwitchMatrix(HubType):
    """A switch matrix scanned from the scan interrupt: inputs only, pin n = row n / columns,
    column n % columns."""
    key, enum, num = "switch_matrix", "GM_HUB_SWMATRIX", 8
    keys = ("id", "rows", "columns", "settle", "rows_per_scan")
    roles = frozenset({"in"})
    # its config (.sm) and state (u.sm) are the core's: the scan reads them (vpin_scan_matrices ...)
    c_ops = "gm_switch_matrix_ops"

    def validate(self, v, dev, hub, h, p, line, ctl):
        # Rows may name a decoder declared further down: pins are resolved after all hubs
        # (resolve_switch_matrices). The size is known now, for pin numbers on this hub.
        rows, cols = h.get("rows"), h.get("columns")
        nr = rows.get("count", 0) if isinstance(rows, dict) else len(rows) if isinstance(rows, list) else 0
        nc = len(cols) if isinstance(cols, list) else 0
        hub.n_pins = max(nr if isinstance(nr, int) else 0, 0) * nc
        if hub.n_pins > 32:
            v.err(p, line, f"{nr} rows x {nc} columns = {hub.n_pins} switches: at most 32 per switch_matrix "
                           "(split it into two)")
        hub.settle_us = v.period(h, "settle", p, 0, 255, default=1, example="2us")
        # rows read per scan, in turn (0 = all): spreads a big panel over several scans
        hub.rows_per_scan = v.integer(h, "rows_per_scan", p, 1, 32, default=0)
        if hub.rows_per_scan and isinstance(nr, int) and nr and hub.rows_per_scan >= nr:
            v.warn(f"{p}.rows_per_scan", h.line_of("rows_per_scan"), f"{hub.rows_per_scan} >= its {nr} rows: "
                                                                      "every row is read on every scan anyway")

    def c_tables(self, hub):
        if not hub.rows or not hub.cols:
            return []
        c = []
        for nm, lst in (("rows", hub.rows), ("cols", hub.cols)):
            c.append(f"static const gm_pin_t hub{hub.index + 1}_{nm}[{len(lst)}] = {{")
            c += [f"    {x.c()}, /* {c_comment(x.desc)} */" for x in lst]
            c += ["};"]
        return c

    def c_fields(self, hub):
        if not hub.rows or not hub.cols:
            return [f"        .sm = {{ .settle_us = {hub.settle_us} }},"]
        n = hub.index + 1
        return [f"        .sm = {{ .rows = hub{n}_rows, .cols = hub{n}_cols, .n_rows = {len(hub.rows)}, "
                f".n_cols = {len(hub.cols)},",
                f"                .settle_us = {hub.settle_us}, .rows_per_scan = {hub.rows_per_scan or 0} }},"]


HUB_TYPES = (SwitchMatrix(),)

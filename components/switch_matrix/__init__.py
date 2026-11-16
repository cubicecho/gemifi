"""switch_matrix: a matrix of switches scanned from the scan interrupt, a hub type on the hub base
(components/hub). The YAML and the C: README.md here. Its rows and columns are resolved after every hub
is known (final_validate: rows may name a decoder declared further down).
"""
from components.hub import HubType, c_comment
from components.keys import Id, IdRef, Int, Key, Keys, List, Map, OneOf, Period, Pin

# A matrix's rows and columns (gemifi_device.py matrix_rows / matrix_columns check them).
ROWS_KEY = Key("rows", OneOf(List(Pin()), Map(Keys(
    Key("decoder", IdRef("decoder"), required=True, doc="The decoder hub whose outputs are the rows."),
    Key("first", Int(0, 255), default=0, doc="Its output that is the first row."),
    Key("count", Int(1, 255), required=True, doc="The rows: its outputs from `first` on.")))),
    required=True, see='core/README.md "Matrices behind decoders"',
    doc="The row pins, driven in turn: MCU pins or decoder outputs, or a run of a decoder's outputs.")
COLUMNS_KEY = Key("columns", List(Pin()), required=True,
                  doc="The column pins, read for each row: MCU inputs (at most 32).")

SPEC = Keys(
    Key("id", Id(), required=True, doc="What its switches name: `{ switch_matrix: <id>, number: n }`."),
    ROWS_KEY,
    COLUMNS_KEY,
    Key("settle", Period(0, 255, example="2us"), shown_default="`1us`",
        doc="How long it waits after driving a row before it reads the columns."),
    Key("rows_per_scan", Int(1, 32), shown_default="all rows",
        doc="The rows read per scan, in turn, to spread a big panel over several scans."),
)


class SwitchMatrix(HubType):
    """A switch matrix scanned from the scan interrupt: inputs only, pin n = row n / columns,
    column n % columns."""
    key, enum, num = "switch_matrix", "GM_HUB_SWMATRIX", 8
    spec = SPEC
    keys = tuple(SPEC.names)
    roles = frozenset({"in"})
    # its config (.sm) and state (u.sm) are the core's: the scan reads them (vpin_scan_matrices ...)
    c_ops = "gm_switch_matrix_ops"

    def validate(self, v, dev, hub, h, p, line, ctl):
        # Rows may name a decoder declared further down: pins are resolved after all hubs
        # (final_validate). The size is known now, for pin numbers on this hub.
        rows, cols = h.get("rows"), h.get("columns")
        nr = rows.get("count", 0) if isinstance(rows, dict) else len(rows) if isinstance(rows, list) else 0
        nc = len(cols) if isinstance(cols, list) else 0
        hub.n_pins = max(nr if isinstance(nr, int) else 0, 0) * nc
        if hub.n_pins > 32:
            v.err(p, line, f"{nr} rows x {nc} columns = {hub.n_pins} switches: at most 32 per switch_matrix "
                           "(split it into two)")
        hub.settle_us = SPEC.get(v, h, "settle", p, default=1)
        hub.rows_per_scan = SPEC.get(v, h, "rows_per_scan", p, default=0)     # 0: all
        if hub.rows_per_scan and isinstance(nr, int) and nr and hub.rows_per_scan >= nr:
            v.warn(f"{p}.rows_per_scan", h.line_of("rows_per_scan"), f"{hub.rows_per_scan} >= its {nr} rows: "
                                                                      "every row is read on every scan anyway")

    def final_validate(self, v, dev, hubs):
        for hub in hubs:
            if isinstance(hub.raw, dict):
                hub.rows = v.matrix_rows(hub.raw, hub.path, "switch_matrix rows are MCU pins or decoder outputs")
                hub.cols = v.matrix_columns(hub.raw, hub.path, owner=f"hub{hub.index}")

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

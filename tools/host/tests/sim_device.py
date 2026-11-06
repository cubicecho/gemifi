"""A simulated gemifi bootloader/app that follows protocol/sysex.md, for host-tool tests.
It enforces the upload rules strictly (order, page 0 last, page boundaries, no double programming).
Given a `schema` (gemifi_proto.Schema) it also acts as an app with config (sysex.md 4.3).
With `ab=True` it is an A/B app (ESP32/ESP8266 OTA, sysex.md 4.1 "Who takes uploads"): role 0, caps bit 0, a
non-zero max_chunk, FW_* taken by the app in plain order from 0, and REBOOT(0) after a good FW_END boots the
new slot (its fw_version then shows in HELLO).
With `presets` (a list of {"name", "vals": {(id, index): value}, "program"}; slot 1 first, its "vals" must be
empty) it also simulates presets (sysex.md 4.5, research/core/presets-design.md §10.3/§10.4): params with the
PRESET flag live per slot, preset.active (17) switches, preset.dirty (19) follows host SET_PARAMs, COMMIT and
PRESET_SAVE store the per-slot diff against the slot's YAML values, PRESET_RESET / PRESET_NAME, PRESET_CHANGED
in `outbox`. A schema with preset.count (33) is a firmware with PRESET_DELETE (sysex.md 4.5 "Presets in use"):
slots past the count are free (PRESET_INFO bit 3 clear, not selectable), PRESET_NAME / PRESET_SAVE on a free
slot put it in use, PRESET_DELETE moves the later presets up; without it, an older firmware (every slot in use,
PRESET_DELETE answered BAD_CMD). `next_revision` is the presets revision the A/B image booted by REBOOT(0) reports (a firmware
update that changes the YAML presets); stored edits survive it.
With `components` (a list of gemifi_proto.Component, or True for FEEDBACK_COMPONENTS) it is an app with host
feedback (sysex.md 4.6): HELLO caps bit 6, GET_COMPONENT per kind (BAD_PARAM past the end, `count` filled in),
and FB_* notifications (0x20-0x3F) are never answered: each one goes into `notifications` as
(gemifi_proto.Notification, seq), a malformed one into `dropped`; `send_booted()` queues the BOOTED a rebooted
device sends (sysex.md 4.6.3). Without `components` it behaves like older firmware: BAD_CMD as cmd | 0x40, which
lands in the unsolicited range."""
import dataclasses
import os
import zlib
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gemifi_image as gi  # noqa: E402
import gemifi_proto as gp  # noqa: E402

PAGE = gp.PAGE_SIZE

C = gp.Component
# A small feedback-capable app: 2 buttons with LEDs, a key matrix, 1 fader, 1 encoder with a 5-LED ring and
# a centre LED, 3 LEDs (one bicolour), no display. Index fields are set from the list order.
FEEDBACK_COMPONENTS = [
    C(gp.KIND_BUTTON, 0, 0, "play", "Play", "play", led=0),
    C(gp.KIND_BUTTON, 0, 0, "rec", "Record", "record", led=1),
    C(gp.KIND_KEYS, 0, 0, "keys", "Keys", "", keys=32, first_note=48),
    C(gp.KIND_ANALOG, 0, 0, "fader1", "Fader 1", "track_volume", takeover=gp.TAKEOVER_PICKUP),
    C(gp.KIND_ENCODER, 0, 0, "enc1", "Encoder 1", "device_param_1", ring=5, center=2, mode=gp.ENC_ABSOLUTE),
    C(gp.KIND_LED, 0, 0, "play_led", "Play LED", "", colours=1, owner=gp.OWNER_HOST_THEN_LOCAL),
    C(gp.KIND_LED, 0, 0, "rec_led", "Record LED", "", colours=2, owner=gp.OWNER_LOCAL),
    C(gp.KIND_LED, 0, 0, "enc1_centre", "Encoder 1 centre", "", colours=1, owner=gp.OWNER_HOST),
]


class SimDevice:
    def __init__(self, role=gp.ROLE_BOOT, board_id=1, max_chunk=256, drop_replies=(), drop_requests=(),
                 busy_on=(), schema=None, bulk=True, ab=False, fw_version=0x00010000, presets=None, slots=None,
                 revision=0, next_revision=None, components=None, flash_size=gp.APROM_SIZE, extra_caps=0,
                 layout=None, raw=None):
        self.ab = ab
        self.flash_size, self.extra_caps = flash_size, extra_caps   # a Code 61: its app area, caps bit 7
        if ab:
            role = gp.ROLE_APP
        self.role, self.board_id, self.max_chunk = role, board_id, max_chunk
        self.fw_version = fw_version
        self.slot = None          # A/B: the image in the running slot after a reboot into a new one
        self.pending = None       # A/B: (image, version) checked by FW_END, booted by the next REBOOT(0)
        self.reboots = []         # every REBOOT mode received
        self.ab_chunks = []       # A/B: (offset, length) of every accepted FW_DATA
        self.schema, self.bulk = schema, bulk
        self.schema_bytes = gp.build_schema(schema) if schema else b""
        self.values = self.defaults()
        self.stored = None        # committed profile
        self.commits = 0
        self.flash = bytearray(b"\xff" * flash_size)
        self.program_count = [0] * (flash_size // 4)
        self.state = "idle"
        self.last = None          # (frame bytes, reply bytes) of the last completed request
        self.n_req = 0
        self.n_reply = 0
        self.drop_replies, self.drop_requests, self.busy_on = set(drop_replies), set(drop_requests), set(busy_on)
        self.log = []
        self.reboot_to = None
        self.outbox = []          # unsolicited frames (PRESET_CHANGED, BOOTED) the port delivers after the next reply
        self.useq = 0
        self.presets = None
        if presets is not None:
            self.setup_presets(presets, slots, revision)
        self.next_revision = next_revision
        if components is True:
            components = FEEDBACK_COMPONENTS
        self.components = None    # kind -> [Component], index = position
        if components is not None:
            self.components = {}
            for c in components:
                lst = self.components.setdefault(c.kind, [])
                lst.append(dataclasses.replace(c, index=len(lst)))
        self.layout = layout      # Layout (sysex.md 4.7): GET_LAYOUT and caps bit 8
        self.raw = raw            # [RawMsg] (sysex.md 4.8): MONITOR and caps bit 9; queued after each MONITOR
        self.monitor_kinds = []   # the kinds of every MONITOR received
        self.notifications = []   # (Notification, seq) of every FB_* received, in order
        self.dropped = []         # malformed or unknown notifications (raw frames), dropped without a reply

    # -- transport side --
    def handle(self, msg):
        """Returns reply bytes or None."""
        self.n_req += 1
        if self.n_req in self.drop_requests:
            return None
        try:
            f = gp.parse_frame(msg)
        except gp.ProtocolError:
            return None
        if f.is_notification and self.components is not None:
            try:                                     # sysex.md 4.6.2: never answered, not even with an error
                self.notifications.append((gp.parse_notification(f.cmd, f.payload), f.seq))
            except gp.ProtocolError:
                self.dropped.append(bytes(msg))
            return None
        if self.last and self.last[0] == bytes(msg):
            reply = self.last[1]                     # duplicate seq: reply again, don't redo the work
        else:
            if self.n_req in self.busy_on:
                return gp.build_frame(f.cmd | gp.REPLY, f.seq, bytes([gp.BUSY]))
            payload = self.dispatch(f)
            reply = gp.build_frame(f.cmd | gp.REPLY, f.seq, payload)
            self.last = (bytes(msg), reply)
        self.n_reply += 1
        if self.n_reply in self.drop_replies:
            return None
        return reply

    def hello(self):
        app_caps = (gp.CAP_LOG | gp.CAP_EVENT | (gp.CAP_CONFIG if self.schema else 0) | (gp.CAP_FW if self.ab else 0)
                    | (gp.CAP_PRESETS if self.presets is not None else 0)
                    | (gp.CAP_FEEDBACK if self.components is not None else 0) | self.extra_caps
                    | (gp.CAP_LAYOUT if self.layout is not None else 0) | (gp.CAP_MONITOR if self.raw is not None else 0))
        return gp.Hello(self.role, self.board_id, self.fw_version, 1,
                        self.max_chunk if self.role == gp.ROLE_BOOT or self.ab else 0,
                        gp.CAP_FW if self.role == gp.ROLE_BOOT else app_caps, bytes(range(12)),
                        None if self.role == gp.ROLE_BOOT else "sim-device")

    def send_booted(self):
        """A reboot or replug as a DAW script sees it: an app with caps bit 6 queues BOOTED (sysex.md 4.6.3) for
        the port to deliver. Returns whether it did (older firmware and the bootloader don't)."""
        h = self.hello()
        if self.role != gp.ROLE_APP or (h.caps & gp.CAP_FEEDBACK) == 0:
            return False
        self.useq = (self.useq + 1) & 0x7F
        self.outbox.append(gp.build_frame(gp.BOOTED, self.useq, gp.build_booted(h)))
        return True

    # -- protocol --
    def dispatch(self, f):
        self.log.append(gp.CMD_NAMES.get(f.cmd, hex(f.cmd)))
        p = f.payload
        if f.cmd == gp.HELLO:
            return gp.build_hello_reply(self.hello())
        if f.cmd in (gp.GET_SCHEMA, gp.GET_PARAM, gp.SET_PARAM, gp.GET_PARAMS, gp.SET_PARAMS, gp.COMMIT,
                     gp.FACTORY_RESET):
            if self.role != gp.ROLE_APP or not self.schema:
                return bytes([gp.BAD_CMD])
            return self.config(f.cmd, p)
        if f.cmd in (gp.PRESET_INFO, gp.PRESET_SAVE, gp.PRESET_RESET, gp.PRESET_NAME, gp.PRESET_DELETE,
                     gp.PRESET_MOVE):
            if self.role != gp.ROLE_APP or self.presets is None or (
                    f.cmd in (gp.PRESET_DELETE, gp.PRESET_MOVE) and not self.has_count):
                return bytes([gp.BAD_CMD])
            return self.preset_cmd(f.cmd, p)
        if f.cmd == gp.GET_LAYOUT:
            if self.role != gp.ROLE_APP or self.layout is None:
                return bytes([gp.BAD_CMD])
            if len(p) != 6:
                return bytes([gp.BAD_LEN])
            return gp.build_layout_reply(gp.build_layout(self.layout), gp.dec_u28(p[0:4]), gp.dec_u14(p[4:6]))
        if f.cmd == gp.MONITOR:
            if self.role != gp.ROLE_APP or self.raw is None:
                return bytes([gp.BAD_CMD])
            if len(p) != 1:
                return bytes([gp.BAD_LEN])
            have = 0
            for b, k in gp.MON_KIND_RAW.items():
                if any(m.kind == k for m in self.raw):
                    have |= b
            self.monitor_kinds.append(p[0])
            for m in self.raw:
                if p[0] & have & next(b for b, k in gp.MON_KIND_RAW.items() if k == m.kind):
                    self.useq = (self.useq + 1) & 0x7F
                    self.outbox.append(gp.build_frame(gp.RAW, self.useq, gp.build_raw(m)))
            return gp.build_monitor_reply(p[0] & have)
        if f.cmd == gp.GET_COMPONENT:
            if self.role != gp.ROLE_APP or self.components is None:
                return bytes([gp.BAD_CMD])
            if len(p) != 3:
                return bytes([gp.BAD_LEN])
            lst = self.components.get(p[0], [])
            idx = gp.dec_u14(p[1:3])
            if idx >= len(lst):
                return bytes([gp.BAD_PARAM])
            return gp.build_component_reply(dataclasses.replace(lst[idx], count=len(lst)))
        if f.cmd == gp.REBOOT:
            self.reboot_to = p[0]
            self.reboots.append(p[0])
            if self.ab and p[0] == gp.REBOOT_APP and self.pending:
                self.slot, self.fw_version = self.pending
                self.pending = None
                if self.presets is not None and self.next_revision is not None:
                    self.revision = self.next_revision
            if self.presets is not None and p[0] == gp.REBOOT_APP:
                self.boot()
            self.state = "idle"
            return bytes([gp.OK])
        if self.ab and f.cmd in (gp.FW_BEGIN, gp.FW_DATA, gp.FW_END, gp.FW_ABORT):
            return self.ab_fw(f.cmd, p)
        if f.cmd in (gp.FW_BEGIN, gp.FW_DATA, gp.FW_END, gp.FW_ABORT) and self.role != gp.ROLE_BOOT:
            return bytes([gp.BAD_STATE])
        if f.cmd == gp.FW_BEGIN:
            if len(p) != 17:
                return bytes([gp.BAD_LEN])
            size, crc, board, ver = gp.dec_u32(p[0:5]), gp.dec_u32(p[5:10]), gp.dec_u14(p[10:12]), gp.dec_u32(p[12:17])
            if board != self.board_id:
                return bytes([gp.BAD_BOARD])
            if size == 0 or size % 4 or size > self.flash_size:
                return bytes([gp.BAD_ADDR])
            self.size, self.crc, self.ver = size, crc, ver
            self.erase(0)
            self.expect = PAGE if size > PAGE else 0
            self.state = "loading"
            return bytes([gp.OK]) + gp.u14(self.max_chunk)
        if f.cmd == gp.FW_DATA:
            if self.state != "loading":
                return bytes([gp.BAD_STATE])
            off = gp.dec_u28(p[0:4])
            data = gp.unpack7(p[4:])
            n = len(data)
            if n == 0 or n % 4 or n > self.max_chunk:
                return bytes([gp.BAD_LEN])
            if off != self.expect or off + n > self.size or off // PAGE != (off + n - 1) // PAGE:
                return bytes([gp.BAD_ADDR])
            if off % PAGE == 0 and off != 0:
                self.erase(off)
            for i in range(0, n, 4):
                w = (off + i) // 4
                if self.flash[off + i:off + i + 4] != b"\xff\xff\xff\xff":
                    return bytes([gp.FLASH_ERR])
                self.program_count[w] += 1
            self.flash[off:off + n] = data
            nxt = off + n
            if self.expect >= PAGE:
                self.expect = nxt if nxt < self.size else 0
            else:
                self.expect = nxt if nxt < min(PAGE, self.size) else None
            return bytes([gp.OK]) + gp.u28(off)
        if f.cmd == gp.FW_END:
            if self.state != "loading" or self.expect is not None:
                return bytes([gp.BAD_STATE])
            img = bytes(self.flash[:self.size])
            self.state = "idle"
            try:
                h = gi.check_image(img, self.board_id)
            except gi.ImageError:
                return bytes([gp.BAD_IMAGE])
            if h.crc32 != self.crc or gi.image_crc(img, gi.find_hdr_offset(img, self.board_id)) != self.crc:
                return bytes([gp.BAD_IMAGE])
            return bytes([gp.OK])
        if f.cmd == gp.FW_ABORT:
            self.state = "idle"
            return bytes([gp.OK])
        return bytes([gp.BAD_CMD])

    def ab_fw(self, cmd, p):
        """sysex.md 4.2 "A/B devices": in order from 0, any multiple of 4 up to max_chunk, CRC over the file."""
        if cmd == gp.FW_BEGIN:
            if len(p) != 17:
                return bytes([gp.BAD_LEN])
            size, crc, board, ver = gp.dec_u32(p[0:5]), gp.dec_u32(p[5:10]), gp.dec_u14(p[10:12]), gp.dec_u32(p[12:17])
            if board != self.board_id:
                return bytes([gp.BAD_BOARD])
            if size == 0 or size % 4:
                return bytes([gp.BAD_ADDR])
            self.size, self.crc, self.ver = size, crc, ver
            self.inactive = bytearray()
            self.pending = None
            self.ab_chunks = []
            self.state = "loading"
            return bytes([gp.OK]) + gp.u14(self.max_chunk)
        if cmd == gp.FW_DATA:
            if self.state != "loading":
                return bytes([gp.BAD_STATE])
            off = gp.dec_u28(p[0:4])
            data = gp.unpack7(p[4:])
            n = len(data)
            if n == 0 or n % 4 or n > self.max_chunk:
                return bytes([gp.BAD_LEN])
            if off != len(self.inactive) or off + n > self.size:
                return bytes([gp.BAD_ADDR])
            self.inactive += data
            self.ab_chunks.append((off, n))
            return bytes([gp.OK]) + gp.u28(off)
        if cmd == gp.FW_END:
            if self.state != "loading":
                return bytes([gp.BAD_STATE])
            self.state = "idle"
            if len(self.inactive) != self.size or zlib.crc32(bytes(self.inactive)) != self.crc:
                return bytes([gp.BAD_IMAGE])
            self.pending = (bytes(self.inactive), self.ver)
            return bytes([gp.OK])
        self.state = "idle"        # FW_ABORT
        return bytes([gp.OK])

    def defaults(self):
        if not self.schema:
            return {}
        return {(q.id, i): q.default(i) for q in self.schema.params for i in range(q.count)}

    def check(self, pid, index, n=1, write=False):
        q = self.schema.by_id(pid)
        if q is None or index + n > q.count or (write and q.read_only):
            return None
        return q

    def config(self, cmd, p):
        if cmd == gp.GET_SCHEMA:
            if len(p) != 6:
                return bytes([gp.BAD_LEN])
            off, mx = gp.dec_u28(p[0:4]), gp.dec_u14(p[4:6])
            if not 1 <= mx <= 64:
                return bytes([gp.BAD_LEN])
            return gp.build_schema_reply(self.schema_bytes, off, mx)
        if cmd in (gp.GET_PARAM, gp.SET_PARAM):
            if len(p) != (4 if cmd == gp.GET_PARAM else 9):
                return bytes([gp.BAD_LEN])
            pid, idx = gp.dec_u14(p[0:2]), gp.dec_u14(p[2:4])
            q = self.check(pid, idx, write=cmd == gp.SET_PARAM)
            if q is None:
                return bytes([gp.BAD_PARAM])
            if cmd == gp.SET_PARAM:
                v = gp.dec_s32(p[4:9])
                if not q.min <= v <= q.max or not self.value_ok(q.id, v):
                    return bytes([gp.BAD_VALUE])
                self.host_set(q, idx, [v])
            return gp.build_param_reply(pid, idx, self.values[(pid, idx)])
        if cmd in (gp.GET_PARAMS, gp.SET_PARAMS):
            if not self.bulk:
                return bytes([gp.BAD_CMD])
            if len(p) < 6:
                return bytes([gp.BAD_LEN])
            pid, idx, n = gp.dec_u14(p[0:2]), gp.dec_u14(p[2:4]), gp.dec_u14(p[4:6])
            if not 1 <= n <= gp.MAX_RUN or len(p) != (6 if cmd == gp.GET_PARAMS else 6 + 5 * n):
                return bytes([gp.BAD_LEN])
            q = self.check(pid, idx, n, write=cmd == gp.SET_PARAMS)
            if q is None:
                return bytes([gp.BAD_PARAM])
            if cmd == gp.GET_PARAMS:
                return gp.build_params_reply(pid, idx, [self.values[(pid, idx + k)] for k in range(n)])
            vals = [gp.dec_s32(p[6 + 5 * k:11 + 5 * k]) for k in range(n)]
            if not all(q.min <= v <= q.max and self.value_ok(q.id, v) for v in vals):
                return bytes([gp.BAD_VALUE])     # all or none
            self.host_set(q, idx, vals)
            return gp.build_set_params_reply(pid, idx, n)
        if cmd == gp.COMMIT:
            self.commits += 1
            if self.presets is not None:
                self.store_slot(self.active)
                return bytes([gp.OK])
            self.stored = {k: v for k, v in self.values.items() if not self.schema.by_id(k[0]).volatile}
            return bytes([gp.OK])
        if cmd == gp.FACTORY_RESET:
            self.values, self.stored = self.defaults(), None
            if self.presets is not None:
                self.values = self.fresh_values()
                self.records = {}
                self.active = 1
                self.load(1)
            return bytes([gp.OK])
        return bytes([gp.BAD_CMD])

    def value_ok(self, pid, v):
        """presets.c presets_value_ok(): preset.active only on a preset in use."""
        return self.presets is None or pid != gp.P_PRESET_ACTIVE or v <= self.used()

    def host_set(self, q, idx, vals):
        """Values from SET_PARAM / SET_PARAMS (already checked)."""
        for k, v in enumerate(vals):
            self.values[(q.id, idx + k)] = v
        if self.presets is None:
            return
        if q.id == gp.P_PRESET_ACTIVE:
            self.select(self.values[(q.id, 0)])       # a set switches; the same slot again reloads it
        elif q.per_preset and not q.volatile:
            self.set_dirty(True)

    # -- presets (sysex.md 4.5) --
    def setup_presets(self, presets, slots, revision):
        for pid in (gp.P_PRESET_ACTIVE, gp.P_PRESET_PROGRAM, gp.P_PRESET_DIRTY):
            if self.schema.by_id(pid) is None:
                raise ValueError(f"a preset schema needs param {pid}")
        self.n_slots = slots or len(presets)
        assert not presets[0].get("vals"), "slot 1 is the component defaults"
        self.yaml_names, self.yaml_vals, self.yaml_defined = {}, {}, set()
        for n in range(1, self.n_slots + 1):
            d = presets[n - 1] if n <= len(presets) else {}
            if n <= len(presets):
                self.yaml_defined.add(n)
            self.yaml_names[n] = d.get("name", f"Preset {n}")
            vals = dict(d.get("vals", {}))
            vals.setdefault((gp.P_PRESET_PROGRAM, 0), d.get("program", -1))
            self.yaml_vals[n] = vals
        self.presets = presets
        self.n_yaml = max(1, min(len(presets), self.n_slots))
        self.has_count = self.schema.by_id(gp.P_PRESET_COUNT) is not None
        self.revision = revision
        self.records = {}         # slot -> {"vals": {(id, index): value}, "name": str or None, "rev": int}
        self.stored = None        # the globals record (slot 0)
        self.boot()

    def preset_keys(self):
        return [(q.id, i) for q in self.schema.params if q.per_preset and not q.volatile for i in range(q.count)]

    def global_keys(self):
        return [(q.id, i) for q in self.schema.params if not q.per_preset and not q.volatile for i in range(q.count)]

    def yaml_value(self, slot, key):
        return self.yaml_vals[slot].get(key, self.schema.by_id(key[0]).default(key[1]))

    def slot_values(self, slot):
        """Defaults -> the slot's YAML overrides -> its stored record."""
        rec = self.records.get(slot, {}).get("vals", {})
        return {k: rec.get(k, self.yaml_value(slot, k)) for k in self.preset_keys()}

    def fresh_values(self):
        """The param defaults; preset.count's is the YAML's list (presets.c GM_PRESETS_IN_USE)."""
        v = self.defaults()
        if self.has_count:
            v[(gp.P_PRESET_COUNT, 0)] = self.n_yaml
        return v

    def used(self):
        """Presets in use: slots 1..used() (every slot on an older firmware)."""
        if not self.has_count:
            return self.n_slots
        return max(1, min(self.n_slots, self.values[(gp.P_PRESET_COUNT, 0)]))

    def set_used(self, n):
        self.values[(gp.P_PRESET_COUNT, 0)] = n

    def write_state(self):
        """presets.c write_state(): preset.active and preset.count into the stored globals, keeping the
        other stored globals as they are (not the live ones)."""
        self.stored = dict(self.stored or {})
        self.stored[(gp.P_PRESET_ACTIVE, 0)] = self.active
        if self.has_count:
            self.stored[(gp.P_PRESET_COUNT, 0)] = self.used()

    def boot(self):
        """Power-up: defaults, the globals record (with preset.active and preset.count), then the active
        slot. Stored presets past the count stay in use (a store from before preset.count)."""
        self.values = self.fresh_values()
        for k, v in (self.stored or {}).items():
            if k in self.values:
                self.values[k] = v
        if self.has_count and self.records:
            self.set_used(max(self.used(), max(self.records)))
        a = self.values[(gp.P_PRESET_ACTIVE, 0)]
        self.active = a if 1 <= a <= self.used() else 1
        self.load(self.active)

    def load(self, slot):
        self.values.update(self.slot_values(slot))
        self.values[(gp.P_PRESET_ACTIVE, 0)] = slot
        self.dirty = False
        self.values[(gp.P_PRESET_DIRTY, 0)] = 0

    def emit(self):
        self.useq = (self.useq + 1) & 0x7F
        self.outbox.append(gp.build_frame(gp.PRESET_CHANGED, self.useq, gp.build_preset_changed(self.active, self.dirty)))

    def select(self, slot):
        self.active = slot
        self.load(slot)
        self.emit()

    def set_dirty(self, d):
        if self.dirty != d:
            self.dirty = d
            self.values[(gp.P_PRESET_DIRTY, 0)] = int(d)
            self.emit()

    def store_slot(self, slot):
        """COMMIT / PRESET_SAVE: the globals record, and the live per-preset values as `slot`'s diff."""
        self.stored = {k: self.values[k] for k in self.global_keys()}
        self.stored[(gp.P_PRESET_ACTIVE, 0)] = slot
        diff = {k: self.values[k] for k in self.preset_keys() if self.values[k] != self.yaml_value(slot, k)}
        name = self.records.get(slot, {}).get("name")
        self.put_record(slot, diff, name)
        changed = slot != self.active
        self.active = slot
        self.values[(gp.P_PRESET_ACTIVE, 0)] = slot
        if changed:
            self.dirty = False
            self.values[(gp.P_PRESET_DIRTY, 0)] = 0
            self.emit()
        else:
            self.set_dirty(False)

    def put_record(self, slot, vals, name):
        """A slot has stored edits while it holds a value change or a name."""
        if vals or name:
            self.records[slot] = {"vals": dict(vals), "name": name, "rev": self.revision}
        else:
            self.records.pop(slot, None)

    def info(self, slot):
        rec = self.records.get(slot)
        flags = ((gp.PI_STORED if rec else 0) | (gp.PI_DIRTY if slot == self.active and self.dirty else 0) |
                 (gp.PI_YAML if slot in self.yaml_defined else 0) |
                 (gp.PI_IN_USE if self.has_count and slot <= self.used() else 0))
        prog_key = (gp.P_PRESET_PROGRAM, 0)
        program = self.values[prog_key] if slot == self.active else self.slot_values(slot)[prog_key]
        return gp.PresetInfo(slot, self.n_slots, self.active, flags, self.revision, rec["rev"] if rec else 0,
                             program, (rec or {}).get("name") or self.yaml_names[slot])

    def preset_cmd(self, cmd, p):
        if cmd == gp.PRESET_MOVE:
            if len(p) != 4:
                return bytes([gp.BAD_LEN])
            return bytes([self.move(gp.dec_u14(p[0:2]), gp.dec_u14(p[2:4]))])
        if cmd == gp.PRESET_NAME:
            if len(p) < 3 or len(p) != 3 + p[2]:
                return bytes([gp.BAD_LEN])
            slot, name = gp.dec_u14(p[0:2]), bytes(p[3:]).decode("ascii")   # checked below: BAD_VALUE
        else:
            if len(p) != 2:
                return bytes([gp.BAD_LEN])
            slot = gp.dec_u14(p)
        if slot > self.n_slots or (slot == 0 and cmd in (gp.PRESET_SAVE, gp.PRESET_NAME)):
            return bytes([gp.BAD_PARAM])
        if cmd == gp.PRESET_INFO:
            return gp.build_preset_info_reply(self.info(slot or self.active))
        if cmd == gp.PRESET_SAVE:
            if slot > self.used():
                self.set_used(slot)              # a free slot: now in use
            self.store_slot(slot)
            return bytes([gp.OK])
        if cmd == gp.PRESET_DELETE:
            return bytes([self.delete(slot)])
        if cmd == gp.PRESET_RESET:
            for n in (range(1, self.n_slots + 1) if slot == 0 else (slot,)):
                self.records.pop(n, None)
            if slot == 0 and self.has_count:
                self.set_used(self.n_yaml)       # the firmware's presets: the YAML's list in use
                if self.active > self.used():
                    self.active = 1
                self.write_state()
            if slot in (0, self.active):
                was = self.dirty
                self.load(self.active)          # back to the YAML values; unsaved edits go too
                if was:
                    self.emit()
            return bytes([gp.OK])
        try:
            gp.check_preset_name(name)
        except ValueError:
            return bytes([gp.BAD_VALUE])
        if slot > self.used():
            # naming a free slot makes it a preset: a copy of preset 1's firmware values, under that name
            diff = {k: self.yaml_value(1, k) for k in self.preset_keys() if self.yaml_value(1, k) != self.yaml_value(slot, k)}
            self.put_record(slot, diff, name or None)
            self.set_used(slot)
            self.write_state()
            return bytes([gp.OK])
        rec = self.records.get(slot, {})
        self.put_record(slot, rec.get("vals", {}), name or None)
        return bytes([gp.OK])

    def moved_name(self, src, dst):
        """presets.c moved_name(): the name `src` shows (stored, else its YAML one; an empty slot's
        "Preset N" is not carried), or None when `dst` would show it anyway."""
        rec = self.records.get(src, {})
        n = rec.get("name") or (self.yaml_names[src] if src in self.yaml_defined else None)
        return None if n == self.yaml_names[dst] else n

    def delete(self, slot):
        """PRESET_DELETE (presets.c presets_delete()): later presets move up, the last one is freed."""
        n = self.used()
        if not 1 <= slot <= n:
            return gp.BAD_PARAM
        if n == 1:
            return gp.BAD_STATE
        was = self.active
        for s in range(slot, n):
            name = self.moved_name(s + 1, s)
            vals = self.slot_values(s + 1)
            self.put_record(s, {k: v for k, v in vals.items() if v != self.yaml_value(s, k)}, name)
        self.records.pop(n, None)
        self.set_used(n - 1)
        nxt = (slot if slot <= n - 1 else n - 1) if was == slot else was - 1 if was > slot else was
        self.select(nxt)
        self.write_state()
        return gp.OK

    def move(self, src, dst):
        """PRESET_MOVE (presets.c presets_move()): the preset in src goes to dst, the ones between shift one
        slot towards src; the active preset follows, its unsaved edits dropped."""
        n = self.used()
        if not (1 <= src <= n and 1 <= dst <= n):
            return gp.BAD_PARAM
        if src == dst:
            return gp.OK
        was = self.active
        held_name = self.moved_name(src, dst)
        held = self.slot_values(src)
        step = 1 if src < dst else -1
        for s in range(src, dst, step):
            name = self.moved_name(s + step, s)
            vals = self.slot_values(s + step)
            self.put_record(s, {k: v for k, v in vals.items() if v != self.yaml_value(s, k)}, name)
        self.put_record(dst, {k: v for k, v in held.items() if v != self.yaml_value(dst, k)}, held_name)
        if was == src:
            nxt = dst
        elif src < dst and src < was <= dst:
            nxt = was - 1
        elif src > dst and dst <= was < src:
            nxt = was + 1
        else:
            nxt = was
        self.select(nxt)
        if nxt != was:
            self.useq = (self.useq + 1) & 0x7F
            self.outbox.append(gp.build_frame(gp.PARAM_CHANGED, self.useq,
                                              gp._param_value_bytes(gp.P_PRESET_ACTIVE, 0, nxt)))
        self.write_state()
        return gp.OK

    def erase(self, off):
        self.flash[off:off + PAGE] = b"\xff" * PAGE


class SimBackend:
    """A backend (gemifi_midi interface) with one simulated device on one port."""
    label = "sim"

    def __init__(self, dev, name="DIY ESP32 (gemifi)"):
        self.dev, self.name = dev, name
        self.opened = 0

    def ports(self):
        return [self.name]

    def open(self, name):
        self.opened += 1
        return SimPort(self.dev)


class SimSerial:
    """A pyserial-like byte stream to a SimDevice: replies come back split into small pieces, with
    boot-log text and other MIDI around them, the way a UART delivers them."""

    def __init__(self, dev, piece=3, noise=b"ets Jun  8 2016 00:22:57\r\nrst:0x1 (POWERON_RESET)\r\n"):
        from gemifi_proto import SysexAssembler
        self.dev, self.piece, self.noise = dev, piece, noise
        self.rx = bytearray(noise + bytes([0x90, 0x3C, 0x40, 0xF8]))   # boot log, a note on, a clock tick
        self.asm = SysexAssembler()
        self.written = bytearray()
        self.closed = False

    def write(self, data):
        self.written += data
        for msg in self.asm.feed(data):
            r = self.dev.handle(msg)
            if r is not None:
                self.rx += bytes([0xFE]) + r      # active sensing in front of it, dropped by the parser
        return len(data)

    @property
    def in_waiting(self):
        return min(len(self.rx), self.piece)

    def read(self, n):
        out = bytes(self.rx[:n])
        del self.rx[:n]
        return out

    def close(self):
        self.closed = True


class SimPort:
    def __init__(self, dev, unsolicited=()):
        self.dev = dev
        self.q = list(unsolicited)
        self.sent = []

    def send(self, msg):
        self.sent.append(bytes(msg))
        r = self.dev.handle(bytes(msg))
        if r is not None:
            self.q.append(r)
        self.q += self.dev.outbox
        self.dev.outbox = []

    def recv(self, timeout):
        out, self.q = self.q, []
        return out

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *e):
        pass


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
in `outbox`. `next_revision` is the presets revision the A/B image booted by REBOOT(0) reports (a firmware
update that changes the YAML presets); stored edits survive it."""
import os
import zlib
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gemifi_image as gi  # noqa: E402
import gemifi_proto as gp  # noqa: E402

PAGE = gp.PAGE_SIZE


class SimDevice:
    def __init__(self, role=gp.ROLE_BOOT, board_id=1, max_chunk=256, drop_replies=(), drop_requests=(),
                 busy_on=(), schema=None, bulk=True, ab=False, fw_version=0x00010000, presets=None, slots=None,
                 revision=0, next_revision=None):
        self.ab = ab
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
        self.flash = bytearray(b"\xff" * gp.APROM_SIZE)
        self.program_count = [0] * (gp.APROM_SIZE // 4)
        self.state = "idle"
        self.last = None          # (frame bytes, reply bytes) of the last completed request
        self.n_req = 0
        self.n_reply = 0
        self.drop_replies, self.drop_requests, self.busy_on = set(drop_replies), set(drop_requests), set(busy_on)
        self.log = []
        self.reboot_to = None
        self.outbox = []          # unsolicited frames (PRESET_CHANGED) the port delivers after the next reply
        self.useq = 0
        self.presets = None
        if presets is not None:
            self.setup_presets(presets, slots, revision)
        self.next_revision = next_revision

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

    # -- protocol --
    def dispatch(self, f):
        self.log.append(gp.CMD_NAMES.get(f.cmd, hex(f.cmd)))
        p = f.payload
        if f.cmd == gp.HELLO:
            app_caps = (gp.CAP_LOG | gp.CAP_EVENT | (gp.CAP_CONFIG if self.schema else 0) | (gp.CAP_FW if self.ab else 0)
                        | (gp.CAP_PRESETS if self.presets is not None else 0))
            h = gp.Hello(self.role, self.board_id, self.fw_version, 1,
                         self.max_chunk if self.role == gp.ROLE_BOOT or self.ab else 0,
                         gp.CAP_FW if self.role == gp.ROLE_BOOT else app_caps, bytes(range(12)),
                         None if self.role == gp.ROLE_BOOT else "sim-device")
            return gp.build_hello_reply(h)
        if f.cmd in (gp.GET_SCHEMA, gp.GET_PARAM, gp.SET_PARAM, gp.GET_PARAMS, gp.SET_PARAMS, gp.COMMIT,
                     gp.FACTORY_RESET):
            if self.role != gp.ROLE_APP or not self.schema:
                return bytes([gp.BAD_CMD])
            return self.config(f.cmd, p)
        if f.cmd in (gp.PRESET_INFO, gp.PRESET_SAVE, gp.PRESET_RESET, gp.PRESET_NAME):
            if self.role != gp.ROLE_APP or self.presets is None:
                return bytes([gp.BAD_CMD])
            return self.preset_cmd(f.cmd, p)
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
            if size == 0 or size % 4 or size > gp.APROM_SIZE:
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
            if h.crc32 != self.crc or gi.image_crc(img) != self.crc:
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
                if not q.min <= v <= q.max:
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
            if not all(q.min <= v <= q.max for v in vals):
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
                self.records = {}
                self.active = 1
                self.load(1)
            return bytes([gp.OK])
        return bytes([gp.BAD_CMD])

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

    def boot(self):
        """Power-up: defaults, the globals record (with preset.active), then the active slot."""
        self.values = self.defaults()
        for k, v in (self.stored or {}).items():
            if k in self.values:
                self.values[k] = v
        a = self.values[(gp.P_PRESET_ACTIVE, 0)]
        self.active = a if 1 <= a <= self.n_slots else 1
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
                 (gp.PI_YAML if slot in self.yaml_defined else 0))
        prog_key = (gp.P_PRESET_PROGRAM, 0)
        program = self.values[prog_key] if slot == self.active else self.slot_values(slot)[prog_key]
        return gp.PresetInfo(slot, self.n_slots, self.active, flags, self.revision, rec["rev"] if rec else 0,
                             program, (rec or {}).get("name") or self.yaml_names[slot])

    def preset_cmd(self, cmd, p):
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
            self.store_slot(slot)
            return bytes([gp.OK])
        if cmd == gp.PRESET_RESET:
            for n in (range(1, self.n_slots + 1) if slot == 0 else (slot,)):
                self.records.pop(n, None)
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
        rec = self.records.get(slot, {})
        self.put_record(slot, rec.get("vals", {}), name or None)
        return bytes([gp.OK])

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


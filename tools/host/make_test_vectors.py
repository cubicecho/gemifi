#!/usr/bin/env python3
"""Write protocol/test-vectors.json from the reference codec (gemifi_proto / gemifi_image).

Usage: make_test_vectors.py [--out PATH] [--check]
--check compares the existing file with what would be generated (exit 1 if different).
Every implementation (bootloader, app, web updater) can test itself against this file.
The unit tests in tests/test_vectors.py check the file against the codec as well.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gemifi_image as gi  # noqa: E402
import gemifi_proto as gp  # noqa: E402

DEFAULT_OUT = os.path.join(HERE, "..", "..", "protocol", "test-vectors.json")


def hx(b):
    return bytes(b).hex(" ").upper()


def sample_raw_image():
    """A 254-byte synthetic APROM image: SP 0x20000FF0, reset 0xE1, zeroed header slot, filler bytes.
    254 is deliberately not a multiple of 4, so mkimage must pad it (with 0xFF) to 256."""
    raw = bytearray((i * 7 + 3) & 0xFF for i in range(254))
    struct.pack_into("<II", raw, 0, 0x20000FF0, 0x000000E1)
    raw[gi.HDR_OFFSET:gi.HDR_OFFSET + gi.HDR_SIZE] = bytes(gi.HDR_SIZE)
    return bytes(raw)


BUTTON_LABELS = ["Sustain", "Edit", "Oct -", "Oct +", "Pitch down", "Pitch up", "Mod"]
BUTTON_ACTIONS = ["None", "CC momentary", "CC toggle", "CC ramp", "Note", "Octave down", "Octave up",
                  "Transpose down", "Transpose up", "Pitch bend down", "Pitch bend up", "Program next",
                  "Program previous"]


def example_schema():
    """The Keystation Mini 32 app's schema (a subset of what the firmware exposes; see its README)."""
    mp = gp.make_param
    return gp.Schema(gp.SCHEMA_VERSION, ["MIDI", "Velocity", "Knob", "Buttons", "Debug"], [
        mp(1, "midi.channel", gp.T_INT, 0, 1, 16, 1, "MIDI channel"),
        mp(2, "midi.transpose", gp.T_INT, 0, -24, 24, 0, "Transpose"),
        mp(3, "midi.octave", gp.T_INT, 0, -4, 4, 0, "Octave"),
        mp(4, "velocity.curve", gp.T_ENUM, 1, 0, None, 0, "Velocity curve", options=["Normal", "Soft", "Hard", "Fixed"]),
        mp(5, "velocity.fixed", gp.T_INT, 1, 1, 127, 100, "Fixed velocity"),
        mp(8, "knob.cc", gp.T_INT, 2, 0, 127, 7, "Knob CC"),
        mp(9, "button.action", gp.T_ENUM, 3, 0, None, [1, 2, 5, 6, 9, 10, 3], "Button action", count=7,
           index_labels=BUTTON_LABELS, options=BUTTON_ACTIONS),
        mp(10, "button.number", gp.T_INT, 3, 0, 127, [64, 80, 0, 0, 0, 0, 1], "Button CC/note number", count=7,
           index_labels=BUTTON_LABELS),
        mp(11, "bend.glide", gp.T_INT, 3, 0, 2000, 80, "Pitch bend glide", unit="ms"),
        mp(13, "debug.events", gp.T_BOOL, 4, 0, 1, 0, "Debug events"),
    ])


def config_vectors():
    schema = example_schema()
    raw = gp.build_schema(schema)
    frames = []

    def fr(desc, cmd, seq, payload, **fields):
        t = {"desc": desc, "cmd": cmd, "seq": seq, "payload": hx(payload), "frame": hx(gp.build_frame(cmd, seq, payload))}
        if fields:
            t["fields"] = fields
        frames.append(t)

    w = gp.SCHEMA_WINDOW
    last = (len(raw) - 1) // w * w
    fr("GET_SCHEMA request offset=0 max=64", gp.GET_SCHEMA, 10, gp.req_get_schema(0, w), offset=0, max=w)
    fr("GET_SCHEMA reply: first window", gp.GET_SCHEMA | gp.REPLY, 10, gp.build_schema_reply(raw, 0, w),
       status=gp.OK, total=len(raw), offset=0, data=hx(raw[:w]))
    fr(f"GET_SCHEMA request offset={last} max=64", gp.GET_SCHEMA, 11, gp.req_get_schema(last, w), offset=last, max=w)
    fr(f"GET_SCHEMA reply: last window, {len(raw) - last} bytes", gp.GET_SCHEMA | gp.REPLY, 11,
       gp.build_schema_reply(raw, last, w), status=gp.OK, total=len(raw), offset=last, data=hx(raw[last:]))
    fr("GET_SCHEMA reply: offset at total, no data", gp.GET_SCHEMA | gp.REPLY, 12,
       gp.build_schema_reply(raw, len(raw), w), status=gp.OK, total=len(raw), offset=len(raw), data="")
    fr("GET_SCHEMA reply BAD_LEN (max 0 or > 64)", gp.GET_SCHEMA | gp.REPLY, 13, bytes([gp.BAD_LEN]), status=gp.BAD_LEN)
    fr("GET_PARAM request midi.transpose (id 2) index 0", gp.GET_PARAM, 20, gp.req_get_param(2, 0), id=2, index=0)
    fr("GET_PARAM reply id 2 index 0 value -12", gp.GET_PARAM | gp.REPLY, 20, gp.build_param_reply(2, 0, -12),
       status=gp.OK, id=2, index=0, value=-12)
    fr("GET_PARAM reply BAD_PARAM", gp.GET_PARAM | gp.REPLY, 21, bytes([gp.BAD_PARAM]), status=gp.BAD_PARAM)
    fr("SET_PARAM request button.action (id 9) index 3 = 8 (Transpose up)", gp.SET_PARAM, 22,
       gp.req_set_param(9, 3, 8), id=9, index=3, value=8)
    fr("SET_PARAM reply id 9 index 3 value 8", gp.SET_PARAM | gp.REPLY, 22, gp.build_param_reply(9, 3, 8),
       status=gp.OK, id=9, index=3, value=8)
    fr("SET_PARAM request bend.glide (id 11) = 2001 (above max 2000)", gp.SET_PARAM, 23,
       gp.req_set_param(11, 0, 2001), id=11, index=0, value=2001)
    fr("SET_PARAM reply BAD_VALUE (to seq 23)", gp.SET_PARAM | gp.REPLY, 23, bytes([gp.BAD_VALUE]), status=gp.BAD_VALUE)
    fr("GET_PARAMS request button.number (id 10) index 0 n 7", gp.GET_PARAMS, 25, gp.req_get_params(10, 0, 7),
       id=10, index=0, n=7)
    fr("GET_PARAMS reply id 10 index 0 n 7", gp.GET_PARAMS | gp.REPLY, 25,
       gp.build_params_reply(10, 0, [64, 80, 0, 0, 0, 0, 1]), status=gp.OK, id=10, index=0, n=7,
       values=[64, 80, 0, 0, 0, 0, 1])
    fr("SET_PARAMS request button.action (id 9) index 2 n 3 = 7, 8, 0", gp.SET_PARAMS, 26,
       gp.req_set_params(9, 2, [7, 8, 0]), id=9, index=2, n=3, values=[7, 8, 0])
    fr("SET_PARAMS reply id 9 index 2 n 3", gp.SET_PARAMS | gp.REPLY, 26, gp.build_set_params_reply(9, 2, 3),
       status=gp.OK, id=9, index=2, n=3)
    fr("SET_PARAMS reply BAD_PARAM (index + n > count)", gp.SET_PARAMS | gp.REPLY, 27, bytes([gp.BAD_PARAM]),
       status=gp.BAD_PARAM)
    fr("COMMIT request", gp.COMMIT, 30, gp.req_commit())
    fr("COMMIT reply OK", gp.COMMIT | gp.REPLY, 30, bytes([gp.OK]), status=gp.OK)
    fr("FACTORY_RESET request", gp.FACTORY_RESET, 31, gp.req_factory_reset())
    fr("FACTORY_RESET reply FLASH_ERR", gp.FACTORY_RESET | gp.REPLY, 31, bytes([gp.FLASH_ERR]), status=gp.FLASH_ERR)
    fr("PARAM_CHANGED midi.octave (id 3) index 0 = -1", gp.PARAM_CHANGED, 40, gp.build_param_changed(3, 0, -1),
       id=3, index=0, value=-1)
    fr("EVENT encoder index 2 delta -3 t=1000", gp.EVENT, 41, gp.build_event(gp.EV_ENCODER, 2, -3 & 0x3FFF, 1000),
       type=gp.EV_ENCODER, index=2, value=-3 & 0x3FFF, delta=-3, t_ms=1000)

    trunc = len(raw) - 5
    return {
        "_comment": [
            "Config (sysex.md 4.3, 4.4): GET_SCHEMA 0x02, GET_PARAM 0x03, SET_PARAM 0x04, COMMIT 0x07,",
            "FACTORY_RESET 0x08, GET_PARAMS 0x09, SET_PARAMS 0x0A, PARAM_CHANGED 0x62; BAD_PARAM 10, BAD_VALUE 11.",
            "frames[].fields are the decoded payload fields (status first for replies).",
            "schema.raw is the unpacked binary schema; schema.parsed is what a parser should produce from it.",
        ],
        "s32": [{"value": x, "bytes": hx(gp.s32(x))} for x in (0, 1, -1, 127, -8192, 2147483647, -2147483648)],
        "schema": {"desc": "Keystation Mini 32 app example schema (v1)", "raw": hx(raw), "size": len(raw),
                   "parsed": schema.to_json()},
        "bad_schemas": [
            {"desc": f"truncated: last 5 bytes of the example missing ({trunc} of {len(raw)})", "raw": hx(raw[:trunc]),
             "error": "truncated"},
            {"desc": "header only, 1 param promised", "raw": hx(bytes([1, 0, 1, 0])), "error": "truncated"},
            {"desc": "schema_version 2", "raw": hx(bytes([2]) + raw[1:]), "error": "version"},
            {"desc": "empty", "raw": "", "error": "truncated"},
        ],
        "frames": frames,
    }


def example_preset_schema():
    """A small app schema with presets (sysex.md 4.4 flag bit 5 PRESET, 4.5 params 17..19): two buttons,
    channel and button actions per preset, the velocity calibration global."""
    mp = gp.make_param
    P = gp.F_PRESET
    labels = ["Sustain", "Edit"]
    actions = BUTTON_ACTIONS + ["Preset next", "Preset previous", "Preset select"]   # 13, 14, 15 (MCU = 16)
    return gp.Schema(gp.SCHEMA_VERSION, ["MIDI", "Buttons", "Presets"], [
        mp(1, "midi.channel", gp.T_INT, 0, 1, 16, 1, "MIDI channel", flags=P),
        mp(6, "velocity.fast", gp.T_INT, 0, 1, 1000, 20, "Velocity fast", unit="0.1 ms"),
        mp(9, "button.action", gp.T_ENUM, 1, 0, None, [1, 13], "Button action", count=2, index_labels=labels,
           options=actions, flags=P),
        mp(10, "button.number", gp.T_INT, 1, 0, 127, [64, 0], "Button CC/note number", count=2,
           index_labels=labels, flags=P),
        mp(gp.P_PRESET_ACTIVE, "preset.active", gp.T_INT, 2, 1, 8, 1, "Active preset"),
        mp(gp.P_PRESET_PROGRAM, "preset.program", gp.T_INT, 2, -1, 127, -1, "Program Change", flags=P),
        mp(gp.P_PRESET_DIRTY, "preset.dirty", gp.T_BOOL, 2, 0, 1, 0, "Unsaved edits",
           flags=gp.F_VOLATILE | gp.F_READ_ONLY),
    ])


def preset_vectors():
    schema = example_preset_schema()
    raw = gp.build_schema(schema)
    frames = []

    def fr(desc, cmd, seq, payload, **fields):
        t = {"desc": desc, "cmd": cmd, "seq": seq, "payload": hx(payload), "frame": hx(gp.build_frame(cmd, seq, payload))}
        if fields:
            t["fields"] = fields
        frames.append(t)

    def info(desc, seq, i):
        fr(desc, gp.PRESET_INFO | gp.REPLY, seq, gp.build_preset_info_reply(i), status=gp.OK, slot=i.slot,
           count=i.count, active=i.active, flags=i.flags, revision=i.revision, stored_revision=i.stored_revision,
           program=i.program, name=i.name)

    fr("PRESET_INFO request slot 0 (the active one)", gp.PRESET_INFO, 50, gp.req_preset_info(0), slot=0)
    info("PRESET_INFO reply: slot 2 of 8, active, unsaved and stored edits, YAML preset, revision 3, "
         "stored under 2, program 5, 'Drums'", 50,
         gp.PresetInfo(2, 8, 2, gp.PI_STORED | gp.PI_DIRTY | gp.PI_YAML, 3, 2, 5, "Drums"))
    fr("PRESET_INFO request slot 8", gp.PRESET_INFO, 51, gp.req_preset_info(8), slot=8)
    info("PRESET_INFO reply: slot 8, empty slot, no stored edits, no program, 'Preset 8'", 51,
         gp.PresetInfo(8, 8, 2, 0, 3, 0, -1, "Preset 8"))
    info("PRESET_INFO reply: 16-character name", 52,
         gp.PresetInfo(1, 1, 1, gp.PI_YAML, 0, 0, 127, "ABCDEFGHIJKLMNOP"))
    info("PRESET_INFO reply: empty name", 53, gp.PresetInfo(1, 1, 1, gp.PI_STORED, 16383, 16383, 0, ""))
    fr("PRESET_INFO reply BAD_PARAM (slot > count)", gp.PRESET_INFO | gp.REPLY, 54, bytes([gp.BAD_PARAM]),
       status=gp.BAD_PARAM)
    fr("SET_PARAM request preset.active (id 17) = 3: select preset 3", gp.SET_PARAM, 55,
       gp.req_set_param(gp.P_PRESET_ACTIVE, 0, 3), id=gp.P_PRESET_ACTIVE, index=0, value=3)
    fr("PRESET_SAVE request slot 4 (save-as)", gp.PRESET_SAVE, 56, gp.req_preset_save(4), slot=4)
    fr("PRESET_SAVE reply OK", gp.PRESET_SAVE | gp.REPLY, 56, bytes([gp.OK]), status=gp.OK)
    fr("PRESET_RESET request slot 0 (all)", gp.PRESET_RESET, 57, gp.req_preset_reset(0), slot=0)
    fr("PRESET_RESET reply OK", gp.PRESET_RESET | gp.REPLY, 57, bytes([gp.OK]), status=gp.OK)
    fr("PRESET_RESET request slot 200", gp.PRESET_RESET, 58, gp.req_preset_reset(200), slot=200)
    fr("PRESET_RESET reply BAD_PARAM", gp.PRESET_RESET | gp.REPLY, 58, bytes([gp.BAD_PARAM]), status=gp.BAD_PARAM)
    fr("PRESET_NAME request slot 2 'Live set'", gp.PRESET_NAME, 59, gp.req_preset_name(2, "Live set"), slot=2,
       name="Live set")
    fr("PRESET_NAME reply OK", gp.PRESET_NAME | gp.REPLY, 59, bytes([gp.OK]), status=gp.OK)
    fr("PRESET_NAME request slot 2 '' (back to the YAML name)", gp.PRESET_NAME, 60, gp.req_preset_name(2, ""),
       slot=2, name="")
    fr("PRESET_NAME request slot 2, 17 characters (device replies BAD_VALUE)", gp.PRESET_NAME, 61,
       gp.u14(2) + bytes([17]) + b"ABCDEFGHIJKLMNOPQ", slot=2, name="ABCDEFGHIJKLMNOPQ")
    fr("PRESET_NAME reply BAD_VALUE", gp.PRESET_NAME | gp.REPLY, 61, bytes([gp.BAD_VALUE]), status=gp.BAD_VALUE)
    fr("PRESET_CHANGED slot 3, clean", gp.PRESET_CHANGED, 70, gp.build_preset_changed(3, 0), slot=3, dirty=0)
    fr("PRESET_CHANGED slot 3, unsaved edits", gp.PRESET_CHANGED, 71, gp.build_preset_changed(3, 1), slot=3, dirty=1)
    return {
        "_comment": [
            "Presets (sysex.md 4.5): PRESET_INFO 0x0B, PRESET_SAVE 0x0C, PRESET_RESET 0x0D, PRESET_NAME 0x0E,",
            "PRESET_CHANGED 0x63; select = SET_PARAM preset.active (id 17). HELLO caps bit 5 = presets.",
            "PRESET_INFO reply: status, slot u14, count u14, active u14, flags (1 stored edits, 2 dirty, 4 YAML),",
            "revision u14, stored_revision u14, program s32 (-1 = none), name string (u8 len 0..16 + ASCII).",
            "schema: an example with the PRESET flag (bit 5 = 32) on per-preset params, and params 17..19.",
        ],
        "cap_presets": gp.CAP_PRESETS,
        "flag_preset": gp.F_PRESET,
        "schema": {"desc": "example app schema with presets (v1)", "raw": hx(raw), "size": len(raw),
                   "parsed": schema.to_json()},
        "frames": frames,
    }


def build():
    v = {"_comment": [
        "gemifi SysEx protocol v1 test vectors, generated by tools/host/make_test_vectors.py from the",
        "reference codec in tools/host. Spec: protocol/sysex.md (this file is derived from it, not normative).",
        "Byte strings are space-separated uppercase hex. Integers are plain JSON numbers.",
        "Command numbers: HELLO 0x01, REBOOT 0x10, FW_BEGIN 0x18, FW_DATA 0x19, FW_END 0x1A, FW_ABORT 0x1B;",
        "replies = cmd | 0x40; LOG 0x60, EVENT 0x61 (renumbered 2026-10-30).",
    ]}
    v["u14"] = [{"value": x, "bytes": hx(gp.u14(x))} for x in (0, 1, 127, 128, 0x1234, 0x3FFF)]
    v["u28"] = [{"value": x, "bytes": hx(gp.u28(x))} for x in (0, 0x200, 0x7E00, 0x0ABCDEF, 0xFFFFFFF)]
    v["u32"] = [{"value": x, "bytes": hx(gp.u32(x))} for x in (0, 0x80, 0x49464D47, 0x01020003, 0xDEADBEEF, 0xFFFFFFFF)]
    packs = [b"", b"\x01", b"\x80", bytes([0xFF] * 7), bytes(range(0x7E, 0x86)),
             bytes([0x81, 0x02, 0x83, 0x04, 0x85, 0x06, 0x87, 0x08, 0x89, 0x0A, 0x8B, 0x0C]),
             bytes((i * 37) & 0xFF for i in range(16))]
    v["pack7"] = [{"raw": hx(p), "packed": hx(gp.pack7(p))} for p in packs]
    v["checksum"] = [{"body": hx(b), "chk": gp.checksum(b)} for b in
                     (bytes([1, 1, 0, 0, 0]), bytes([1, 0x10, 5, 1, 0, 1]), bytes([0x7F] * 10))]

    frames = []

    def fr(desc, cmd, seq, payload):
        frames.append({"desc": desc, "cmd": cmd, "seq": seq, "payload": hx(payload),
                       "frame": hx(gp.build_frame(cmd, seq, payload))})

    fr("HELLO request", gp.HELLO, 0, gp.req_hello())
    fr("HELLO request seq 5 (also in web/updater frame.test.ts)", gp.HELLO, 5, gp.req_hello())
    fr("REBOOT(bootloader) seq 127 (also in web/updater frame.test.ts)", gp.REBOOT, 0x7F, gp.req_reboot(1))
    fr("REBOOT(bootloader) request", gp.REBOOT, 5, gp.req_reboot(gp.REBOOT_BOOT))
    fr("REBOOT reply OK", gp.REBOOT | gp.REPLY, 5, bytes([gp.OK]))
    fr("FW_BEGIN size=0x100 crc=0x89ABCDEF board=1 version=1.2.3", gp.FW_BEGIN, 6,
       gp.req_fw_begin(0x100, 0x89ABCDEF, 1, 0x01020003))
    fr("FW_BEGIN reply OK max_chunk=256", gp.FW_BEGIN | gp.REPLY, 6, bytes([gp.OK]) + gp.u14(256))
    fr("FW_DATA offset=0x200 data=00 81 02 83 F0 F1 F2 F3", gp.FW_DATA, 7,
       gp.req_fw_data(0x200, bytes([0x00, 0x81, 0x02, 0x83, 0xF0, 0xF1, 0xF2, 0xF3])))
    fr("FW_DATA reply OK offset=0x200", gp.FW_DATA | gp.REPLY, 7, bytes([gp.OK]) + gp.u28(0x200))
    fr("FW_END reply BAD_IMAGE", gp.FW_END | gp.REPLY, 8, bytes([gp.BAD_IMAGE]))
    hello = gp.Hello(role=gp.ROLE_BOOT, board_id=1, fw_version=0x00010000, proto_version=1, max_chunk=256,
                     caps=gp.CAP_FW, uid=bytes([0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67]))
    fr("HELLO reply from bootloader: board 1, v0.1.0, max_chunk 256, caps=FW, uid 1032547698BADCFE01234567",
       gp.HELLO | gp.REPLY, 0, gp.build_hello_reply(hello))
    fr("LOG info t=1234 'oct- down'", gp.LOG, 3, gp.build_log(2, 1234, "oct- down"))
    fr("EVENT key index=12 value=100 t=70000", gp.EVENT, 4, gp.build_event(2, 12, 100, 70000))
    v["frames"] = frames
    v["bad_frames"] = [
        {"desc": "checksum off by one", "frame": hx(gp.build_frame(gp.HELLO, 0, b"")[:-2] + bytes([0x03, 0xF7])),
         "error": "checksum"},
        {"desc": "length field says 2, payload has 1 (REBOOT seq 5)",
         "frame": hx(gp.HEADER + bytes([1, 0x10, 5, 2, 0, 1, (1 + 0x10 + 5 + 2 + 1) & 0x7F, 0xF7])), "error": "length"},
    ]
    v["chunk_plan"] = [{"size": s, "max_chunk": m, "chunks": [[o, n] for o, n in gp.chunk_plan(s, m)]}
                       for s, m in ((0x100, 256), (0x500, 256), (0x620, 128), (0x404, 512), (0x10, 4))]

    raw = sample_raw_image()
    img = gi.make_image(raw, 1, gi.parse_version("1.2.3"))
    h = gi.parse_header(img)
    v["image"] = {
        "desc": "mkimage of a 254-byte raw image, board 1, version 1.2.3 (padded with FF to 256)",
        "raw": hx(raw),
        "gfw": hx(img),
        "header_hex": hx(img[gi.HDR_OFFSET:gi.HDR_OFFSET + gi.HDR_SIZE]),
        "fields": {"magic": h.magic, "hdr_version": h.hdr_version, "board_id": h.board_id,
                   "image_size": h.image_size, "crc32": h.crc32, "fw_version": h.fw_version,
                   "proto_version": h.proto_version, "reserved": [h.reserved0, h.reserved1]},
    }
    v["config"] = config_vectors()
    v["presets"] = preset_vectors()
    return v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    text = json.dumps(build(), indent=1) + "\n"
    if a.check:
        with open(a.out) as f:
            same = f.read() == text
        print("up to date" if same else f"{a.out} is out of date; rerun without --check")
        return 0 if same else 1
    with open(a.out, "w") as f:
        f.write(text)
    print(f"wrote {os.path.normpath(a.out)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

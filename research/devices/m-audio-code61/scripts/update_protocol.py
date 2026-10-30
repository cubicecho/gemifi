"""Code-series firmware update format and SysEx stream (see analysis/update-protocol.md).

    python update_protocol.py verify  FirmwareFile.hex.txt
    python update_protocol.py rebuild FirmwareFile.hex.txt app.bin OUT.hex.txt   # swap in a new app image
    python update_protocol.py syx     FirmwareFile.hex.txt OUT.syx               # exact messages the updater sends

Nothing here talks to a device.
"""
import struct
import sys

FLASH = 0x08000000
APP_BASE = 0x08002400
START_UPDATE = bytes.fromhex("f05a576e283c4e45f7")  # handshake; bootloader answers F0 45 F7
CHUNK = 11                                           # record bytes per SysEx message
REPLIES = {0x20: "in update mode", 0x45: "ok/next", 0x1A: "ok/next", 0x15: "NAK"}


def checksum(rec: bytes) -> int:
    """Negated 16-bit sum of every byte after the length field, excluding the checksum."""
    return (-sum(rec[2:-2])) & 0xFFFF


def records(blob: bytes):
    """Split the decoded update file into records. Each starts with a big-endian length (record size - 2)."""
    i = 0
    while i < len(blob):
        n = struct.unpack_from(">H", blob, i)[0] + 2
        yield blob[i:i + n]
        i += n


def address(rec: bytes) -> int:
    return FLASH + struct.unpack_from(">H", rec, 7)[0] * 0x100


def is_data(rec: bytes) -> bool:
    return len(rec) == 0x41E


def with_checksum(rec: bytes) -> bytes:
    return rec[:-2] + struct.pack("<H", checksum(rec))


def load(path: str) -> bytes:
    return bytes.fromhex(open(path).read().strip())


def verify(blob: bytes) -> list:
    out = []
    for r in records(blob):
        ok = struct.unpack("<H", r[-2:])[0] == checksum(r)
        out.append((address(r), len(r), ok))
    return out


def rebuild(blob: bytes, app: bytes) -> bytes:
    """Replace the payload of each data record with the matching slice of `app` (loaded at APP_BASE).

    Records outside the app (erase commands, the 0x0801B000 metadata block) are kept as they are.
    """
    out = bytearray()
    for r in records(blob):
        a = address(r)
        if is_data(r) and APP_BASE <= a < APP_BASE + len(app):
            payload = app[a - APP_BASE:a - APP_BASE + 1024].ljust(1024, b"\xff")
            r = r[:0x12] + payload + r[0x412:]
            r = with_checksum(r)
        out += r
    return bytes(out)


def syx_stream(blob: bytes):
    """Yield (message, wait_for_reply) in the order the stock updater sends them."""
    yield START_UPDATE, True
    for r in records(blob):
        for i in range(0, len(r), CHUNK):
            last = i + CHUNK >= len(r)
            yield b"\xf0" + r[i:i + CHUNK].hex().upper().encode() + b"\xf7", last


def main(argv):
    cmd = argv[1]
    blob = load(argv[2])
    if cmd == "verify":
        res = verify(blob)
        bad = [x for x in res if not x[2]]
        print(f"{len(res)} records, {len(bad)} bad checksums")
        for a, n, ok in bad:
            print(f"  bad: 0x{a:08x} len {n}")
        return 1 if bad else 0
    if cmd == "rebuild":
        app = open(argv[3], "rb").read()
        new = rebuild(blob, app)
        open(argv[4], "w").write(new.hex().upper())
        print(f"wrote {argv[4]}; identical to input: {new == blob}")
        return 0
    if cmd == "syx":
        msgs = list(syx_stream(blob))
        open(argv[3], "wb").write(b"".join(m for m, _ in msgs))
        print(f"{len(msgs)} messages, {sum(w for _, w in msgs)} reply waits -> {argv[3]}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

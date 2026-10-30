"""Pull the Code-series app firmware out of M-Audio's Windows updater.

Usage: python extract_firmware.py "Install M-Audio CODE49 FirmwareUpdater 1.2.1.exe" OUTDIR

Chain: WiX Burn .exe -> embedded MSI (OLE file) -> MSZIP cabinet -> "FirmwareFile"
(hex text) -> update blocks -> flat app image loaded at 0x08002400.
Never runs anything from the installer. Needs: olefile.
"""
import io
import os
import struct
import sys
import zlib

import olefile

OLE_MAGIC = bytes.fromhex("d0cf11e0a1b11ae1")
BLOCK = 0x41E          # 18-byte header + 1024 data bytes + 12-byte trailer
FLASH = 0x08000000


def cab_files(cab):
    """Extract every file from a single-folder MSZIP cabinet."""
    coff = struct.unpack_from("<I", cab, 16)[0]
    nfiles, flags = struct.unpack_from("<HH", cab, 28)
    hdr, cbd = 36, 0
    if flags & 4:
        cbres, _, cbd = struct.unpack_from("<HBB", cab, 36)
        hdr += 4 + cbres
    off, nblocks, ctype = struct.unpack_from("<IHH", cab, hdr)
    assert ctype & 0xF == 1, "only MSZIP is handled"
    p, buf, prev = off, b"", b""
    for _ in range(nblocks):
        _, cb, _ = struct.unpack_from("<IHH", cab, p)
        p += 8 + cbd
        blk = cab[p:p + cb]
        p += cb
        assert blk[:2] == b"CK"
        z = zlib.decompressobj(-15, zdict=prev) if prev else zlib.decompressobj(-15)
        out = z.decompress(blk[2:])
        buf += out
        prev = (prev + out)[-32768:]
    files, p = {}, coff
    for _ in range(nfiles):
        size, uoff = struct.unpack_from("<II", cab, p)
        end = cab.index(b"\0", p + 16)
        files[cab[p + 16:end].decode()] = buf[uoff:uoff + size]
        p = end + 1
    return files


def find_firmware(exe):
    i = exe.find(OLE_MAGIC)
    while i >= 0:
        ole = olefile.OleFileIO(io.BytesIO(exe[i:]))
        for s in ole.listdir():
            data = ole.openstream(s).read()
            if data[:4] == b"MSCF":
                files = cab_files(data)
                if "FirmwareFile" in files:
                    return files["FirmwareFile"]
        i = exe.find(OLE_MAGIC, i + 8)
    raise SystemExit("FirmwareFile not found")


def build_app(raw):
    """Turn update blocks into a flat image. Returns (base, image)."""
    chunks = {}
    for k in range(len(raw) // BLOCK):
        blk = raw[k * BLOCK:(k + 1) * BLOCK]
        if blk[12:16] != b"\x0a\x00\x00\x00":   # data blocks only; the tail holds erase/finish commands
            continue
        addr = FLASH + ((blk[7] << 8) | blk[8]) * 0x100
        chunks[addr] = blk[18:18 + 1024]
    base = min(chunks)
    img = bytearray(b"\xff" * (max(chunks) + 1024 - base))
    for a, d in chunks.items():
        img[a - base:a - base + 1024] = d
    return base, bytes(img)


def main():
    exe_path, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    hextext = find_firmware(open(exe_path, "rb").read())
    raw = bytes.fromhex(hextext.decode().strip())
    base, img = build_app(raw)
    open(os.path.join(outdir, "FirmwareFile.hex.txt"), "wb").write(hextext)
    open(os.path.join(outdir, "update_blocks.bin"), "wb").write(raw)
    open(os.path.join(outdir, f"app_{base:#010x}.bin"), "wb").write(img)
    print(f"app image: {base:#010x}-{base + len(img):#010x} ({len(img)} bytes)")


if __name__ == "__main__":
    main()

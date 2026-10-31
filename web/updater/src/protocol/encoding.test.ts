import { describe, expect, it } from 'vitest';
import {
  DecodeError,
  decodeS32,
  decodeText,
  decodeU14,
  decodeU28,
  decodeU32,
  encodeS32,
  encodeU14,
  encodeU28,
  encodeU32,
  PayloadReader,
  pack,
  packedLength,
  toHex,
  unpack,
  unpackedLength,
} from './encoding.ts';

describe('7-bit integers (§2)', () => {
  it('writes u14 low 7 bits first', () => {
    expect(encodeU14(0)).toEqual([0x00, 0x00]);
    expect(encodeU14(300)).toEqual([0x2c, 0x02]);
    expect(encodeU14(0x3fff)).toEqual([0x7f, 0x7f]);
  });

  it('writes u28 offsets', () => {
    expect(encodeU28(0x200)).toEqual([0x00, 0x04, 0x00, 0x00]);
    expect(encodeU28(0x0fff_ffff)).toEqual([0x7f, 0x7f, 0x7f, 0x7f]);
  });

  it('writes u32 in five bytes, the last carrying the top 4 bits', () => {
    expect(encodeU32(0xffff_ffff)).toEqual([0x7f, 0x7f, 0x7f, 0x7f, 0x0f]);
    expect(encodeU32(0x49464d47)).toEqual([0x47, 0x1a, 0x19, 0x4a, 0x04]);
  });

  it('round-trips', () => {
    for (const v of [0, 1, 127, 128, 0x3fff]) {
      expect(decodeU14(encodeU14(v))).toBe(v);
    }
    for (const v of [0, 0x200, 0x7fff, 0x0fff_ffff]) {
      expect(decodeU28(encodeU28(v))).toBe(v);
    }
    for (const v of [0, 0xcbf4_3926, 0xffff_ffff, 0x8000_0000]) {
      expect(decodeU32(encodeU32(v))).toBe(v);
    }
  });

  it('refuses values that do not fit', () => {
    expect(() => encodeU14(0x4000)).toThrow(RangeError);
    expect(() => encodeU28(-1)).toThrow(RangeError);
    expect(() => encodeU32(2 ** 32)).toThrow(RangeError);
  });

  it('refuses a u32 whose fifth byte overflows, and bytes with bit 7 set', () => {
    expect(() => decodeU32([0, 0, 0, 0, 0x10])).toThrow(DecodeError);
    expect(() => decodeU14([0x80, 0])).toThrow(DecodeError);
  });
});

describe('s32 (§2)', () => {
  it("sends the u32 of the two's-complement pattern", () => {
    expect(encodeS32(-1)).toEqual([0x7f, 0x7f, 0x7f, 0x7f, 0x0f]);
    expect(encodeS32(0)).toEqual([0, 0, 0, 0, 0]);
    expect(encodeS32(1)).toEqual([1, 0, 0, 0, 0]);
    expect(encodeS32(-24)).toEqual(encodeU32(0xffff_ffe8));
    expect(encodeS32(0x7fff_ffff)).toEqual([0x7f, 0x7f, 0x7f, 0x7f, 0x07]);
    expect(encodeS32(-0x8000_0000)).toEqual([0, 0, 0, 0, 0x08]);
  });

  it('round-trips', () => {
    for (const v of [0, 1, -1, 127, -128, 16, -24, 0x7fff_ffff, -0x8000_0000, 123456789, -987654321]) {
      expect(decodeS32(encodeS32(v))).toBe(v);
    }
  });

  it('refuses values that do not fit, and a fifth byte that overflows', () => {
    expect(() => encodeS32(0x8000_0000)).toThrow(RangeError);
    expect(() => encodeS32(-0x8000_0001)).toThrow(RangeError);
    expect(() => encodeS32(1.5)).toThrow(RangeError);
    expect(() => decodeS32([0, 0, 0, 0, 0x10])).toThrow(DecodeError);
    expect(() => decodeS32([0, 0, 0])).toThrow(DecodeError);
  });

  it('reads through PayloadReader', () => {
    const reader = new PayloadReader([...encodeS32(-5), ...encodeS32(42)]);
    expect(reader.s32()).toBe(-5);
    expect(reader.s32()).toBe(42);
    expect(reader.remaining).toBe(0);
  });
});

describe('8→7 packing (§2)', () => {
  it('puts the MSBs first, bit i for input i', () => {
    expect(pack([0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0xff])).toEqual([0x41, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x7f]);
  });

  it('makes n + 1 bytes of a final short group', () => {
    expect(pack([0xff])).toEqual([0x01, 0x7f]);
    expect(pack([0x12, 0x80, 0x34])).toEqual([0x02, 0x12, 0x00, 0x34]);
    expect(pack([])).toEqual([]);
  });

  it('packs 8 bytes into 10', () => {
    const packed = pack([0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80]);
    expect(packed).toEqual([0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x00]);
  });

  it('round-trips every byte value at every group length', () => {
    const all = Array.from({ length: 256 }, (_, i) => i);
    for (let n = 0; n <= 20; n++) {
      const data = all.slice(256 - n);
      const packed = pack(data);
      expect(packed.length).toBe(packedLength(n));
      expect(unpackedLength(packed.length)).toBe(n);
      expect(packed.every((b) => b <= 0x7f)).toBe(true);
      expect(unpack(packed)).toEqual(data);
    }
    expect(unpack(pack(all))).toEqual(all);
  });

  it('packs the 12-byte UID into 14 bytes', () => {
    expect(packedLength(12)).toBe(14);
  });

  it('refuses a lone trailing MSB byte', () => {
    expect(() => unpack([0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x00])).toThrow(DecodeError);
  });
});

describe('text and hex', () => {
  it('decodes 7-bit ASCII', () => {
    expect(decodeText([0x6f, 0x63, 0x74, 0x2d])).toBe('oct-');
  });

  it('formats hex', () => {
    expect(toHex([0xf0, 0x7d, 0x0a])).toBe('F0 7D 0A');
  });
});

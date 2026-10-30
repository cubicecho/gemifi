/**
 * The 7-bit encodings of `protocol/sysex.md` §2: u14, u28, u32, 8→7 packed binary and text.
 * Every function here is pure and works on plain arrays of byte values.
 */

import { DATA_MASK, HEX_RADIX, U32_LIMIT } from './wire.ts';

/** Bits carried by one SysEx data byte. */
const BITS_PER_BYTE = 7;
/** Input bytes per packed group. */
const PACK_GROUP = 7;
/** The high bit of an 8-bit byte. */
const HIGH_BIT = 0x80;
/** Hex digits in one byte. */
const BYTE_HEX_DIGITS = 2;
/** Bytes in a u14. */
export const U14_BYTES = 2;
/** Bytes in a u28. */
export const U28_BYTES = 4;
/** Bytes in a u32. */
export const U32_BYTES = 5;

/** Thrown when bytes cannot be decoded as the spec says. */
export class DecodeError extends Error {
  override name = 'DecodeError';
}

/**
 * Writes an unsigned integer as `count` 7-bit groups, least significant first.
 *
 * @param value - The integer, 0 up to 2^(7·count) − 1 (and at most 2^32 − 1).
 * @param count - How many bytes to write.
 * @returns The bytes.
 */
function encodeSevenBit(value: number, count: number): number[] {
  const limit = Math.min(2 ** (BITS_PER_BYTE * count), U32_LIMIT);
  const isInRange = Number.isInteger(value) && value >= 0 && value < limit;
  if (isInRange === false) {
    throw new RangeError(`${value} does not fit in ${count} 7-bit bytes`);
  }
  const out: number[] = [];
  let rest = value;
  for (let i = 0; i < count; i++) {
    out.push(rest % 2 ** BITS_PER_BYTE);
    rest = Math.floor(rest / 2 ** BITS_PER_BYTE);
  }
  return out;
}

/**
 * Reads `count` 7-bit groups, least significant first.
 *
 * @param bytes - The source.
 * @param offset - Where the integer starts.
 * @param count - How many bytes it takes.
 * @returns The integer.
 */
function decodeSevenBit(bytes: ArrayLike<number>, offset: number, count: number): number {
  if (offset + count > bytes.length) {
    throw new DecodeError(`need ${count} bytes at offset ${offset}, have ${bytes.length - offset}`);
  }
  let value = 0;
  for (let i = count - 1; i >= 0; i--) {
    const byte = bytes[offset + i] ?? 0;
    if (byte > DATA_MASK) {
      throw new DecodeError(`byte ${byte} at offset ${offset + i} is not 7-bit`);
    }
    value = value * 2 ** BITS_PER_BYTE + byte;
  }
  return value;
}

/**
 * Encodes a u14.
 *
 * @param value - 0..16383.
 * @returns Two bytes, low 7 bits first.
 */
export function encodeU14(value: number): number[] {
  return encodeSevenBit(value, U14_BYTES);
}

/**
 * Encodes a u28.
 *
 * @param value - 0..2^28 − 1.
 * @returns Four bytes, low 7 bits first.
 */
export function encodeU28(value: number): number[] {
  return encodeSevenBit(value, U28_BYTES);
}

/**
 * Encodes a u32. The last of its five bytes carries the top 4 bits.
 *
 * @param value - 0..2^32 − 1.
 * @returns Five bytes, low 7 bits first.
 */
export function encodeU32(value: number): number[] {
  return encodeSevenBit(value, U32_BYTES);
}

/**
 * Decodes a u14.
 *
 * @param bytes - The source.
 * @param offset - Where it starts.
 * @returns The value.
 */
export function decodeU14(bytes: ArrayLike<number>, offset = 0): number {
  return decodeSevenBit(bytes, offset, U14_BYTES);
}

/**
 * Decodes a u28.
 *
 * @param bytes - The source.
 * @param offset - Where it starts.
 * @returns The value.
 */
export function decodeU28(bytes: ArrayLike<number>, offset = 0): number {
  return decodeSevenBit(bytes, offset, U28_BYTES);
}

/**
 * Decodes a u32. A fifth byte above 0x0F would overflow 32 bits and is rejected.
 *
 * @param bytes - The source.
 * @param offset - Where it starts.
 * @returns The value.
 */
export function decodeU32(bytes: ArrayLike<number>, offset = 0): number {
  const value = decodeSevenBit(bytes, offset, U32_BYTES);
  if (value >= U32_LIMIT) {
    throw new DecodeError(`u32 at offset ${offset} overflows 32 bits`);
  }
  return value;
}

/**
 * How many SysEx bytes `n` binary bytes pack into.
 *
 * @param n - Unpacked length.
 * @returns Packed length.
 */
export function packedLength(n: number): number {
  return n + Math.ceil(n / PACK_GROUP);
}

/**
 * How many binary bytes `n` packed bytes unpack to.
 *
 * @param n - Packed length.
 * @returns Unpacked length.
 */
export function unpackedLength(n: number): number {
  const fullGroups = Math.floor(n / (PACK_GROUP + 1));
  const rest = n % (PACK_GROUP + 1);
  return fullGroups * PACK_GROUP + Math.max(rest - 1, 0);
}

/**
 * Packs 8-bit data 8→7 (§2, the Korg/Elektron scheme): each group of up to 7 bytes becomes a byte
 * of their MSBs (bit i = bit 7 of input i) followed by the bytes with bit 7 cleared.
 *
 * @param data - Bytes 0..255.
 * @returns 7-bit bytes.
 */
export function pack(data: ArrayLike<number>): number[] {
  const out: number[] = [];
  for (let start = 0; start < data.length; start += PACK_GROUP) {
    const end = Math.min(start + PACK_GROUP, data.length);
    let msbs = 0;
    const low: number[] = [];
    for (let i = start; i < end; i++) {
      const byte = data[i] ?? 0;
      if (byte & HIGH_BIT) {
        msbs |= 1 << (i - start);
      }
      low.push(byte & DATA_MASK);
    }
    out.push(msbs, ...low);
  }
  return out;
}

/**
 * Reverses {@link pack}.
 *
 * @param packed - 7-bit bytes.
 * @returns The original 8-bit data.
 */
export function unpack(packed: ArrayLike<number>): number[] {
  const out: number[] = [];
  const groupBytes = PACK_GROUP + 1;
  for (let start = 0; start < packed.length; start += groupBytes) {
    const end = Math.min(start + groupBytes, packed.length);
    if (end - start === 1) {
      throw new DecodeError(`packed data ends with a lone MSB byte at offset ${start}`);
    }
    const msbs = packed[start] ?? 0;
    for (let i = start + 1; i < end; i++) {
      const byte = packed[i] ?? 0;
      if (byte > DATA_MASK) {
        throw new DecodeError(`byte ${byte} at offset ${i} is not 7-bit`);
      }
      const hasHighBit = (msbs >> (i - start - 1)) & 1;
      out.push(hasHighBit ? byte | HIGH_BIT : byte);
    }
  }
  return out;
}

/**
 * Decodes 7-bit ASCII text (§2: unpacked, no terminator).
 *
 * @param bytes - The text's bytes.
 * @returns The string.
 */
export function decodeText(bytes: ArrayLike<number>): string {
  return String.fromCharCode(...Array.from(bytes, (b) => b & DATA_MASK));
}

/**
 * Formats bytes as spaced upper-case hex, for the raw SysEx view.
 *
 * @param bytes - Any bytes.
 * @returns E.g. `F0 7D 67 6D …`.
 */
export function toHex(bytes: ArrayLike<number>): string {
  return Array.from(bytes, (b) => b.toString(HEX_RADIX).toUpperCase().padStart(BYTE_HEX_DIGITS, '0')).join(' ');
}

/** Reads a payload front to back, so a message parser reads like the spec's field list. */
export class PayloadReader {
  readonly #bytes: ArrayLike<number>;
  #offset = 0;

  /**
   * @param bytes - The payload.
   */
  constructor(bytes: ArrayLike<number>) {
    this.#bytes = bytes;
  }

  /** @returns Bytes not yet read. */
  get remaining(): number {
    return this.#bytes.length - this.#offset;
  }

  /** @returns One 7-bit byte. */
  byte(): number {
    const value = decodeSevenBit(this.#bytes, this.#offset, 1);
    this.#offset += 1;
    return value;
  }

  /** @returns A u14. */
  u14(): number {
    const value = decodeU14(this.#bytes, this.#offset);
    this.#offset += U14_BYTES;
    return value;
  }

  /** @returns A u28. */
  u28(): number {
    const value = decodeU28(this.#bytes, this.#offset);
    this.#offset += U28_BYTES;
    return value;
  }

  /** @returns A u32. */
  u32(): number {
    const value = decodeU32(this.#bytes, this.#offset);
    this.#offset += U32_BYTES;
    return value;
  }

  /**
   * @param unpackedBytes - How many binary bytes the packed field holds.
   * @returns Those bytes, unpacked.
   */
  packed(unpackedBytes: number): number[] {
    const count = packedLength(unpackedBytes);
    if (count > this.remaining) {
      throw new DecodeError(`need ${count} packed bytes, have ${this.remaining}`);
    }
    const slice = Array.from(this.#bytes).slice(this.#offset, this.#offset + count);
    this.#offset += count;
    return unpack(slice);
  }

  /** @returns Everything left, as text. */
  restAsText(): string {
    const slice = Array.from(this.#bytes).slice(this.#offset);
    this.#offset = this.#bytes.length;
    return decodeText(slice);
  }
}

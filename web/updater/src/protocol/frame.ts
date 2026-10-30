/**
 * gemifi SysEx framing (`protocol/sysex.md` §1):
 * `F0 7D 67 6D ver cmd seq len_lo len_hi payload… chk F7`.
 */

import { decodeU14, encodeU14 } from './encoding.ts';
import {
  DATA_MASK,
  HEADER_BYTES,
  MANUFACTURER_ID,
  MAX_PAYLOAD_BYTES,
  MAX_SEQ,
  PROTOCOL_VERSION,
  SIGNATURE,
  SYSEX_END,
  SYSEX_START,
  TRAILER_BYTES,
  VERSION_OFFSET,
} from './wire.ts';

/** One decoded gemifi frame. */
export interface Frame {
  /** Protocol version byte. */
  version: number;
  /** Command: a request, a reply (`cmd | 0x40`) or an unsolicited message. */
  cmd: number;
  /** Sequence number. */
  seq: number;
  /** Payload, still 7-bit encoded. */
  payload: number[];
}

/** Why bytes are not a usable gemifi frame. */
export const FrameProblem = {
  /** Not a SysEx message at all, or truncated. */
  NotSysex: 'not-sysex',
  /** SysEx for someone else (another ID or signature, or a Universal message). */
  NotGemifi: 'not-gemifi',
  /** A gemifi frame of a protocol version this module does not speak. */
  BadVersion: 'bad-version',
  /** The length field disagrees with the frame's size. */
  BadLength: 'bad-length',
  /** The checksum does not match. */
  BadChecksum: 'bad-checksum',
  /** A data byte has bit 7 set. */
  NotSevenBit: 'not-seven-bit',
} as const;
export type FrameProblem = (typeof FrameProblem)[keyof typeof FrameProblem];

/** The result of {@link decodeFrame}. */
export type DecodeResult = { ok: true; frame: Frame } | { ok: false; problem: FrameProblem };

/**
 * The frame checksum: the sum of the bytes from `ver` through the last payload byte, & 0x7F.
 *
 * @param bytes - Exactly those bytes.
 * @returns The 7-bit checksum.
 */
export function checksum(bytes: ArrayLike<number>): number {
  let sum = 0;
  for (let i = 0; i < bytes.length; i++) {
    sum += bytes[i] ?? 0;
  }
  return sum & DATA_MASK;
}

/**
 * Builds a frame.
 *
 * @param cmd - Command byte.
 * @param seq - Sequence number, 0..127.
 * @param payload - 7-bit payload bytes (already encoded and packed).
 * @param version - Protocol version byte.
 * @returns The whole SysEx message, F0 to F7.
 */
export function encodeFrame(
  cmd: number,
  seq: number,
  payload: ArrayLike<number> = [],
  version: number = PROTOCOL_VERSION,
): Uint8Array {
  const isPayloadTooLong = payload.length > MAX_PAYLOAD_BYTES;
  if (isPayloadTooLong) {
    throw new RangeError(`payload of ${payload.length} bytes exceeds the 14-bit length field`);
  }
  const isSeqInRange = Number.isInteger(seq) && seq >= 0 && seq <= MAX_SEQ;
  if (isSeqInRange === false) {
    throw new RangeError(`seq ${seq} is not 0..127`);
  }
  const body = [version, cmd, seq, ...encodeU14(payload.length), ...Array.from(payload)];
  const hasHighBit = body.some((b) => b > DATA_MASK || b < 0);
  if (hasHighBit) {
    throw new RangeError('every byte between F0 and F7 must be 7-bit');
  }
  return Uint8Array.from([SYSEX_START, MANUFACTURER_ID, ...SIGNATURE, ...body, checksum(body), SYSEX_END]);
}

/**
 * Decodes one complete SysEx message as a gemifi frame.
 *
 * @param bytes - The message as received, F0 to F7.
 * @returns The frame, or why it is not one.
 */
export function decodeFrame(bytes: ArrayLike<number>): DecodeResult {
  const data = Array.from(bytes);
  const isSysex = data.length >= 2 && data[0] === SYSEX_START && data[data.length - 1] === SYSEX_END;
  if (isSysex === false) {
    return { ok: false, problem: FrameProblem.NotSysex };
  }
  const isGemifi = data[1] === MANUFACTURER_ID && data[2] === SIGNATURE[0] && data[3] === SIGNATURE[1];
  if (isGemifi === false) {
    return { ok: false, problem: FrameProblem.NotGemifi };
  }
  if (data.length < HEADER_BYTES + TRAILER_BYTES) {
    return { ok: false, problem: FrameProblem.BadLength };
  }
  const inner = data.slice(1, -1);
  const hasHighBit = inner.some((b) => b > DATA_MASK);
  if (hasHighBit) {
    return { ok: false, problem: FrameProblem.NotSevenBit };
  }
  const version = data[VERSION_OFFSET] ?? 0;
  if (version !== PROTOCOL_VERSION) {
    return { ok: false, problem: FrameProblem.BadVersion };
  }
  const length = decodeU14(data, HEADER_BYTES - 2);
  if (data.length !== HEADER_BYTES + length + TRAILER_BYTES) {
    return { ok: false, problem: FrameProblem.BadLength };
  }
  const covered = data.slice(VERSION_OFFSET, HEADER_BYTES + length);
  const chk = data[HEADER_BYTES + length];
  if (checksum(covered) !== chk) {
    return { ok: false, problem: FrameProblem.BadChecksum };
  }
  return {
    ok: true,
    frame: {
      version,
      cmd: data[VERSION_OFFSET + 1] ?? 0,
      seq: data[VERSION_OFFSET + 2] ?? 0,
      payload: data.slice(HEADER_BYTES, HEADER_BYTES + length),
    },
  };
}

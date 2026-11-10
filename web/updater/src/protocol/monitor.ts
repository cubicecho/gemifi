/**
 * Raw readings (`protocol/sysex.md` §4.8): MONITOR starts, renews or stops the stream, and RAW
 * carries one kind's readings as the scan sees them. Framework-free.
 */

import type { RequestOptions, SysexClient } from './client.ts';
import { PayloadReader } from './encoding.ts';
import type { HelloInfo } from './messages.ts';
import { Capability, Command, DATA_MASK, RAW_NO_READING, RawKind, Role } from './wire.ts';

/** How often a host sends MONITOR again to keep the stream going (§4.8: it stops after 5 s). */
export const MONITOR_RENEW_MS = 2000;
/** Bits per byte of a RAW bit field. */
const BITS_PER_BYTE = 7;

/**
 * @param info - HELLO from the connected device, or null.
 * @returns Whether it is the app and streams raw readings (caps bit 9).
 */
export function offersMonitor(info: HelloInfo | null): boolean {
  return info !== null && info.role === Role.App && (info.caps & Capability.Monitor) !== 0;
}

/**
 * The MONITOR request payload.
 *
 * @param kinds - {@link MonitorKind} bits; 0 stops.
 * @returns The payload.
 */
export function monitorPayload(kinds: number): number[] {
  return [kinds & DATA_MASK];
}

/** MONITOR's reply (§4.8). */
export interface MonitorReply {
  /** The kinds the device has and now sends. */
  kinds: number;
  /** The time between two RAW messages. */
  intervalMs: number;
  /** The stream stops this long after the last MONITOR. */
  timeoutMs: number;
}

/**
 * @param body - MONITOR's reply payload without its status byte.
 * @returns Its fields.
 */
export function parseMonitorReply(body: ArrayLike<number>): MonitorReply {
  const reader = new PayloadReader(body);
  return { kinds: reader.byte(), intervalMs: reader.u14(), timeoutMs: reader.u14() };
}

/**
 * Starts or renews the stream (or stops it, with 0).
 *
 * @param client - The device, which has HELLO caps bit 9.
 * @param kinds - {@link MonitorKind} bits.
 * @param options - An abort signal.
 * @returns The reply.
 */
export async function monitor(client: SysexClient, kinds: number, options: RequestOptions = {}): Promise<MonitorReply> {
  return parseMonitorReply(await client.request(Command.Monitor, monitorPayload(kinds), options));
}

/** One RAW message, by kind (§4.8). */
export type RawReading =
  /** Buttons `first`…: pressed, before debounce. */
  | { kind: 'buttons'; first: number; pressed: boolean[] }
  /** Key matrix rows `row`…`row + rows - 1`: `closed[(r - row) * cols + col]`. */
  | { kind: 'matrix'; row: number; rows: number; cols: number; closed: boolean[] }
  /** Analog inputs `first`…: 0..4095, or null for no reading. */
  | { kind: 'analog'; first: number; values: (number | null)[] }
  /** Encoders `first`…: detents since power-up, wrapping at 16384. */
  | { kind: 'encoders'; first: number; counts: number[] }
  /** Pads `first`…: pressure readings 0..4095. */
  | { kind: 'pads'; first: number; values: number[] }
  /** A kind this module doesn't know. */
  | { kind: 'unknown'; type: number };

/**
 * @param reader - At the bit field.
 * @param n - Bits to read.
 * @returns The bits, 7 per byte, low bit first.
 */
function readBits(reader: PayloadReader, n: number): boolean[] {
  const bytes = Array.from({ length: Math.ceil(n / BITS_PER_BYTE) }, () => reader.byte());
  return Array.from(
    { length: n },
    (_, i) => ((bytes[Math.floor(i / BITS_PER_BYTE)] ?? 0) >> (i % BITS_PER_BYTE)) % 2 === 1,
  );
}

/**
 * @param reader - At `n` u14 values.
 * @param n - How many.
 * @returns The values.
 */
function readU14s(reader: PayloadReader, n: number): number[] {
  return Array.from({ length: n }, () => reader.u14());
}

/**
 * Parses a RAW payload.
 *
 * @param payload - The frame's payload.
 * @returns The reading.
 */
export function parseRaw(payload: ArrayLike<number>): RawReading {
  const reader = new PayloadReader(payload);
  const type = reader.byte();
  switch (type) {
    case RawKind.Buttons: {
      const first = reader.u14();
      return { kind: 'buttons', first, pressed: readBits(reader, reader.byte()) };
    }
    case RawKind.KeyMatrix: {
      const row = reader.byte();
      const rows = reader.byte();
      const cols = reader.byte();
      return { kind: 'matrix', row, rows, cols, closed: readBits(reader, rows * cols) };
    }
    case RawKind.Analog: {
      const first = reader.u14();
      const values = readU14s(reader, reader.byte()).map((v) => (v === RAW_NO_READING ? null : v));
      return { kind: 'analog', first, values };
    }
    case RawKind.Encoders: {
      const first = reader.u14();
      return { kind: 'encoders', first, counts: readU14s(reader, reader.byte()) };
    }
    case RawKind.Pads: {
      const first = reader.u14();
      return { kind: 'pads', first, values: readU14s(reader, reader.byte()) };
    }
    default:
      return { kind: 'unknown', type };
  }
}

/** Encoder counts wrap at this (§4.8). */
export const ENCODER_COUNT_WRAP = 0x4000;

/**
 * The detents between two encoder counts, the short way round the 14-bit wrap.
 *
 * @param from - The earlier count.
 * @param to - The later count.
 * @returns The signed change.
 */
export function countDelta(from: number, to: number): number {
  const half = ENCODER_COUNT_WRAP / 2;
  const d = (((to - from) % ENCODER_COUNT_WRAP) + ENCODER_COUNT_WRAP) % ENCODER_COUNT_WRAP;
  return d >= half ? d - ENCODER_COUNT_WRAP : d;
}

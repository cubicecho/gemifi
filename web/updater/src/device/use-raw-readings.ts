import { useEffect, useRef, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';
import { MONITOR_RENEW_MS, monitor, type RawReading } from '../protocol/monitor.ts';

/** The latest raw readings, kind by kind (§4.8). */
export interface RawSnapshot {
  /** Buttons pressed, by index. */
  buttons: readonly boolean[];
  /** The key matrix: `closed[row * cols + col]`; rows grow as messages for later rows arrive. */
  matrix: { rows: number; cols: number; closed: readonly boolean[] } | null;
  /** Analog readings 0..4095, null = no reading. */
  analog: readonly (number | null)[];
  /** Encoder detent counts, wrapping at 16384. */
  encoders: readonly number[];
  /** Pad pressure readings 0..4095. */
  pads: readonly number[];
}

/** What {@link useRawReadings} gives. */
export interface RawReadings {
  snapshot: RawSnapshot;
  /** The kinds the device said it sends, or null before the first reply. */
  kinds: number | null;
  /** Why MONITOR failed, or null. */
  error: string | null;
}

const EMPTY: RawSnapshot = { buttons: [], matrix: null, analog: [], encoders: [], pads: [] };
/** How often the readings are drawn, at most: often enough to look live, rarely enough to stay cheap. */
const DRAW_MS = 80;

/**
 * @param list - The values so far.
 * @param first - Where the new ones start.
 * @param values - The new ones.
 * @param fill - What goes in a gap before `first`.
 * @returns A copy with them put in.
 */
function spliced<T>(list: readonly T[], first: number, values: readonly T[], fill: T): readonly T[] {
  const isSame = first + values.length <= list.length && values.every((v, i) => list[first + i] === v);
  if (isSame) {
    return list;
  }
  const next = [...list];
  while (next.length < first) {
    next.push(fill);
  }
  next.splice(first, values.length, ...values);
  return next;
}

/**
 * @param snapshot - The readings.
 * @param key - Which list.
 * @param list - Its new value.
 * @returns The readings with it, or the same readings when it didn't change (nothing to redraw).
 */
function withList<K extends 'buttons' | 'analog' | 'encoders' | 'pads'>(
  snapshot: RawSnapshot,
  key: K,
  list: RawSnapshot[K],
): RawSnapshot {
  return list === snapshot[key] ? snapshot : { ...snapshot, [key]: list };
}

/**
 * @param snapshot - The readings so far.
 * @param reading - One RAW message.
 * @returns The readings with it applied.
 */
export function applyRaw(snapshot: RawSnapshot, reading: RawReading): RawSnapshot {
  switch (reading.kind) {
    case 'buttons':
      return withList(snapshot, 'buttons', spliced(snapshot.buttons, reading.first, reading.pressed, false));
    case 'matrix': {
      const old = snapshot.matrix;
      const isSame =
        old !== null &&
        old.cols === reading.cols &&
        reading.row + reading.rows <= old.rows &&
        reading.closed.every((v, i) => old.closed[reading.row * reading.cols + i] === v);
      if (isSame) {
        return snapshot;
      }
      const rows = Math.max(reading.row + reading.rows, old !== null && old.cols === reading.cols ? old.rows : 0);
      const closed =
        old !== null && old.cols === reading.cols
          ? [...old.closed]
          : Array.from({ length: rows * reading.cols }, () => false);
      while (closed.length < rows * reading.cols) {
        closed.push(false);
      }
      closed.splice(reading.row * reading.cols, reading.closed.length, ...reading.closed);
      return { ...snapshot, matrix: { rows, cols: reading.cols, closed } };
    }
    case 'analog':
      return withList(snapshot, 'analog', spliced(snapshot.analog, reading.first, reading.values, null));
    case 'encoders':
      return withList(snapshot, 'encoders', spliced(snapshot.encoders, reading.first, reading.counts, 0));
    case 'pads':
      return withList(snapshot, 'pads', spliced(snapshot.pads, reading.first, reading.values, 0));
    default:
      return snapshot;
  }
}

/**
 * Runs MONITOR while `kinds` is non-zero (renewed every 2 s, stopped when it turns 0 or the
 * device goes), and keeps the latest RAW readings.
 *
 * @param client - The device, with HELLO caps bit 9, or null.
 * @param kinds - {@link MonitorKind} bits to stream; 0 = none.
 * @returns The readings.
 */
export function useRawReadings(client: SysexClient | null, kinds: number): RawReadings {
  const [snapshot, setSnapshot] = useState<RawSnapshot>(EMPTY);
  const [sent, setSent] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const latest = useRef<RawSnapshot>(EMPTY);
  useEffect(() => {
    if (client === null || kinds === 0) {
      return;
    }
    latest.current = EMPTY;
    setSnapshot(EMPTY);
    setError(null);
    let hasChanged = false;
    const stopMessages = client.onMessage((message) => {
      if (message.kind === 'raw') {
        const next = applyRaw(latest.current, message.reading);
        if (next !== latest.current) {
          latest.current = next;
          hasChanged = true;
        }
      }
    });
    const ask = () =>
      monitor(client, kinds).then(
        (reply) => setSent(reply.kinds),
        (failure: unknown) => setError(failure instanceof Error ? failure.message : String(failure)),
      );
    void ask();
    const renew = setInterval(ask, MONITOR_RENEW_MS);
    const draw = setInterval(() => {
      if (hasChanged) {
        hasChanged = false;
        setSnapshot(latest.current);
      }
    }, DRAW_MS);
    return () => {
      clearInterval(renew);
      clearInterval(draw);
      stopMessages();
      void monitor(client, 0).catch(() => undefined);
    };
  }, [client, kinds]);
  return { snapshot, kinds: sent, error };
}

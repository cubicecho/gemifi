/**
 * The MIDI monitor's list: the newest decoded messages, bounded, with the filter the card applies
 * and the line each one is shown as. Framework-free.
 */

import { decodeMidi, type MidiKind, type MidiMessage } from './midi-message.ts';

/** How many messages the monitor keeps: a fader sweep sends hundreds a second. */
export const MIDI_LOG_MAX = 500;

/** One received message. */
export interface MidiLogEntry {
  /** Counts up from 1, for React keys. */
  id: number;
  /** Milliseconds since the monitor started. */
  tMs: number;
  /** Which port it came in on, e.g. `Device`, `DAW`. */
  port: string;
  message: MidiMessage;
}

/** What the monitor shows. */
export interface MidiFilter {
  kinds: ReadonlySet<MidiKind>;
  /** 1..16, or null for every channel. System messages have none and pass a channel filter. */
  channel: number | null;
}

/** Width of the time column's number. */
const TIME_WIDTH = 9;
/** Milliseconds in a second. */
const MS_PER_S = 1000;
/** Decimals of the time column. */
const TIME_DECIMALS = 3;
/** Width of the channel column, `ch 16`. */
const CHANNEL_WIDTH = 5;

/** The newest messages, oldest first. */
export class MidiLog {
  readonly #entries: MidiLogEntry[] = [];
  #nextId = 1;

  /** @returns How many messages came in since the last {@link clear}, kept or not. */
  get total(): number {
    return this.#nextId - 1;
  }

  /**
   * Adds a message, if it is one the monitor decodes.
   *
   * @param bytes - The message, status first.
   * @param port - The port's tag.
   * @param tMs - When, in ms since the monitor started.
   * @returns Whether it was kept (SysEx and stray bytes are not).
   */
  add(bytes: ArrayLike<number>, port: string, tMs: number): boolean {
    const message = decodeMidi(bytes);
    if (message === null) {
      return false;
    }
    this.#entries.push({ id: this.#nextId++, tMs, port, message });
    if (this.#entries.length > MIDI_LOG_MAX) {
      this.#entries.splice(0, this.#entries.length - MIDI_LOG_MAX);
    }
    return true;
  }

  /** @returns A copy of what is kept, oldest first. */
  entries(): MidiLogEntry[] {
    return [...this.#entries];
  }

  /** Forgets everything. */
  clear(): void {
    this.#entries.length = 0;
    this.#nextId = 1;
  }
}

/**
 * @param entry - A message.
 * @param filter - What to show.
 * @returns Whether it passes.
 */
export function passesFilter(entry: MidiLogEntry, filter: MidiFilter): boolean {
  const { kind, channel } = entry.message;
  return filter.kinds.has(kind) && (filter.channel === null || channel === null || channel === filter.channel);
}

/**
 * @param entry - A message.
 * @param portWidth - The widest port tag shown, to line the columns up.
 * @returns E.g. `[   12.345 s] Device  ch 1   Note on  C4 (60)  velocity 100`.
 */
export function formatEntry(entry: MidiLogEntry, portWidth: number): string {
  const time = (entry.tMs / MS_PER_S).toFixed(TIME_DECIMALS).padStart(TIME_WIDTH);
  const channel = entry.message.channel === null ? '' : `ch ${entry.message.channel}`;
  return `[${time} s] ${entry.port.padEnd(portWidth)}  ${channel.padEnd(CHANNEL_WIDTH)}  ${entry.message.text}`;
}

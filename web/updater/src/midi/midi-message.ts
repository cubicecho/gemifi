/**
 * Channel and system MIDI messages, decoded for the MIDI monitor: what a device sends besides
 * gemifi's SysEx. Framework-free.
 */

import { noteName } from '../protocol/schema.ts';

/** What kind of message it is, for the monitor's filter. */
export const MidiKind = {
  Note: 'note',
  Cc: 'cc',
  PitchBend: 'pitch-bend',
  Pressure: 'pressure',
  Program: 'program',
  /** MIDI clock (F8): many a second, so the monitor hides it unless asked. */
  Clock: 'clock',
  /** The other system messages: start, stop, song position, active sensing and the rest. */
  System: 'system',
} as const;
export type MidiKind = (typeof MidiKind)[keyof typeof MidiKind];

/** The monitor's name for each kind, in filter order. */
export const MIDI_KIND_LABELS: Readonly<Record<MidiKind, string>> = Object.freeze({
  note: 'Notes',
  cc: 'Control change',
  'pitch-bend': 'Pitch bend',
  pressure: 'Pressure (aftertouch)',
  program: 'Program change',
  clock: 'Clock',
  system: 'Other system messages',
});

/** One decoded message. */
export interface MidiMessage {
  kind: MidiKind;
  /** 1..16 for a channel message, null for a system one. */
  channel: number | null;
  /** E.g. `Note on  C4 (60)  velocity 100`. */
  text: string;
}

/** Status nibbles of the channel messages. */
const NOTE_OFF = 0x8;
const NOTE_ON = 0x9;
const POLY_PRESSURE = 0xa;
const CONTROL_CHANGE = 0xb;
const PROGRAM_CHANGE = 0xc;
const CHANNEL_PRESSURE = 0xd;
const PITCH_BEND = 0xe;
/** First system status byte. */
const SYSTEM_FIRST = 0xf0;
const SYSEX_START = 0xf0;
const CLOCK = 0xf8;
const MTC_QUARTER_FRAME = 0xf1;
const SONG_POSITION = 0xf2;
const SONG_SELECT = 0xf3;
const HEX = 16;
/** Pitch bend's centre: 0 bend. */
const BEND_CENTRE = 8192;
/** Bits in a data byte. */
const DATA_BITS = 7;
/** A status byte has its top bit set. */
const STATUS_BIT = 0x80;
const NIBBLE = 4;
const LOW_NIBBLE = 0x0f;

/** System messages with no data worth showing, by status. */
const SYSTEM_NAMES: Readonly<Record<number, string>> = Object.freeze({
  246: 'Tune request',
  248: 'Clock',
  250: 'Start',
  251: 'Continue',
  252: 'Stop',
  254: 'Active sensing',
  255: 'Reset',
});

/**
 * @param note - 0..127.
 * @returns E.g. `C4 (60)`.
 */
function noteText(note: number): string {
  return `${noteName(note)} (${note})`;
}

/**
 * @param message - The bytes.
 * @param i - Which data byte (1 = the first after the status).
 * @returns It, or 0 when the message is short.
 */
function data(message: ArrayLike<number>, i: number): number {
  return message[i] ?? 0;
}

/**
 * A system message (F1..FF; SysEx is not decoded here).
 *
 * @param status - Its status byte.
 * @param message - The bytes.
 * @returns The decoded message.
 */
function decodeSystem(status: number, message: ArrayLike<number>): MidiMessage {
  const named = SYSTEM_NAMES[status];
  if (named !== undefined) {
    return { kind: status === CLOCK ? MidiKind.Clock : MidiKind.System, channel: null, text: named };
  }
  let text: string;
  switch (status) {
    case MTC_QUARTER_FRAME:
      text = `MTC quarter frame ${data(message, 1)}`;
      break;
    case SONG_POSITION:
      text = `Song position ${data(message, 1) | (data(message, 2) << DATA_BITS)}`;
      break;
    case SONG_SELECT:
      text = `Song select ${data(message, 1)}`;
      break;
    default:
      text = `System 0x${status.toString(HEX).toUpperCase()}`;
  }
  return { kind: MidiKind.System, channel: null, text };
}

/**
 * Decodes one MIDI message as Web MIDI delivers it (one whole message per event).
 *
 * @param message - The bytes, status first.
 * @returns The message, or null for SysEx, an empty message or stray data bytes.
 */
export function decodeMidi(message: ArrayLike<number>): MidiMessage | null {
  const status = message[0];
  if (status === undefined || status < STATUS_BIT || status === SYSEX_START) {
    return null;
  }
  if (status >= SYSTEM_FIRST) {
    return decodeSystem(status, message);
  }
  const channel = (status & LOW_NIBBLE) + 1;
  const a = data(message, 1);
  const b = data(message, 2);
  switch (status >> NIBBLE) {
    case NOTE_ON:
      // Note On with velocity 0 is a Note Off (running-status style), as every synth reads it.
      return b === 0
        ? { kind: MidiKind.Note, channel, text: `Note off ${noteText(a)}` }
        : { kind: MidiKind.Note, channel, text: `Note on  ${noteText(a)}  velocity ${b}` };
    case NOTE_OFF:
      return { kind: MidiKind.Note, channel, text: `Note off ${noteText(a)}${b === 0 ? '' : `  velocity ${b}`}` };
    case POLY_PRESSURE:
      return { kind: MidiKind.Pressure, channel, text: `Poly pressure ${noteText(a)} = ${b}` };
    case CONTROL_CHANGE:
      return { kind: MidiKind.Cc, channel, text: `CC ${a} = ${b}` };
    case PROGRAM_CHANGE:
      return { kind: MidiKind.Program, channel, text: `Program ${a}` };
    case CHANNEL_PRESSURE:
      return { kind: MidiKind.Pressure, channel, text: `Channel pressure ${a}` };
    case PITCH_BEND: {
      const bend = (a | (b << DATA_BITS)) - BEND_CENTRE;
      return { kind: MidiKind.PitchBend, channel, text: `Pitch bend ${bend > 0 ? '+' : ''}${bend}` };
    }
    default:
      return null;
  }
}

/** Every kind, in filter order. */
export const MIDI_KINDS: readonly MidiKind[] = Object.freeze(Object.values(MidiKind));

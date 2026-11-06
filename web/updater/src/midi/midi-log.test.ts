import { describe, expect, it } from 'vitest';
import { formatEntry, MIDI_LOG_MAX, MidiLog, passesFilter } from './midi-log.ts';
import { MidiKind } from './midi-message.ts';

describe('MidiLog', () => {
  it('keeps the newest messages and skips what it does not decode', () => {
    const log = new MidiLog();
    expect(log.add([0xf0, 0x7d, 0xf7], 'Device', 0)).toBe(false);
    for (let i = 0; i < MIDI_LOG_MAX + 10; i++) {
      log.add([0xb0, 1, i & 0x7f], 'Device', i);
    }
    const entries = log.entries();
    expect(entries).toHaveLength(MIDI_LOG_MAX);
    expect(entries[0]?.id).toBe(11);
    expect(log.total).toBe(MIDI_LOG_MAX + 10);
    log.clear();
    expect(log.entries()).toEqual([]);
    expect(log.total).toBe(0);
  });
});

describe('passesFilter and formatEntry', () => {
  const log = new MidiLog();
  log.add([0x91, 60, 100], 'Device', 1235);
  log.add([0xf8], 'DAW', 1300);
  const [note, clock] = log.entries();

  it('filters by kind and channel; system messages pass any channel', () => {
    if (note === undefined || clock === undefined) {
      throw new Error('two entries');
    }
    const all = new Set(Object.values(MidiKind));
    expect(passesFilter(note, { kinds: all, channel: null })).toBe(true);
    expect(passesFilter(note, { kinds: all, channel: 2 })).toBe(true);
    expect(passesFilter(note, { kinds: all, channel: 1 })).toBe(false);
    expect(passesFilter(note, { kinds: new Set([MidiKind.Cc]), channel: null })).toBe(false);
    expect(passesFilter(clock, { kinds: all, channel: 5 })).toBe(true);
  });

  it('lines the columns up', () => {
    if (note === undefined || clock === undefined) {
      throw new Error('two entries');
    }
    expect(formatEntry(note, 6)).toBe('[    1.235 s] Device  ch 2   Note on  C4 (60)  velocity 100');
    expect(formatEntry(clock, 6)).toBe('[    1.300 s] DAW            Clock');
  });
});

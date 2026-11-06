import { describe, expect, it } from 'vitest';
import { decodeMidi, MidiKind } from './midi-message.ts';

describe('decodeMidi', () => {
  it('names notes with 60 as C4, and reads velocity 0 as a release', () => {
    expect(decodeMidi([0x90, 60, 100])).toEqual({
      kind: MidiKind.Note,
      channel: 1,
      text: 'Note on  C4 (60)  velocity 100',
    });
    expect(decodeMidi([0x95, 61, 0])).toEqual({ kind: MidiKind.Note, channel: 6, text: 'Note off C#4 (61)' });
    expect(decodeMidi([0x8f, 0, 64])?.text).toBe('Note off C-1 (0)  velocity 64');
    expect(decodeMidi([0x8f, 0, 64])?.channel).toBe(16);
  });

  it('decodes CC, program change and both pressures', () => {
    expect(decodeMidi([0xb0, 7, 127])).toEqual({ kind: MidiKind.Cc, channel: 1, text: 'CC 7 = 127' });
    expect(decodeMidi([0xc9, 5])).toEqual({ kind: MidiKind.Program, channel: 10, text: 'Program 5' });
    expect(decodeMidi([0xd0, 64])).toEqual({ kind: MidiKind.Pressure, channel: 1, text: 'Channel pressure 64' });
    expect(decodeMidi([0xa1, 48, 30])?.text).toBe('Poly pressure C3 (48) = 30');
  });

  it('shows pitch bend signed around the centre', () => {
    expect(decodeMidi([0xe0, 0x00, 0x40])?.text).toBe('Pitch bend 0');
    expect(decodeMidi([0xe0, 0x7f, 0x7f])?.text).toBe('Pitch bend +8191');
    expect(decodeMidi([0xe0, 0x00, 0x00])?.text).toBe('Pitch bend -8192');
    expect(decodeMidi([0xe3, 0, 0x40])?.kind).toBe(MidiKind.PitchBend);
  });

  it('tells clock apart from the other system messages', () => {
    expect(decodeMidi([0xf8])).toEqual({ kind: MidiKind.Clock, channel: null, text: 'Clock' });
    expect(decodeMidi([0xfa])).toEqual({ kind: MidiKind.System, channel: null, text: 'Start' });
    expect(decodeMidi([0xf2, 0x01, 0x01])?.text).toBe('Song position 129');
    expect(decodeMidi([0xf3, 4])?.text).toBe('Song select 4');
  });

  it('leaves SysEx, empty messages and stray data bytes out', () => {
    expect(decodeMidi([0xf0, 0x7d, 0xf7])).toBeNull();
    expect(decodeMidi([])).toBeNull();
    expect(decodeMidi([0x40, 0x10])).toBeNull();
  });
});

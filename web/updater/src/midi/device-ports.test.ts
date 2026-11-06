import { describe, expect, it } from 'vitest';
import { otherDevicePorts, portTag } from './device-ports.ts';

const inputs = [
  { name: 'gemifi (Code 61) MIDI 1' },
  { name: 'gemifi (Code 61) DAW' },
  { name: 'gemifi (Keystation Mini 32) MIDI 1' },
  { name: 'Midi Through Port-0' },
  { name: null },
];

describe('otherDevicePorts', () => {
  it("finds the device's other ports by its product string", () => {
    expect(otherDevicePorts('gemifi (Code 61) MIDI 1', inputs).map((input) => input.name)).toEqual([
      'gemifi (Code 61) DAW',
    ]);
  });

  it('finds none for a port that is not a gemifi one, or none selected', () => {
    expect(otherDevicePorts('Midi Through Port-0', inputs)).toEqual([]);
    expect(otherDevicePorts(null, inputs)).toEqual([]);
  });
});

describe('portTag', () => {
  it('keeps what the OS added to the product string', () => {
    expect(portTag('gemifi (Code 61) DAW')).toBe('DAW');
    expect(portTag('gemifi (Code 61) MIDI 2')).toBe('MIDI 2');
    expect(portTag('gemifi (Code 61)')).toBe('gemifi (Code 61)');
    expect(portTag('Some synth')).toBe('Some synth');
  });
});

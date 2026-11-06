import { describe, expect, it } from 'vitest';
import { calibrationSnippet, widen } from './calibration.ts';

describe('calibration', () => {
  it('widens each range with the readings, skipping no reading', () => {
    let seen = widen([], [100, null, 2000]);
    seen = widen(seen, [3900, 500, 1000]);
    expect(seen).toEqual([
      { min: 100, max: 3900 },
      { min: 500, max: 500 },
      { min: 1000, max: 2000 },
    ]);
  });

  it('writes the inputs that moved, pulled in at the ends, and keeps a reversed range reversed', () => {
    const inputs = [
      { id: 'volume', name: 'Volume', isReversed: false },
      { id: 'aftertouch', name: 'Aftertouch', isReversed: true },
      { id: null, name: 'Mod: wheel', isReversed: false },
      { id: 'still', name: 'Still', isReversed: false },
    ];
    const seen = [
      { min: 20, max: 4080 },
      { min: 1200, max: 2300 },
      { min: 0, max: 4095 },
      { min: 50, max: 60 },
    ];
    const { yaml, unmoved } = calibrationSnippet(inputs, seen);
    expect(yaml).toBe(
      [
        'analog:',
        '  - { id: volume, adc_min: 28, adc_max: 4072 }',
        '  - { id: aftertouch, adc_min: 2292, adc_max: 1208 }',
        '  - { name: "Mod: wheel", adc_min: 8, adc_max: 4087 }',
        '',
      ].join('\n'),
    );
    expect(unmoved).toEqual(['Still']);
  });

  it('is empty when nothing moved', () => {
    expect(calibrationSnippet([{ id: 'a', name: 'A', isReversed: false }], [null])).toEqual({
      yaml: '',
      unmoved: ['A'],
    });
  });
});

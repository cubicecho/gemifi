import { describe, expect, it } from 'vitest';
import { YAML_REFERENCE } from './yaml-reference.ts';

describe('YAML examples', () => {
  it('has unique ids and something to show for each', () => {
    expect(new Set(YAML_REFERENCE.map((entry) => entry.id)).size).toBe(YAML_REFERENCE.length);
    for (const entry of YAML_REFERENCE) {
      expect(entry.title).not.toBe('');
      expect(entry.notes.length).toBeGreaterThan(0);
      expect(entry.yaml.endsWith('\n')).toBe(true);
      expect(entry.yaml).not.toContain('\t');
    }
  });

  it('covers the features added since the editor last knew them', () => {
    expect(YAML_REFERENCE.map((entry) => entry.id)).toEqual(
      expect.arrayContaining(['xy_pad', 'din_midi', 'dma', 'analog_modes', 'led_looks', 'presets']),
    );
    const din = YAML_REFERENCE.find((entry) => entry.id === 'din_midi');
    expect(din?.params).toEqual(['midi.thru', 'midi.din_out', 'midi.usb_to_din']);
  });
});

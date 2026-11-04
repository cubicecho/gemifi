import { describe, expect, it } from 'vitest';
import { DinMode } from './api.ts';
import { describeDinMidi, describeMidiPort, describePresets, dmaRows } from './validation-text.ts';

const DIN = { mode: DinMode.SeparatePort, midiIn: true, thru: false, controllerOut: true, usbToDin: false, dma: false };

describe('validation text', () => {
  it('lists the preset slots, marking the empty ones', () => {
    const slots = [
      { slot: 1, name: 'Default', program: null, changes: 0, listed: true },
      { slot: 2, name: 'Drums', program: 9, changes: 1, listed: true },
      { slot: 3, name: 'Preset 3', program: null, changes: 0, listed: false },
    ];
    expect(describePresets({ slots, given: true, maxSlots: 3 })).toBe(
      '3 slots: Default, Drums, Preset 3 (empty). Up to 3 fit.',
    );
    expect(describePresets({ slots: slots.slice(0, 1), given: true, maxSlots: null })).toBe('1 slot: Default.');
  });

  it('says what the DIN jacks do', () => {
    expect(describeDinMidi(DIN)).toBe(
      'In and Out, its own USB-MIDI port (separate_port), thru off, controller to DIN on, interrupt-driven.',
    );
    expect(describeDinMidi({ ...DIN, mode: DinMode.UsbMerge, midiIn: false, usbToDin: true, dma: true })).toBe(
      'Out only, merged into the device’s port (usb_merge), thru off, controller to DIN on, USB to DIN on, by DMA.',
    );
  });

  it('names an extra port by what it carries', () => {
    expect(describeMidiPort({ cable: 1, name: 'Code 61 MIDI', role: 'din' })).toBe(
      'Code 61 MIDI (cable 1: the DIN jacks)',
    );
    expect(describeMidiPort({ cable: 1, name: 'Keys DAW', role: 'daw' })).toBe('Keys DAW (cable 1: Mackie Control)');
  });

  it('draws every DMA1 channel, the free ones as nulls', () => {
    const rows = dmaRows({ channels: [{ channel: 7, request: 'USART2_TX', user: "rgb_serial 'pad_leds'" }], free: [] });
    expect(rows).toHaveLength(7);
    expect(rows[0]).toEqual({ channel: 1, request: null, user: null });
    expect(rows[6]).toEqual({ channel: 7, request: 'USART2_TX', user: "rgb_serial 'pad_leds'" });
  });
});

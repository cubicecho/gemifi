/** How the Devices view words what the validator read. Framework-free. */

import { type DinMidiSummary, DinMode, type DmaMap, type MidiPort, type PresetsSummary } from './api.ts';

/** The STM32F103's DMA1 channel count (RM0008 table 78). */
export const DMA1_CHANNELS = 7;

/** What each DIN MIDI mode means, in the reader's terms. */
const DIN_MODE_TEXT: Readonly<Record<DinMode, string>> = Object.freeze({
  [DinMode.SeparatePort]: 'its own USB-MIDI port (separate_port)',
  [DinMode.UsbMerge]: 'merged into the device’s port (usb_merge)',
  [DinMode.DinOnly]: 'not on USB (din_only)',
});

/**
 * @param presets - The presets as read.
 * @returns E.g. `3 slots: Default, Drums, Preset 3 (empty). Up to 3 fit.`
 */
export function describePresets(presets: PresetsSummary): string {
  const count = presets.slots.length;
  const names = presets.slots.map((slot) => (slot.listed ? slot.name : `${slot.name} (empty)`)).join(', ');
  const fit = presets.maxSlots === null ? '' : ` Up to ${presets.maxSlots} fit.`;
  return `${count} slot${count === 1 ? '' : 's'}: ${names}.${fit}`;
}

/**
 * @param din - `din_midi:` as read.
 * @returns One sentence on what the jacks do.
 */
export function describeDinMidi(din: DinMidiSummary): string {
  const jacks = din.midiIn ? 'In and Out' : 'Out only';
  const parts = [
    `${jacks}, ${DIN_MODE_TEXT[din.mode]}`,
    `thru ${din.thru ? 'on' : 'off'}`,
    `controller to DIN ${din.controllerOut ? 'on' : 'off'}`,
  ];
  if (din.mode === DinMode.UsbMerge) {
    parts.push(`USB to DIN ${din.usbToDin ? 'on' : 'off'}`);
  }
  parts.push(din.dma ? 'by DMA' : 'interrupt-driven');
  return `${parts.join(', ')}.`;
}

/**
 * @param port - An extra USB-MIDI port.
 * @returns E.g. `Code 61 MIDI (cable 1: the DIN jacks)`.
 */
export function describeMidiPort(port: MidiPort): string {
  const what = port.role === 'din' ? 'the DIN jacks' : 'Mackie Control';
  return `${port.name} (cable ${port.cable}: ${what})`;
}

/** One row of the DMA table: a channel and who has it, or nulls for a free one. */
export interface DmaRow {
  channel: number;
  request: string | null;
  user: string | null;
}

/**
 * @param dma - The map.
 * @returns Every channel from 1 to 7 (or higher, if the map names one), in order.
 */
export function dmaRows(dma: DmaMap): DmaRow[] {
  const highest = Math.max(DMA1_CHANNELS, ...dma.channels.map((entry) => entry.channel));
  return Array.from({ length: highest }, (_, i) => {
    const channel = i + 1;
    const used = dma.channels.find((entry) => entry.channel === channel);
    return { channel, request: used?.request ?? null, user: used?.user ?? null };
  });
}

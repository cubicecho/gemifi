import { DescriptionList, PropertyRow } from '@/components/description-list';
import { Section } from '@/components/section';
import { Table, TableBody, TableCaption, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { type DinMidiSummary, DinMode, type DmaMap, type MidiPort, type ValidationResult } from './api.ts';
import { describeDinMidi, describeMidiPort, describePresets, dmaRows } from './validation-text.ts';

/** Props of {@link ValidationFacts}. */
export interface ValidationFactsProps {
  result: ValidationResult;
}

/**
 * @param props - The DMA map.
 * @returns A row per DMA1 channel, the free ones too.
 */
function DmaTable({ dma }: { dma: DmaMap }) {
  return (
    <Table>
      <TableCaption className="sr-only">DMA1 channels and the features that use them</TableCaption>
      <TableHeader>
        <TableRow>
          <TableHead>Channel</TableHead>
          <TableHead>Request</TableHead>
          <TableHead>Used by</TableHead>
        </TableRow>
      </TableHeader>
      <TableBody>
        {dmaRows(dma).map((row) => (
          <TableRow key={row.channel}>
            <TableHead>{row.channel}</TableHead>
            <TableCell className="font-mono text-xs">{row.request ?? '—'}</TableCell>
            <TableCell className={row.user === null ? 'text-foreground/60' : 'font-mono text-xs'}>
              {row.user ?? 'free'}
            </TableCell>
          </TableRow>
        ))}
      </TableBody>
    </Table>
  );
}

/**
 * @param din - `din_midi:` as read.
 * @param ports - The extra USB-MIDI ports.
 * @returns The rows about MIDI.
 */
function midiRows(din: DinMidiSummary | null, ports: MidiPort[]) {
  const rows = [];
  if (din !== null) {
    rows.push(
      <PropertyRow
        key="din"
        label="DIN MIDI"
        value={describeDinMidi(din)}
        hint={
          din.mode === DinMode.UsbMerge
            ? 'Live settings (Config, MIDI group): midi.thru, midi.din_out, midi.usb_to_din.'
            : 'Live settings (Config, MIDI group): midi.thru, midi.din_out.'
        }
      />,
    );
  }
  if (ports.length > 0) {
    rows.push(
      <PropertyRow
        key="ports"
        label="USB MIDI ports"
        value={['The device’s own (cable 0: notes, CCs and the gemifi protocol)', ...ports.map(describeMidiPort)].join(
          '; ',
        )}
        hint="Connect this page to the device’s own port: the others don’t answer the gemifi protocol."
      />,
    );
  }
  return rows;
}

/**
 * What the builder read from a valid file, beyond the summary line: the presets, DIN MIDI and the
 * USB-MIDI ports it adds, and the stm32f1 DMA channel map. Draws nothing when there is none of it.
 */
export function ValidationFacts({ result }: ValidationFactsProps) {
  const { presets, dma, dinMidi, midiPorts } = result;
  const rows = [
    ...(presets?.given ? [<PropertyRow key="presets" label="Presets" value={describePresets(presets)} />] : []),
    ...midiRows(dinMidi, midiPorts),
  ];
  const hasNothing = rows.length === 0 && dma === null;
  if (hasNothing) {
    return null;
  }
  return (
    <>
      {rows.length === 0 ? null : (
        <Section title="What the builder read" level={4} contentSlot={<DescriptionList contentSlot={rows} />} />
      )}
      {dma === null ? null : (
        <Section
          title="DMA channels"
          level={4}
          description={
            dma.free.length === 0
              ? 'Every DMA1 channel is in use. A dma: switch that needs one stops the build.'
              : `Free: ${dma.free.join(', ')}. The dma: switches on rgb_serial and din_midi take the channels the hardware wires them to.`
          }
          contentSlot={<DmaTable dma={dma} />}
        />
      )}
    </>
  );
}

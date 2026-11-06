import { useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { Button } from '@/components/ui/button';
import { CodeBlock } from '@/components/ui/code';
import { ChevronDown, Monitor, Pause, Play } from '@/components/ui/icons';
import {
  Menu,
  MenuCheckboxItem,
  MenuContent,
  MenuRadioGroup,
  MenuRadioItem,
  MenuSeparator,
  MenuTrigger,
} from '@/components/ui/menu';
import { formatEntry, type MidiFilter, passesFilter } from '../midi/midi-log.ts';
import { MIDI_KIND_LABELS, MIDI_KINDS, MidiKind } from '../midi/midi-message.ts';
import type { MidiMonitor } from './use-midi-monitor.ts';

/** MIDI channels, 1..16. */
const CHANNELS = Array.from({ length: 16 }, (_, i) => i + 1);
/** The filter's "every channel" value (radio values are strings). */
const ALL_CHANNELS = 'all';
/** Shown at first: everything but clock, which comes many times a second. */
const DEFAULT_KINDS: ReadonlySet<MidiKind> = new Set(MIDI_KINDS.filter((kind) => kind !== MidiKind.Clock));

/** Props of {@link MidiMonitorCard}. */
export interface MidiMonitorCardProps {
  monitor: MidiMonitor;
  /** The device's ports listened to, for the description: its own, and any others. */
  portNames: readonly string[];
}

/**
 * @param filter - What is shown.
 * @returns The filter button's words, e.g. `All messages`, `4 kinds, channel 10`.
 */
function filterLabel(filter: MidiFilter): string {
  const isEveryKind = MIDI_KINDS.every((kind) => filter.kinds.has(kind));
  const isDefault =
    isEveryKind === false && MIDI_KINDS.every((kind) => filter.kinds.has(kind) === DEFAULT_KINDS.has(kind));
  const kinds = isEveryKind ? 'All messages' : isDefault ? 'All but clock' : `${filter.kinds.size} kinds`;
  return filter.channel === null ? kinds : `${kinds}, channel ${filter.channel}`;
}

/**
 * The notes, CCs and other channel messages the device sends, decoded: note names (60 = C4), CC
 * numbers and values, signed pitch bend, pressure, program changes and system messages, each
 * tagged with its port. Filter by kind and channel, pause, clear.
 *
 * @param props - The monitor and the ports it listens to.
 * @returns The card.
 */
export function MidiMonitorCard({ monitor, portNames }: MidiMonitorCardProps) {
  const [kinds, setKinds] = useState<ReadonlySet<MidiKind>>(DEFAULT_KINDS);
  const [channel, setChannel] = useState<number | null>(null);
  const filter: MidiFilter = { kinds, channel };
  const shown = monitor.entries.filter((entry) => passesFilter(entry, filter));
  const portWidth = Math.max(0, ...shown.map((entry) => entry.port.length));
  const toggleKind = (kind: MidiKind, isOn: boolean) => {
    const next = new Set(kinds);
    if (isOn) {
      next.add(kind);
    } else {
      next.delete(kind);
    }
    setKinds(next);
  };
  const empty =
    monitor.entries.length === 0
      ? 'No notes or CCs yet. Play a key or move a control.'
      : 'Nothing that passes the filter yet.';
  const ports = portNames.length === 0 ? 'the device' : portNames.join(', ');
  return (
    <CardLayout
      title="MIDI monitor"
      iconSlot={<Monitor />}
      description={`What the device sends on ${ports}: notes, CCs, pitch bend, pressure, program changes and system messages. The newest 500 are kept.`}
      actionSlot={
        <>
          <Menu>
            <MenuTrigger asChild>
              <Button size="sm" variant="outline" content={filterLabel(filter)} trailingSlot={<ChevronDown />} />
            </MenuTrigger>
            <MenuContent align="end">
              {MIDI_KINDS.map((kind) => (
                <MenuCheckboxItem
                  key={kind}
                  label={MIDI_KIND_LABELS[kind]}
                  checked={kinds.has(kind)}
                  onCheckedChange={(isOn) => toggleKind(kind, isOn)}
                />
              ))}
              <MenuSeparator />
              <MenuRadioGroup
                aria-label="Channel"
                value={channel === null ? ALL_CHANNELS : String(channel)}
                onValueChange={(value) => setChannel(value === ALL_CHANNELS ? null : Number(value))}
              >
                <MenuRadioItem value={ALL_CHANNELS} label="Every channel" />
                {CHANNELS.map((n) => (
                  <MenuRadioItem key={n} value={String(n)} label={`Channel ${n}`} />
                ))}
              </MenuRadioGroup>
            </MenuContent>
          </Menu>
          <Button
            size="sm"
            variant="outline"
            iconSlot={monitor.isPaused ? <Play /> : <Pause />}
            content={monitor.isPaused ? (monitor.waiting > 0 ? `Resume (${monitor.waiting} new)` : 'Resume') : 'Pause'}
            onClick={() => monitor.setPaused(monitor.isPaused === false)}
          />
          <Button size="sm" variant="outline" content="Clear" onClick={monitor.clear} />
        </>
      }
      contentSlot={
        <CodeBlock
          content={shown.length === 0 ? empty : shown.map((entry) => formatEntry(entry, portWidth)).join('\n')}
          wrap
          maxHeight="lg"
          follow={monitor.isPaused === false}
        />
      }
    />
  );
}

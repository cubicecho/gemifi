import { useMemo } from 'react';
import { CardLayout } from '@/components/card-layout';
import { Combobox } from '@/components/combobox';
import { FormField } from '@/components/form-field';
import { Button } from '@/components/ui/button';
import { SwitchField } from '@/components/ui/switch-field';
import type { Schema } from '../protocol/schema.ts';
import { controlNamer } from './controls.ts';
import { DeviceDrawing } from './device-drawing.tsx';
import type { LearnSource } from './learn-sources.ts';
import type { DeviceMap } from './use-device-map.ts';
import { learnableAs, type MidiLearn } from './use-midi-learn.ts';

/** Props of {@link LearnCard}. */
export interface LearnCardProps {
  learn: MidiLearn;
  schema: Schema;
  map: DeviceMap;
  sources: readonly LearnSource[];
  /** The device sends EVENTs (HELLO caps bit 3), which finding needs. */
  hasEvents: boolean;
}

/**
 * MIDI learn, both ways: "find a control" (move it on the device and its row is highlighted) and
 * "learn a number" (set the chosen CC or note field from another MIDI input).
 *
 * @param props - The learn controller, the schema, the layout and the inputs.
 * @returns The card.
 */
export function LearnCard({ learn, schema, map, sources, hasEvents }: LearnCardProps) {
  const nameOf = useMemo(() => controlNamer(schema, map.components), [schema, map.components]);
  const { target } = learn.rows;
  const targetParam = target === null ? undefined : schema.params.find((param) => param.id === target.id);
  const targetText =
    targetParam === undefined || target === null
      ? 'Click a CC or note field in the tables below, then press Learn.'
      : `Sets ${targetParam.indexLabels?.[target.index] ?? `#${target.index + 1}`}: ${targetParam.label} (${
          learnableAs(targetParam) === 'note' ? 'a note' : learnableAs(targetParam) === 'cc' ? 'a CC' : 'a CC or a note'
        }).`;
  const found = learn.rows.found;
  const foundParam = found === null ? undefined : schema.params.find((param) => param.id === found.id);
  const options = sources.map((source) => ({ value: source.id, label: source.name }));
  const canLearn = learn.sourceId !== null && targetParam !== undefined;
  let status = targetText;
  if (learn.isListening) {
    status = `Listening: play a note or move a control on the other device. Esc stops. ${targetText}`;
  } else if (learn.note !== null) {
    status = learn.note;
  }
  return (
    <CardLayout
      title="MIDI learn"
      description="Find a control's settings by moving it, or set a CC or note number from another MIDI device."
      contentClassName="gap-5"
      contentSlot={
        <>
          <div className="flex flex-col gap-3">
            <SwitchField
              id="find-control"
              label="Find a control"
              description={
                hasEvents
                  ? 'Move a control on the device: its row below is highlighted. Turns the device’s EVENT messages on while it is on.'
                  : 'This firmware sends no EVENT messages, so moved controls cannot be found.'
              }
              checked={learn.isFinding}
              disabled={hasEvents === false}
              onCheckedChange={learn.setIsFinding}
            />
            {learn.isFinding ? (
              <>
                {map.layout === null ? null : (
                  <DeviceDrawing
                    layout={map.layout}
                    nameOf={nameOf}
                    active={learn.events.lit}
                    values={learn.events.values}
                    className="max-h-72"
                  />
                )}
                <p className="text-foreground/70 text-sm" aria-live="polite">
                  {foundParam === undefined || found === null
                    ? 'Move a control on the device.'
                    : `Found: ${foundParam.indexLabels?.[found.index] ?? `#${found.index + 1}`} (${foundParam.label}).`}
                </p>
              </>
            ) : null}
          </div>
          <div className="flex flex-col gap-3">
            <FormField
              label="Learn from"
              description={
                options.length === 0
                  ? 'No other MIDI inputs: connect a synth or open a DAW’s virtual port.'
                  : 'Another MIDI input: a synth, a controller or a DAW.'
              }
              controlSlot={(wired) => (
                <Combobox
                  {...wired}
                  options={options}
                  value={learn.sourceId ?? undefined}
                  placeholder="Choose an input"
                  disabled={options.length === 0}
                  onValueChange={(id) => learn.setSourceId(id)}
                />
              )}
            />
            <div className="flex flex-wrap items-center gap-3">
              <Button
                size="sm"
                variant={learn.isListening ? 'destructive' : 'outline'}
                content={learn.isListening ? 'Stop' : 'Learn'}
                disabled={learn.isListening === false && canLearn === false}
                hint={canLearn ? undefined : 'Choose an input and click a CC or note field first'}
                onClick={learn.toggleListening}
              />
              <p className="min-w-0 flex-1 text-foreground/70 text-sm" aria-live="polite">
                {status}
              </p>
            </div>
          </div>
        </>
      }
    />
  );
}

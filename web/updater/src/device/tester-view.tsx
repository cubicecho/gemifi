import { useEffect, useMemo, useRef, useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { Alert } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { CodeBlock } from '@/components/ui/code';
import { CopyButton } from '@/components/ui/copy-button';
import { Table, TableBody, TableCaption, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { cn } from '@/lib/utils';
import type { SysexClient } from '../protocol/client.ts';
import type { AnalogComponent } from '../protocol/feedback.ts';
import { controlKey } from '../protocol/layout.ts';
import type { HelloInfo } from '../protocol/messages.ts';
import { countDelta, offersMonitor } from '../protocol/monitor.ts';
import type { Schema } from '../protocol/schema.ts';
import { ComponentKind, MonitorKind } from '../protocol/wire.ts';
import { ADC_TOP, type CalibrationInput, calibrationSnippet, type SeenRange, widen } from './calibration.ts';
import { componentOf, controlNamer, ownCount } from './controls.ts';
import { ControlChips, type ControlRef, DeviceDrawing, unplacedControls } from './device-drawing.tsx';
import { LIT_MS, useControlEvents } from './use-control-events.ts';
import type { DeviceMap } from './use-device-map.ts';
import { type RawSnapshot, useRawReadings } from './use-raw-readings.ts';

/** Every kind the tester shows; the device answers with the ones it has. */
const ALL_KINDS =
  MonitorKind.Buttons | MonitorKind.KeyMatrix | MonitorKind.Analog | MonitorKind.Encoders | MonitorKind.Pads;
/** A reading that changes by more than this counts as the control moving (ADC noise is a few counts). */
const MOVE_COUNTS = 24;
/** A pad reading above this counts as pressed. */
const PAD_PRESSED = 150;
/** Encoder detents per turn of the drawn pointer. */
const DETENTS_PER_TURN = 24;
/** The drawing's value of an encoder at count 0: the pointer straight up. */
const POINTER_UP = 0.5;
const PERCENT = 100;

/** Props of {@link TesterView}. */
export interface TesterViewProps {
  client: SysexClient;
  info: HelloInfo;
  /** The schema, for the controls' names, or null before it is read. */
  schema: Schema | null;
  map: DeviceMap;
  /** Whether the view is on screen: the stream and the EVENTs run only then. */
  isShown: boolean;
}

/**
 * @param component - An analog's description, if the device gives it.
 * @returns Its YAML range, low to high, and whether it is reversed; null when not reported.
 */
function yamlRange(component: AnalogComponent | undefined): { low: number; high: number; isReversed: boolean } | null {
  if (component?.adcMin === undefined || component.adcMax === undefined) {
    return null;
  }
  const isReversed = component.adcMin > component.adcMax;
  return {
    low: Math.min(component.adcMin, component.adcMax),
    high: Math.max(component.adcMin, component.adcMax),
    isReversed,
  };
}

/**
 * Lit controls from the raw readings: pressed buttons and pads, analog inputs and encoders that
 * changed in the last {@link LIT_MS}.
 *
 * @param snapshot - The readings.
 * @returns The lit controls, by `controlKey`.
 */
function useRawLit(snapshot: RawSnapshot): ReadonlySet<string> {
  const previous = useRef<RawSnapshot | null>(null);
  const movedAt = useRef(new Map<string, number>());
  const rest = useRef(new Map<string, number>());
  const [, setTick] = useState(0);
  const now = Date.now();
  const before = previous.current;
  if (before !== snapshot) {
    snapshot.analog.forEach((v, i) => {
      const key = controlKey(ComponentKind.Analog, i);
      const anchor = rest.current.get(key);
      if (v === null) {
        return;
      }
      if (anchor === undefined) {
        rest.current.set(key, v);
      } else if (Math.abs(v - anchor) > MOVE_COUNTS) {
        rest.current.set(key, v);
        if (before !== null) {
          movedAt.current.set(key, now);
        }
      }
    });
    snapshot.encoders.forEach((count, i) => {
      if (before !== null && before.encoders[i] !== undefined && before.encoders[i] !== count) {
        movedAt.current.set(controlKey(ComponentKind.Encoder, i), now);
      }
    });
    previous.current = snapshot;
  }
  useEffect(() => {
    const timer = setInterval(() => setTick((n) => n + 1), LIT_MS / 2);
    return () => clearInterval(timer);
  }, []);
  const lit = new Set<string>();
  for (const [key, at] of movedAt.current) {
    if (now - at < LIT_MS) {
      lit.add(key);
    }
  }
  snapshot.buttons.forEach((on, i) => {
    if (on) {
      lit.add(controlKey(ComponentKind.Button, i));
    }
  });
  snapshot.pads.forEach((v, i) => {
    if (v > PAD_PRESSED) {
      lit.add(controlKey(ComponentKind.Pad, i));
    }
  });
  return lit;
}

/**
 * A reading as a bar over 0..4095, with the YAML range marked when the device reports it.
 *
 * @param props - The reading and the ranges.
 * @returns The bar.
 */
function ReadingBar({
  value,
  range,
  seen,
}: {
  value: number | null;
  range: { low: number; high: number } | null;
  seen: { min: number; max: number } | null;
}) {
  const pct = (v: number) => `${(Math.max(0, Math.min(ADC_TOP, v)) / ADC_TOP) * PERCENT}%`;
  return (
    <div className="relative h-3 w-full min-w-32 overflow-hidden rounded-full bg-muted" aria-hidden="true">
      {range === null ? null : (
        <div
          className="absolute inset-y-0 bg-positive/25"
          style={{ left: pct(range.low), width: `calc(${pct(range.high)} - ${pct(range.low)})` }}
        />
      )}
      {seen === null ? null : (
        <div
          className="absolute inset-y-1 bg-info/50"
          style={{ left: pct(seen.min), width: `calc(${pct(seen.max)} - ${pct(seen.min)})` }}
        />
      )}
      {value === null ? null : (
        <div className="absolute inset-y-0 w-1 -translate-x-1/2 rounded-full bg-primary" style={{ left: pct(value) }} />
      )}
    </div>
  );
}

/**
 * The raw key matrix as a grid of contacts, like Vial's matrix tester.
 *
 * @param props - The matrix reading.
 * @returns The grid.
 */
function MatrixGrid({ matrix }: { matrix: NonNullable<RawSnapshot['matrix']> }) {
  const rows = Array.from({ length: matrix.rows }, (_, r) => r);
  const cols = Array.from({ length: matrix.cols }, (_, c) => c);
  return (
    <div className="overflow-x-auto">
      <table className="border-separate border-spacing-1 text-xs">
        <caption className="sr-only">Key matrix contacts: closed ones are filled</caption>
        <thead>
          <tr>
            <th scope="col" className="sr-only">
              Row
            </th>
            {cols.map((c) => (
              <th key={c} scope="col" className="font-normal text-foreground/60">{`C${c}`}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((r) => (
            <tr key={r}>
              <th scope="row" className="pr-1 text-right font-normal text-foreground/60">{`R${r}`}</th>
              {cols.map((c) => {
                const isClosed = matrix.closed[r * matrix.cols + c] === true;
                return (
                  <td key={c}>
                    <div
                      className={cn(
                        'size-5 rounded border',
                        isClosed ? 'border-primary bg-primary' : 'border-border bg-card',
                      )}
                      title={`Row ${r}, column ${c}: ${isClosed ? 'closed' : 'open'}`}
                    />
                  </td>
                );
              })}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

/**
 * The input tester: the device drawn from its layout with the moving control lit, and its raw
 * readings (§4.8) as the scan sees them, for calibrating and porting. While it is shown it keeps
 * MONITOR running and EVENTs on; leaving the view stops both.
 *
 * @param props - The device, its schema and layout, and whether the view is shown.
 * @returns The view's cards.
 */
export function TesterView({ client, info, schema, map, isShown }: TesterViewProps) {
  const hasMonitor = offersMonitor(info);
  const { snapshot, kinds, error } = useRawReadings(client, isShown && hasMonitor ? ALL_KINDS : 0);
  const events = useControlEvents(client, isShown);
  const rawLit = useRawLit(snapshot);
  const nameOf = useMemo(() => controlNamer(schema, map.components), [schema, map.components]);
  const [calibration, setCalibration] = useState<SeenRange | null>(null);
  const [isCalibrating, setIsCalibrating] = useState(false);
  const [seen, setSeen] = useState<SeenRange>([]);
  const [encoderStart, setEncoderStart] = useState<readonly number[] | null>(null);
  // Ranges seen while the view is open, and the calibration pass's own.
  if (snapshot.analog.length > 0) {
    const next = widen(seen, snapshot.analog);
    if (JSON.stringify(next) !== JSON.stringify(seen)) {
      setSeen(next);
    }
    if (isCalibrating && calibration !== null) {
      const pass = widen(calibration, snapshot.analog);
      if (JSON.stringify(pass) !== JSON.stringify(calibration)) {
        setCalibration(pass);
      }
    }
  }
  if (encoderStart === null && snapshot.encoders.length > 0) {
    setEncoderStart(snapshot.encoders);
  }
  useEffect(() => {
    if (isShown === false) {
      setIsCalibrating(false);
    }
  }, [isShown]);

  const analogCount = Math.max(snapshot.analog.length, schema === null ? 0 : ownCount(schema, ComponentKind.Analog));
  const ranges = Array.from({ length: analogCount }, (_, i) => {
    const component = componentOf(map.components, ComponentKind.Analog, i);
    return yamlRange(component?.kind === ComponentKind.Analog ? component : undefined);
  });
  const values = new Map<string, number>(events.values);
  snapshot.analog.forEach((v, i) => {
    if (v !== null) {
      const range = ranges[i] ?? null;
      const low = range?.low ?? 0;
      const high = range?.high ?? ADC_TOP;
      const position = high === low ? 0 : Math.max(0, Math.min(1, (v - low) / (high - low)));
      values.set(controlKey(ComponentKind.Analog, i), range?.isReversed === true ? 1 - position : position);
    }
  });
  snapshot.pads.forEach((v, i) => {
    values.set(controlKey(ComponentKind.Pad, i), v / ADC_TOP);
  });
  snapshot.encoders.forEach((count, i) => {
    values.set(controlKey(ComponentKind.Encoder, i), (count % DETENTS_PER_TURN) / DETENTS_PER_TURN + POINTER_UP);
  });
  const lit = new Set([...events.lit, ...rawLit]);

  const controls: ControlRef[] = [];
  if (schema !== null) {
    for (const kind of [ComponentKind.Button, ComponentKind.Analog, ComponentKind.Encoder, ComponentKind.Pad]) {
      for (let index = 0; index < ownCount(schema, kind); index++) {
        controls.push({ kind, index });
      }
    }
  }
  const unplaced = unplacedControls(map.layout, controls);
  const drawing = (
    <CardLayout
      title="Device"
      description={
        map.layout === null
          ? 'This firmware has no layout, so the controls are listed. Move one to light it up.'
          : 'Move a control: it lights up here.'
      }
      loading={map.isLoading}
      contentClassName="gap-3"
      contentSlot={
        <>
          {map.error === null ? null : (
            <Alert variant="warning" title="Not everything could be read" description={map.error} />
          )}
          {map.layout === null ? null : (
            <DeviceDrawing layout={map.layout} nameOf={nameOf} active={lit} values={values} />
          )}
          {unplaced.length === 0 ? null : (
            <>
              {map.layout === null ? null : <p className="text-foreground/60 text-sm">Not in the layout:</p>}
              <ControlChips controls={unplaced} nameOf={nameOf} active={lit} />
            </>
          )}
        </>
      }
    />
  );
  if (hasMonitor === false) {
    return (
      <>
        <Alert
          variant="info"
          title="No raw readings from this firmware"
          description="It sends EVENTs, so the drawing follows what you move, but not the readings behind them. Firmware with raw readings (HELLO caps bit 9) shows them here too."
        />
        {drawing}
      </>
    );
  }

  const has = (bit: number) => kinds === null || (kinds & bit) !== 0;
  const inputs: CalibrationInput[] = Array.from({ length: analogCount }, (_, i) => {
    const component = componentOf(map.components, ComponentKind.Analog, i);
    return {
      id: component?.id ?? null,
      name: nameOf(ComponentKind.Analog, i),
      isReversed: ranges[i]?.isReversed ?? false,
    };
  });
  const snippet = calibration === null || isCalibrating ? null : calibrationSnippet(inputs, calibration);
  const hasIds = map.components !== null;
  const hasRanges = ranges.some((range) => range !== null);

  return (
    <>
      {error === null ? null : (
        <Alert variant="destructive" title="The raw readings could not be started" description={error} />
      )}
      {drawing}
      {has(MonitorKind.KeyMatrix) && snapshot.matrix !== null ? (
        <CardLayout
          title="Key matrix"
          description="Each contact as the scan reads it, row by column. A velocity key closes two contacts, one after the other."
          contentSlot={<MatrixGrid matrix={snapshot.matrix} />}
        />
      ) : null}
      {has(MonitorKind.Buttons) && snapshot.buttons.length > 0 ? (
        <CardLayout
          title="Buttons"
          description="Lit while pressed, as the pin reads before debounce."
          contentSlot={
            <ControlChips
              controls={snapshot.buttons.map((_, index) => ({ kind: ComponentKind.Button, index }))}
              nameOf={nameOf}
              active={new Set(snapshot.buttons.flatMap((on, i) => (on ? [controlKey(ComponentKind.Button, i)] : [])))}
            />
          }
        />
      ) : null}
      {has(MonitorKind.Analog) && snapshot.analog.length > 0 ? (
        <CardLayout
          title="Analog inputs"
          description={
            hasRanges
              ? 'The ADC reading, 0 to 4095, before calibration. Green is the YAML range (adc_min to adc_max), blue what you have moved through since opening this view.'
              : 'The ADC reading, 0 to 4095, before calibration. This firmware does not report its adc_min and adc_max, so blue shows what you have moved through since opening this view.'
          }
          actionSlot={
            isCalibrating ? (
              <Button size="sm" variant="positive" content="Done" onClick={() => setIsCalibrating(false)} />
            ) : (
              <Button
                size="sm"
                variant="outline"
                content="Calibrate"
                hint="Move every analog control end to end, then press Done"
                onClick={() => {
                  setCalibration([]);
                  setIsCalibrating(true);
                }}
              />
            )
          }
          contentClassName="gap-4"
          contentSlot={
            <>
              {isCalibrating ? (
                <Alert
                  variant="info"
                  title="Calibrating"
                  description="Move every fader, knob, wheel and pedal from one end to the other, then press Done."
                />
              ) : null}
              <Table>
                <TableCaption className="sr-only">Analog readings</TableCaption>
                <TableHeader>
                  <TableRow>
                    <TableHead>Input</TableHead>
                    <TableHead className="text-right">Reading</TableHead>
                    <TableHead className="w-1/2">0 … 4095</TableHead>
                    <TableHead>{hasRanges ? 'adc_min … adc_max' : 'Seen'}</TableHead>
                  </TableRow>
                </TableHeader>
                <TableBody>
                  {snapshot.analog.map((v, i) => {
                    const range = ranges[i] ?? null;
                    const seenHere = (isCalibrating ? calibration?.[i] : seen[i]) ?? null;
                    const rangeText =
                      range === null
                        ? seenHere === null
                          ? '–'
                          : `${seenHere.min} … ${seenHere.max}`
                        : range.isReversed
                          ? `${range.high} … ${range.low} (reversed)`
                          : `${range.low} … ${range.high}`;
                    return (
                      <TableRow
                        key={controlKey(ComponentKind.Analog, i)}
                        className={lit.has(controlKey(ComponentKind.Analog, i)) ? 'bg-selection/40' : undefined}
                      >
                        <TableHead className="whitespace-nowrap">{nameOf(ComponentKind.Analog, i)}</TableHead>
                        <TableCell className="text-right font-mono tabular-nums">{v === null ? 'none' : v}</TableCell>
                        <TableCell>
                          <ReadingBar value={v} range={range} seen={seenHere} />
                        </TableCell>
                        <TableCell className="whitespace-nowrap font-mono tabular-nums">{rangeText}</TableCell>
                      </TableRow>
                    );
                  })}
                </TableBody>
              </Table>
              {snippet === null ? null : snippet.yaml === '' ? (
                <Alert
                  variant="warning"
                  title="Nothing moved far enough"
                  description="Press Calibrate and move each control from one end to the other."
                />
              ) : (
                <div className="flex flex-col gap-2">
                  <CodeBlock
                    title="adc_min and adc_max for the device YAML"
                    content={snippet.yaml}
                    actionSlot={<CopyButton value={snippet.yaml} label="Copy the YAML" />}
                  />
                  <p className="text-foreground/60 text-sm">
                    {`Put these values on the matching entries of analog: in the device YAML, then build and flash.${
                      hasIds
                        ? ''
                        : ' This firmware does not report the YAML ids, so the entries are named by their display name.'
                    }${snippet.unmoved.length === 0 ? '' : ` Not moved far enough, so left out: ${snippet.unmoved.join(', ')}.`}`}
                  </p>
                </div>
              )}
            </>
          }
        />
      ) : null}
      {has(MonitorKind.Encoders) && snapshot.encoders.length > 0 ? (
        <CardLayout
          title="Encoders"
          description="Detents counted since power-up (wrapping at 16384), and since this view opened."
          contentSlot={
            <Table>
              <TableCaption className="sr-only">Encoder counts</TableCaption>
              <TableHeader>
                <TableRow>
                  <TableHead>Encoder</TableHead>
                  <TableHead className="text-right">Count</TableHead>
                  <TableHead className="text-right">Turned</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {snapshot.encoders.map((count, i) => (
                  <TableRow
                    key={controlKey(ComponentKind.Encoder, i)}
                    className={lit.has(controlKey(ComponentKind.Encoder, i)) ? 'bg-selection/40' : undefined}
                  >
                    <TableHead>{nameOf(ComponentKind.Encoder, i)}</TableHead>
                    <TableCell className="text-right font-mono tabular-nums">{count}</TableCell>
                    <TableCell className="text-right font-mono tabular-nums">
                      {countDelta(encoderStart?.[i] ?? count, count)}
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          }
        />
      ) : null}
      {has(MonitorKind.Pads) && snapshot.pads.length > 0 ? (
        <CardLayout
          title="Pads"
          description="The pressure reading, 0 to 4095. Hits start at pad.threshold and reach velocity 127 at pad.full (Pads settings)."
          contentSlot={
            <ul className="grid gap-2 sm:grid-cols-2 lg:grid-cols-4">
              {snapshot.pads.map((v, i) => (
                <li key={controlKey(ComponentKind.Pad, i)} className="flex flex-col gap-1">
                  <span className="flex justify-between text-sm">
                    <span>{nameOf(ComponentKind.Pad, i)}</span>
                    <span className="font-mono tabular-nums">{v}</span>
                  </span>
                  <ReadingBar value={v} range={null} seen={null} />
                </li>
              ))}
            </ul>
          }
        />
      ) : null}
      {kinds === 0 ? <Badge variant="secondary">This device has nothing to stream.</Badge> : null}
    </>
  );
}

import { useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { Combobox } from '@/components/combobox';
import { EmptyState } from '@/components/empty-state';
import { Section } from '@/components/section';
import { Alert } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Monitor, RefreshCw } from '@/components/ui/icons';
import { Input } from '@/components/ui/input';
import { SegmentedButton, SegmentedGroup } from '@/components/ui/segmented';
import { Switch } from '@/components/ui/switch';
import { Table, TableBody, TableCaption, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import type { SysexClient } from '../protocol/client.ts';
import {
  COMPONENT_KIND_NAMES,
  COMPONENT_KINDS,
  type Component,
  DISPLAY_TYPE_NAMES,
  type DisplayComponent,
  type EncoderComponent,
  FEEDBACK_VALUE_MAX,
  fbLedPayload,
  fbRingPayload,
  fbTextPayload,
  fbValuePayload,
  LED_COLOURS_NAMES,
  type LedComponent,
  RING_MODE_NAMES,
  type ValueKind,
} from '../protocol/feedback.ts';
import { noteName } from '../protocol/schema.ts';
import { ComponentKind, FB_TEXT_MAX, LedColours, LedLevel, Notification, RingMode } from '../protocol/wire.ts';
import { type FeedbackController, useFeedback } from './use-feedback.ts';

/** Props of {@link FeedbackCard}. */
export interface FeedbackCardProps {
  /** The connected device, which has HELLO caps bit 6. */
  client: SysexClient;
}

/** A 14-bit slider's starting point: the middle, where a boost/cut ring is empty. */
const VALUE_MIDDLE = Math.ceil(FEEDBACK_VALUE_MAX / 2);

/** The LED choices of a single-colour or RGB LED (RGB: on is white). */
const LED_CHOICES = [
  { label: 'Off', level: LedLevel.Off },
  { label: 'Blink', level: LedLevel.Blink },
  { label: 'On', level: LedLevel.On },
] as const;

/** The LED choices of a bicolour LED: 2..63 is colour 1, 64..127 colour 2. */
const BICOLOUR_CHOICES = [
  { label: 'Off', level: LedLevel.Off },
  { label: 'Blink', level: LedLevel.Blink },
  { label: 'Colour 1', level: LedLevel.Colour1Last },
  { label: 'Colour 2', level: LedLevel.Colour2First },
] as const;

/** FB_RING's modes, for the select. */
const RING_OPTIONS = Object.values(RingMode).map((mode) => ({ value: String(mode), label: RING_MODE_NAMES[mode] }));

/**
 * A 0..16383 slider. The registry has no slider, so this is the native range input.
 *
 * @param props - Its name, its value and what to do on a move.
 * @returns The slider and the number it is on.
 */
function ValueSlider({
  label,
  value,
  onValueChange,
}: {
  label: string;
  value: number;
  onValueChange: (value: number) => void;
}) {
  return (
    <div className="flex min-w-40 items-center gap-2">
      <input
        type="range"
        aria-label={label}
        min={0}
        max={FEEDBACK_VALUE_MAX}
        step={1}
        value={value}
        className="min-w-0 flex-1 accent-active"
        onChange={(event) => onValueChange(Number(event.target.value))}
      />
      <span className="w-12 text-right font-mono text-foreground/60 text-xs">{value}</span>
    </div>
  );
}

/**
 * FB_VALUE for an analog input or an encoder.
 *
 * @param props - The component and the controller.
 * @returns The slider.
 */
function ValueControl({
  component,
  feedback,
}: {
  component: Component & { kind: ValueKind };
  feedback: FeedbackController;
}) {
  const [value, setValue] = useState(0);
  return (
    <ValueSlider
      label={`${component.name}, value`}
      value={value}
      onValueChange={(next) => {
        setValue(next);
        feedback.send(Notification.FbValue, () => fbValuePayload(component.kind, component.index, next));
      }}
    />
  );
}

/**
 * FB_RING for an encoder with a ring: a mode, a value and, with a centre LED, whether it is lit.
 *
 * @param props - The encoder and the controller.
 * @returns The controls.
 */
function RingControl({ encoder, feedback }: { encoder: EncoderComponent; feedback: FeedbackController }) {
  const [mode, setMode] = useState<RingMode>(RingMode.Dot);
  const [value, setValue] = useState(VALUE_MIDDLE);
  const [isCentreOn, setIsCentreOn] = useState(false);
  const sendRing = (next: { mode?: RingMode; value?: number; centre?: boolean }) => {
    const ring = { mode, value, centre: isCentreOn, ...next };
    feedback.send(Notification.FbRing, () => fbRingPayload(encoder.index, ring.mode, ring.value, ring.centre));
  };
  return (
    <div className="flex flex-wrap items-center gap-2">
      <Combobox
        aria-label={`${encoder.name}, ring mode`}
        className="w-32"
        options={RING_OPTIONS}
        value={String(mode)}
        onValueChange={(next) => {
          const nextMode = Number(next) as RingMode;
          setMode(nextMode);
          sendRing({ mode: nextMode });
        }}
      />
      <ValueSlider
        label={`${encoder.name}, ring`}
        value={value}
        onValueChange={(next) => {
          setValue(next);
          sendRing({ value: next });
        }}
      />
      {encoder.center === null ? null : (
        <Switch
          aria-label={`${encoder.name}, centre LED`}
          checked={isCentreOn}
          onCheckedChange={(on) => {
            setIsCentreOn(on);
            sendRing({ centre: on });
          }}
        />
      )}
    </div>
  );
}

/**
 * FB_LED: off, blink, on (or, on a bicolour LED, either colour).
 *
 * @param props - The LED and the controller.
 * @returns The pills.
 */
function LedControl({ led, feedback }: { led: LedComponent; feedback: FeedbackController }) {
  const [level, setLevel] = useState<string | undefined>(undefined);
  const choices = led.colours === LedColours.Bicolour ? BICOLOUR_CHOICES : LED_CHOICES;
  return (
    <SegmentedGroup
      aria-label={`${led.name}, state`}
      value={level}
      onValueChange={(next) => {
        setLevel(next);
        feedback.send(Notification.FbLed, () => fbLedPayload(led.index, Number(next)));
      }}
    >
      {choices.map((choice) => (
        <SegmentedButton key={choice.level} value={String(choice.level)}>
          {choice.label}
        </SegmentedButton>
      ))}
    </SegmentedGroup>
  );
}

/**
 * FB_TEXT at row 0, column 0. The text is padded with spaces to the display's width, so a
 * shorter text clears what a longer one left.
 *
 * @param props - The display and the controller.
 * @returns The text box.
 */
function DisplayControl({ display, feedback }: { display: DisplayComponent; feedback: FeedbackController }) {
  const [text, setText] = useState('');
  const width = display.cols === 0 ? FB_TEXT_MAX : Math.min(display.cols, FB_TEXT_MAX);
  return (
    <Input
      aria-label={`${display.name}, text`}
      className="min-w-40"
      value={text}
      maxLength={width}
      placeholder="Text for row 1"
      onChangeText={(next) => {
        setText(next);
        feedback.send(Notification.FbText, () => fbTextPayload(display.index, 0, 0, next.padEnd(width)));
      }}
    />
  );
}

/**
 * @param component - A component.
 * @returns Its kind's extra facts, in a reader's words.
 */
function details(component: Component): string {
  switch (component.kind) {
    case ComponentKind.Button:
      return component.led === null ? 'No LED' : `LED ${component.led}`;
    case ComponentKind.KeyMatrix:
      return `${component.keys} keys from ${noteName(component.firstNote) || component.firstNote}`;
    case ComponentKind.Analog:
      return '';
    case ComponentKind.Encoder: {
      const centre = component.center === null ? '' : `, centre LED ${component.center}`;
      return component.ring === 0 ? 'No ring' : `Ring of ${component.ring}${centre}`;
    }
    case ComponentKind.Led:
      return LED_COLOURS_NAMES[component.colours] ?? `Colours ${component.colours}`;
    case ComponentKind.Display: {
      const type = DISPLAY_TYPE_NAMES[component.type] ?? `type ${component.type}`;
      return `${component.cols} × ${component.rows}, ${type}`;
    }
  }
}

/**
 * The feedback controls for one component, or null when its kind has none.
 *
 * @param props - The component and the controller.
 * @returns The controls.
 */
function ComponentControls({ component, feedback }: { component: Component; feedback: FeedbackController }) {
  switch (component.kind) {
    case ComponentKind.Analog:
      return <ValueControl component={component} feedback={feedback} />;
    case ComponentKind.Encoder:
      return (
        <div className="flex flex-col gap-2">
          <ValueControl component={component} feedback={feedback} />
          {component.ring === 0 ? null : <RingControl encoder={component} feedback={feedback} />}
        </div>
      );
    case ComponentKind.Led:
      return <LedControl led={component} feedback={feedback} />;
    case ComponentKind.Display:
      return <DisplayControl display={component} feedback={feedback} />;
    default:
      return null;
  }
}

/** Kinds whose rows have feedback controls. */
const KINDS_WITH_CONTROLS: ReadonlySet<ComponentKind> = new Set([
  ComponentKind.Analog,
  ComponentKind.Encoder,
  ComponentKind.Led,
  ComponentKind.Display,
]);

/**
 * One kind's components as a table.
 *
 * @param props - The kind's name, its components and the controller.
 * @returns The section.
 */
function KindSection({
  title,
  components,
  feedback,
}: {
  title: string;
  components: Component[];
  feedback: FeedbackController;
}) {
  const kind = components[0]?.kind;
  const hasControls = kind !== undefined && KINDS_WITH_CONTROLS.has(kind);
  return (
    <Section
      title={title}
      level={4}
      contentSlot={
        <Table>
          <TableCaption className="sr-only">{title}</TableCaption>
          <TableHeader>
            <TableRow>
              <TableHead>#</TableHead>
              <TableHead>Id</TableHead>
              <TableHead>Name</TableHead>
              <TableHead>Role</TableHead>
              <TableHead>Details</TableHead>
              {hasControls ? <TableHead>Feedback</TableHead> : null}
            </TableRow>
          </TableHeader>
          <TableBody>
            {components.map((component) => (
              <TableRow key={component.index}>
                <TableCell className="font-mono">{component.index}</TableCell>
                <TableCell className="font-mono">{component.id}</TableCell>
                <TableCell>{component.name}</TableCell>
                <TableCell className="font-mono">{component.role === '' ? '–' : component.role}</TableCell>
                <TableCell className="whitespace-nowrap">{details(component)}</TableCell>
                {hasControls ? (
                  <TableCell className="align-top">
                    <ComponentControls component={component} feedback={feedback} />
                  </TableCell>
                ) : null}
              </TableRow>
            ))}
          </TableBody>
        </Table>
      }
    />
  );
}

/**
 * The device's components as GET_COMPONENT lists them, with test controls that send the FB_*
 * notifications a DAW script would (§4.6). The device keeps those states until Release all.
 *
 * @param props - The client.
 * @returns The card.
 */
export function FeedbackCard({ client }: FeedbackCardProps) {
  const feedback = useFeedback(client);
  const { state } = feedback;
  const groups =
    state.kind === 'ready'
      ? COMPONENT_KINDS.map((kind) => ({
          kind,
          components: state.components.filter((component) => component.kind === kind),
        })).filter((group) => group.components.length > 0)
      : [];
  return (
    <CardLayout
      title="Host feedback"
      iconSlot={<Monitor />}
      description="Drive the device's LEDs, values and displays by component, as a DAW script does. The device keeps what you set until Release all."
      actionSlot={
        <Button
          size="icon-sm"
          variant="outline"
          aria-label="Read the components again"
          iconSlot={<RefreshCw />}
          disabled={state.kind === 'loading'}
          onClick={feedback.reload}
        />
      }
      loading={state.kind === 'loading'}
      contentSlot={
        <>
          {state.kind === 'error' ? (
            <Alert variant="destructive" title="The components could not be read" description={state.message} />
          ) : null}
          {state.kind === 'ready' && groups.length === 0 ? (
            <EmptyState compact title="The device lists no components." />
          ) : null}
          {groups.map((group) => (
            <KindSection
              key={group.kind}
              title={COMPONENT_KIND_NAMES[group.kind]}
              components={group.components}
              feedback={feedback}
            />
          ))}
          {feedback.sendError === null ? null : (
            <Alert variant="destructive" title="That was not sent" description={feedback.sendError} />
          )}
        </>
      }
      footerActionsSlot={
        <Button
          variant="outline"
          content="Release all"
          disabled={state.kind !== 'ready'}
          onClick={feedback.releaseAll}
        />
      }
    />
  );
}

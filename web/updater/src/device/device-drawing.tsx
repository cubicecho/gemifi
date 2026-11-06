import type { ReactElement } from 'react';
import { Badge } from '@/components/ui/badge';
import { cn } from '@/lib/utils';
import { controlKey, type DeviceLayout, keyRects, type LayoutItem } from '../protocol/layout.ts';
import { noteName } from '../protocol/schema.ts';
import { ComponentKind, LayoutShape } from '../protocol/wire.ts';

/** A control of the device, by kind and index. */
export interface ControlRef {
  kind: number;
  index: number;
}

/** Props of {@link DeviceDrawing}. */
export interface DeviceDrawingProps {
  layout: DeviceLayout;
  /** What each control is called. */
  nameOf: (kind: number, index: number) => string;
  /** The controls to light up, by {@link controlKey} (keys: kind 2 and the key index). */
  active: ReadonlySet<string>;
  /** Positions 0..1 by {@link controlKey}: analog inputs, pads (pressure), encoders (turned). */
  values?: ReadonlyMap<string, number>;
  /** Called when a control is pressed in the drawing. */
  onSelect?: (control: ControlRef) => void;
  className?: string;
}

/** A knob's pointer turns over this many degrees, centred on straight up. */
const KNOB_SWEEP_DEG = 270;
const DEG_PER_TURN = 360;
/** Label size, in layout units (about 5 mm each). */
const LABEL_UNITS = 1.1;
/** Corner radius of square controls, in units. */
const CORNER = 0.35;
/** Below this many units across, a control's name goes under it rather than in it. */
const INSIDE_LABEL_MIN_W = 2.5;
/** The middle of a 0..1 value: a knob straight up, a pitch wheel at rest. */
const MIDDLE = 0.5;
/** Shares of a control's size for the parts of its drawing. */
const Share = {
  /** A round control's body, of its radius: room for the outline. */
  Body: 0.92,
  /** A wheel's inset, in units, each side. */
  WheelInset: 0.1,
  /** A wheel's corners, of its narrow side. */
  WheelCorner: 0.33,
  /** A label inside a control, of its height. */
  InsideLabel: 0.4,
  /** A label under a control, of the label size. */
  UnderLabel: 0.85,
  /** The display's word, of its height. */
  DisplayLabel: 0.5,
} as const;

/** The fill of a control: lit, or its resting colour. */
function fillOf(isActive: boolean, rest = 'fill-card'): string {
  return isActive ? 'fill-primary' : rest;
}

/** Props every shape gets. */
interface ShapeProps {
  item: LayoutItem;
  isActive: boolean;
  value: number | undefined;
  name: string;
}

/**
 * @param props - The item and how it shows.
 * @returns A round control's body (knob, encoder, round button).
 */
function RoundShape({ item, isActive, value, shape }: ShapeProps & { shape: number }) {
  const r = Math.min(item.w, item.h) / 2;
  const cx = item.x + item.w / 2;
  const cy = item.y + item.h / 2;
  const hasPointer = shape === LayoutShape.Knob || shape === LayoutShape.Encoder;
  const sweep = shape === LayoutShape.Encoder ? DEG_PER_TURN : KNOB_SWEEP_DEG;
  const angle = ((value ?? MIDDLE) - MIDDLE) * sweep;
  const rad = (angle * Math.PI) / (DEG_PER_TURN / 2);
  const POINTER_SHARE = 0.8;
  return (
    <>
      <circle
        cx={cx}
        cy={cy}
        r={r * Share.Body}
        className={cn(fillOf(isActive), 'stroke-foreground/50')}
        strokeDasharray={shape === LayoutShape.Encoder ? '0.4 0.3' : undefined}
        vectorEffect="non-scaling-stroke"
      />
      {hasPointer ? (
        <line
          x1={cx}
          y1={cy}
          x2={cx + Math.sin(rad) * r * POINTER_SHARE}
          y2={cy - Math.cos(rad) * r * POINTER_SHARE}
          className={isActive ? 'stroke-primary-foreground' : 'stroke-foreground'}
          strokeWidth={2}
          strokeLinecap="round"
          vectorEffect="non-scaling-stroke"
        />
      ) : null}
    </>
  );
}

/**
 * @param props - The item and how it shows.
 * @returns A fader (or a wheel): a slot and a cap at the value, along the long side.
 */
function SlideShape({ item, isActive, value, isWheel }: ShapeProps & { isWheel: boolean }) {
  const isVertical = item.h >= item.w;
  const v = value ?? (isWheel ? MIDDLE : 0);
  const CAP = 0.18;
  const SLOT = 0.15;
  if (isWheel) {
    const bar = isVertical ? item.y + item.h * (1 - v) : item.x + item.w * v;
    return (
      <>
        <rect
          x={item.x + Share.WheelInset}
          y={item.y}
          width={item.w - 2 * Share.WheelInset}
          height={item.h}
          rx={Math.min(item.w, item.h) * Share.WheelCorner}
          className={cn(fillOf(isActive, 'fill-muted'), 'stroke-foreground/50')}
          vectorEffect="non-scaling-stroke"
        />
        {isVertical ? (
          <line
            x1={item.x + Share.WheelInset}
            x2={item.x + item.w - Share.WheelInset}
            y1={bar}
            y2={bar}
            className="stroke-foreground"
            strokeWidth={2}
            vectorEffect="non-scaling-stroke"
          />
        ) : (
          <line
            x1={bar}
            x2={bar}
            y1={item.y}
            y2={item.y + item.h}
            className="stroke-foreground"
            strokeWidth={2}
            vectorEffect="non-scaling-stroke"
          />
        )}
      </>
    );
  }
  if (isVertical) {
    const capH = item.h * CAP;
    const capY = item.y + (item.h - capH) * (1 - v);
    return (
      <>
        <rect
          x={item.x + item.w / 2 - (item.w * SLOT) / 2}
          y={item.y}
          width={item.w * SLOT}
          height={item.h}
          className="fill-muted-foreground/40"
        />
        <rect
          x={item.x}
          y={capY}
          width={item.w}
          height={capH}
          rx={CORNER}
          className={cn(fillOf(isActive), 'stroke-foreground/60')}
          vectorEffect="non-scaling-stroke"
        />
      </>
    );
  }
  const capW = item.w * CAP;
  const capX = item.x + (item.w - capW) * v;
  return (
    <>
      <rect
        x={item.x}
        y={item.y + item.h / 2 - (item.h * SLOT) / 2}
        width={item.w}
        height={item.h * SLOT}
        className="fill-muted-foreground/40"
      />
      <rect
        x={capX}
        y={item.y}
        width={capW}
        height={item.h}
        rx={CORNER}
        className={cn(fillOf(isActive), 'stroke-foreground/60')}
        vectorEffect="non-scaling-stroke"
      />
    </>
  );
}

/**
 * @param props - The item, the lit controls and the values.
 * @returns An XY pad with a dot where it is touched.
 */
function XyShape({ item, isActive, x, y }: ShapeProps & { x: number | undefined; y: number | undefined }) {
  const hasDot = x !== undefined && y !== undefined;
  const DOT = 0.45;
  return (
    <>
      <rect
        x={item.x}
        y={item.y}
        width={item.w}
        height={item.h}
        rx={CORNER}
        className={cn(fillOf(false, 'fill-muted'), isActive ? 'stroke-primary' : 'stroke-foreground/50')}
        strokeWidth={isActive ? 2 : 1}
        vectorEffect="non-scaling-stroke"
      />
      {hasDot ? (
        <circle cx={item.x + item.w * x} cy={item.y + item.h * (1 - y)} r={DOT} className="fill-primary" />
      ) : null}
    </>
  );
}

/**
 * @param props - The item and its name.
 * @returns Where the name goes: in the control when it is wide enough, else under it.
 */
function Label({ item, name, isActive, inside }: ShapeProps & { inside: boolean }) {
  const isInside = inside && item.w >= INSIDE_LABEL_MIN_W;
  const size = isInside ? Math.min(LABEL_UNITS, item.h * Share.InsideLabel) : LABEL_UNITS * Share.UnderLabel;
  return (
    <text
      x={item.x + item.w / 2}
      y={isInside ? item.y + item.h / 2 : item.y + item.h + size}
      fontSize={size}
      textAnchor="middle"
      dominantBaseline={isInside ? 'central' : 'auto'}
      className={cn(
        'pointer-events-none select-none',
        isInside && isActive ? 'fill-primary-foreground' : 'fill-foreground/80',
      )}
    >
      {name}
    </text>
  );
}

/**
 * @param props - The `keys` item, the lit keys.
 * @returns The keys, white ones first so the black ones sit on top.
 */
function KeysShape({
  item,
  active,
  onSelect,
}: {
  item: LayoutItem;
  active: ReadonlySet<string>;
  onSelect: ((control: ControlRef) => void) | undefined;
}) {
  return (
    <>
      {keyRects(item).map((key) => {
        const isActive = active.has(controlKey(ComponentKind.KeyMatrix, key.key));
        return (
          // biome-ignore lint/a11y/noStaticElementInteractions: the drawing is a pointer shortcut; the tables are the accessible way.
          <rect
            key={key.key}
            x={key.x}
            y={key.y}
            width={key.w}
            height={key.h}
            rx={0.15}
            className={cn(
              isActive ? 'fill-primary' : key.isBlack ? 'fill-foreground' : 'fill-card',
              'stroke-foreground/50',
            )}
            vectorEffect="non-scaling-stroke"
            onClick={
              onSelect === undefined ? undefined : () => onSelect({ kind: ComponentKind.KeyMatrix, index: key.key })
            }
          >
            <title>{`Key ${key.key + 1} (${noteName(key.note)})`}</title>
          </rect>
        );
      })}
    </>
  );
}

/**
 * @param props - One item and the drawing's state.
 * @returns The item drawn by its shape.
 */
function ItemShape({
  item,
  nameOf,
  active,
  values,
  onSelect,
}: {
  item: LayoutItem;
  nameOf: DeviceDrawingProps['nameOf'];
  active: ReadonlySet<string>;
  values: ReadonlyMap<string, number> | undefined;
  onSelect: DeviceDrawingProps['onSelect'];
}): ReactElement {
  if (item.shape === LayoutShape.Keys) {
    return <KeysShape item={item} active={active} onSelect={onSelect} />;
  }
  const key = controlKey(item.kind, item.index);
  const yKey = item.yIndex === undefined ? null : controlKey(item.kind, item.yIndex);
  const isActive = active.has(key) || (yKey !== null && active.has(yKey));
  const value = values?.get(key);
  const name = item.shape === LayoutShape.Display ? 'Display' : nameOf(item.kind, item.index);
  const props: ShapeProps = { item, isActive, value, name };
  let body: ReactElement;
  let inside = false;
  switch (item.shape) {
    case LayoutShape.RoundButton:
    case LayoutShape.Knob:
    case LayoutShape.Encoder:
      body = <RoundShape {...props} shape={item.shape} />;
      break;
    case LayoutShape.Fader:
    case LayoutShape.Wheel:
      body = <SlideShape {...props} isWheel={item.shape === LayoutShape.Wheel} />;
      break;
    case LayoutShape.Xy:
      body = <XyShape {...props} x={value} y={yKey === null ? undefined : values?.get(yKey)} />;
      inside = true;
      break;
    case LayoutShape.Pad: {
      const pressure = value ?? 0;
      body = (
        <>
          <rect
            x={item.x}
            y={item.y}
            width={item.w}
            height={item.h}
            rx={CORNER * 2}
            className={cn(fillOf(isActive, 'fill-muted'), 'stroke-foreground/50')}
            vectorEffect="non-scaling-stroke"
          />
          {pressure > 0 && isActive === false ? (
            <rect
              x={item.x}
              y={item.y}
              width={item.w}
              height={item.h}
              rx={CORNER * 2}
              className="fill-primary"
              opacity={pressure}
            />
          ) : null}
        </>
      );
      inside = true;
      break;
    }
    case LayoutShape.Display:
      body = (
        <rect
          x={item.x}
          y={item.y}
          width={item.w}
          height={item.h}
          rx={CORNER}
          className="fill-foreground/85 stroke-foreground"
          vectorEffect="non-scaling-stroke"
        />
      );
      break;
    default:
      body = (
        <rect
          x={item.x}
          y={item.y}
          width={item.w}
          height={item.h}
          rx={CORNER}
          className={cn(fillOf(isActive), 'stroke-foreground/50')}
          vectorEffect="non-scaling-stroke"
        />
      );
      inside = true;
  }
  const isDisplay = item.shape === LayoutShape.Display;
  return (
    // biome-ignore lint/a11y/noStaticElementInteractions: the drawing is a pointer shortcut; the tables are the accessible way.
    <g
      data-control={key}
      className={onSelect === undefined ? undefined : 'cursor-pointer'}
      onClick={onSelect === undefined ? undefined : () => onSelect({ kind: item.kind, index: item.index })}
    >
      <title>{name}</title>
      {body}
      {isDisplay ? (
        <text
          x={item.x + item.w / 2}
          y={item.y + item.h / 2}
          fontSize={Math.min(LABEL_UNITS, item.h * Share.DisplayLabel)}
          textAnchor="middle"
          dominantBaseline="central"
          className="pointer-events-none fill-background"
        >
          Display
        </text>
      ) : (
        <Label {...props} inside={inside} />
      )}
    </g>
  );
}

/**
 * The device drawn from its layout (§4.7): every item by its shape, the lit controls in the
 * primary colour, faders, knobs, wheels and pads at their values. Scales to the width it is given
 * and keeps the layout's proportions.
 *
 * @param props - The layout, names, lit controls and values.
 * @returns The drawing.
 */
export function DeviceDrawing({ layout, nameOf, active, values, onSelect, className }: DeviceDrawingProps) {
  const MARGIN = 1;
  return (
    <svg
      role="img"
      aria-label="The device's controls"
      viewBox={`${-MARGIN} ${-MARGIN} ${layout.width + 2 * MARGIN} ${layout.height + 2 * MARGIN}`}
      className={cn('h-auto w-full', className)}
    >
      <rect
        x={-MARGIN / 2}
        y={-MARGIN / 2}
        width={layout.width + MARGIN}
        height={layout.height + MARGIN}
        rx={1}
        className="fill-muted/40 stroke-border"
        vectorEffect="non-scaling-stroke"
      />
      {layout.items.map((item) => (
        <ItemShape
          key={`${item.kind}:${item.index}:${item.shape}`}
          item={item}
          nameOf={nameOf}
          active={active}
          values={values}
          onSelect={onSelect}
        />
      ))}
    </svg>
  );
}

/**
 * @param layout - The device's layout, or null.
 * @param controls - Every control the page knows of.
 * @returns The ones the layout doesn't place.
 */
export function unplacedControls(layout: DeviceLayout | null, controls: readonly ControlRef[]): ControlRef[] {
  if (layout === null) {
    return [...controls];
  }
  const placed = new Set<string>();
  for (const item of layout.items) {
    if (item.shape === LayoutShape.Keys) {
      continue;
    }
    placed.add(controlKey(item.kind, item.index));
    if (item.yIndex !== undefined) {
      placed.add(controlKey(item.kind, item.yIndex));
    }
  }
  return controls.filter((control) => placed.has(controlKey(control.kind, control.index)) === false);
}

/** Props of {@link ControlChips}. */
export interface ControlChipsProps {
  controls: readonly ControlRef[];
  nameOf: (kind: number, index: number) => string;
  active: ReadonlySet<string>;
}

/**
 * Controls as a plain row of names, lit like the drawing: for a device without a layout, and for
 * the controls its layout leaves out.
 *
 * @param props - The controls, their names, the lit ones.
 * @returns The chips.
 */
export function ControlChips({ controls, nameOf, active }: ControlChipsProps) {
  return (
    <ul className="flex flex-wrap gap-1.5">
      {controls.map(({ kind, index }) => {
        const key = controlKey(kind, index);
        return (
          <li key={key}>
            <Badge variant={active.has(key) ? 'default' : 'outline'}>{nameOf(kind, index)}</Badge>
          </li>
        );
      })}
    </ul>
  );
}

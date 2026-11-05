/**
 * Host feedback (`protocol/sysex.md` §4.6): GET_COMPONENT to learn a device's controls by kind
 * and index, and payload builders for the FB_* notifications that drive its LEDs, values and
 * displays. Framework-free: the caller supplies the client.
 */

import { RequestError, RequestFailure, type RequestOptions, type SysexClient } from './client.ts';
import { DecodeError, encodeU14, PayloadReader } from './encoding.ts';
import {
  Command,
  ComponentKind,
  DATA_MASK,
  DisplayType,
  EncoderMode,
  FB_LEDS_MAX,
  FB_TEXT_MAX,
  LedColours,
  LedOwner,
  NONE_INDEX,
  ReleaseWhat,
  RingMode,
  Status,
  Takeover,
} from './wire.ts';

/** What every component's description starts with (§4.6.1). */
interface ComponentBase {
  /** Its index within its kind. */
  index: number;
  /** How many components of this kind the device has. */
  count: number;
  /** The YAML `id:`. */
  id: string;
  /** The display name. */
  name: string;
  /** The YAML `daw:` value, else the `mcu:` name, else empty. */
  role: string;
}

/** A button: its LED, if it has one. */
export interface ButtonComponent extends ComponentBase {
  kind: typeof ComponentKind.Button;
  /** Its LED's index, or null for none. */
  led: number | null;
}

/** A key matrix: how many keys, from which note. */
export interface KeyMatrixComponent extends ComponentBase {
  kind: typeof ComponentKind.KeyMatrix;
  /** How many keys. */
  keys: number;
  /** The MIDI note of the first key. */
  firstNote: number;
}

/** An analog input: a pot, slider, fader or expression input. */
export interface AnalogComponent extends ComponentBase {
  kind: typeof ComponentKind.Analog;
  /** 0 none, 1 pick-up; null from firmware that doesn't report it. */
  takeover: number | null;
}

/** An encoder: its LED ring, if it has one. */
export interface EncoderComponent extends ComponentBase {
  kind: typeof ComponentKind.Encoder;
  /** LEDs in its ring; 0 = none. */
  ring: number;
  /** Its centre LED's index, or null for none. */
  center: number | null;
  /** 0 absolute, 1 and 2 relative, 3 Mackie V-Pot; null from firmware that doesn't report it. */
  mode: number | null;
}

/** An LED. */
export interface LedComponent extends ComponentBase {
  kind: typeof ComponentKind.Led;
  /** 1 single, 2 bicolour, 3 RGB. */
  colours: number;
  /** 0 host_then_local, 1 host, 2 local; null from firmware that doesn't report it. */
  owner: number | null;
}

/** A display. */
export interface DisplayComponent extends ComponentBase {
  kind: typeof ComponentKind.Display;
  /** Characters per row. */
  cols: number;
  /** Rows. */
  rows: number;
  /** 1 characters, 2 segments. */
  type: number;
}

/** A pad, or a pad twin (a mode's layer): its LED, if it has one. */
export interface PadComponent extends ComponentBase {
  kind: typeof ComponentKind.Pad;
  /** Its LED's index, or null for none. */
  led: number | null;
  /** 0 a pad; n a twin, live while mode n - 1 is on; null from firmware that doesn't report it. */
  layer: number | null;
}

/** One component as GET_COMPONENT describes it, by kind. */
export type Component =
  | ButtonComponent
  | KeyMatrixComponent
  | AnalogComponent
  | EncoderComponent
  | LedComponent
  | DisplayComponent
  | PadComponent;

/** Every kind, in the order the spec numbers them. */
export const COMPONENT_KINDS: readonly ComponentKind[] = Object.values(ComponentKind);

/** A reader's name for each kind, plural, for headings. */
export const COMPONENT_KIND_NAMES: Readonly<Record<ComponentKind, string>> = Object.freeze({
  [ComponentKind.Button]: 'Buttons',
  [ComponentKind.KeyMatrix]: 'Key matrices',
  [ComponentKind.Analog]: 'Analog inputs',
  [ComponentKind.Encoder]: 'Encoders',
  [ComponentKind.Led]: 'LEDs',
  [ComponentKind.Display]: 'Displays',
  [ComponentKind.Pad]: 'Pads',
});

/** A reader's name for each LED `colours`. */
export const LED_COLOURS_NAMES: Readonly<Record<number, string>> = Object.freeze({
  [LedColours.Single]: 'single colour',
  [LedColours.Bicolour]: 'bicolour',
  [LedColours.Rgb]: 'RGB',
});

/** A reader's name for each analog `takeover`. */
export const TAKEOVER_NAMES: Readonly<Record<number, string>> = Object.freeze({
  [Takeover.None]: 'no takeover',
  [Takeover.Pickup]: 'pick-up',
});

/** A reader's name for each encoder `mode`. */
export const ENCODER_MODE_NAMES: Readonly<Record<number, string>> = Object.freeze({
  [EncoderMode.Absolute]: 'absolute',
  [EncoderMode.RelativeTwos]: "relative (two's complement)",
  [EncoderMode.RelativeOffset]: 'relative (offset 64)',
  [EncoderMode.McuVpot]: 'Mackie V-Pot',
});

/** A reader's name for each LED `owner`. */
export const LED_OWNER_NAMES: Readonly<Record<number, string>> = Object.freeze({
  [LedOwner.HostThenLocal]: 'host, then local',
  [LedOwner.Host]: 'host',
  [LedOwner.Local]: 'local only',
});

/**
 * Whether FB_VALUE does anything to this component (§4.6.2): a pick-up analog input or an
 * absolute encoder. True when the firmware doesn't say, as before the fields were added.
 *
 * @param component - An analog input or an encoder.
 * @returns Whether to offer a value.
 */
export function takesValue(component: AnalogComponent | EncoderComponent): boolean {
  if (component.kind === ComponentKind.Analog) {
    return component.takeover === null || component.takeover === Takeover.Pickup;
  }
  return component.mode === null || component.mode === EncoderMode.Absolute;
}

/**
 * @param led - An LED.
 * @returns Whether FB_LED reaches it: every owner but local.
 */
export function hostDrives(led: LedComponent): boolean {
  return led.owner !== LedOwner.Local;
}

/** A reader's name for each display `type`. */
export const DISPLAY_TYPE_NAMES: Readonly<Record<number, string>> = Object.freeze({
  [DisplayType.Characters]: 'characters',
  [DisplayType.Segments]: 'segments',
});

/** A reader's name for each FB_RING `mode`. */
export const RING_MODE_NAMES: Readonly<Record<RingMode, string>> = Object.freeze({
  [RingMode.Dot]: 'Dot',
  [RingMode.BoostCut]: 'Boost/cut',
  [RingMode.Wrap]: 'Wrap',
  [RingMode.Spread]: 'Spread',
});

/** The largest u14: the largest index, and the largest value FB_VALUE and FB_RING carry. */
export const FEEDBACK_VALUE_MAX = 0x3fff;
/** The largest r, g or b in FB_LED_RGB. */
const RGB_MAX = DATA_MASK;
/** FB_LED_RGB `flags` bit 0: blink. */
const RGB_FLAG_BLINK = 1 << 0;
/** FB_RING `flags` bit 0: light the centre LED. */
const RING_FLAG_CENTRE = 1 << 0;
/** FB_RING `flags` bit 1: every ring LED off. */
const RING_FLAG_OFF = 1 << 1;

/**
 * @param index - A u14 index field.
 * @returns The index, or null for {@link NONE_INDEX}.
 */
function noneToNull(index: number): number | null {
  return index === NONE_INDEX ? null : index;
}

/**
 * Throws unless `value` is an integer in `min..max`.
 *
 * @param what - The field's name, for the message.
 * @param value - The value.
 * @param min - The smallest allowed.
 * @param max - The largest allowed.
 */
function checkRange(what: string, value: number, min: number, max: number): void {
  const isInRange = Number.isInteger(value) && value >= min && value <= max;
  if (isInRange === false) {
    throw new RangeError(`${what} ${value} is not in ${min}..${max}`);
  }
}

/**
 * The GET_COMPONENT request payload: `kind` (1 byte), `index` u14.
 *
 * @param kind - 1 button, 2 key matrix, 3 analog, 4 encoder, 5 LED, 6 display, 7 pad.
 * @param index - Which one of that kind.
 * @returns The payload.
 */
export function getComponentPayload(kind: ComponentKind, index: number): number[] {
  checkRange('kind', kind, 0, DATA_MASK);
  return [kind, ...encodeU14(index)];
}

/**
 * Reads an added field (§4.6.1 "Added fields"): one byte, or null when the firmware ends before it.
 *
 * @param reader - The reply, at the field.
 * @returns The byte, or null.
 */
function optionalByte(reader: PayloadReader): number | null {
  return reader.remaining > 0 ? reader.byte() : null;
}

/**
 * Parses GET_COMPONENT's reply fields (§4.6.1): `kind`, `index` u14, `count` u14, `id`, `name`
 * and `role` lstrings, then the kind's extra fields. The added fields (`takeover`, `mode`,
 * `owner`, a pad's `layer`) are null when older firmware leaves them out. Bytes after those are later fields, ignored.
 *
 * @param body - The reply payload without its status byte.
 * @returns The component.
 */
export function parseComponent(body: ArrayLike<number>): Component {
  const reader = new PayloadReader(body);
  const kind = reader.byte();
  const base: ComponentBase = {
    index: reader.u14(),
    count: reader.u14(),
    id: reader.lstring(),
    name: reader.lstring(),
    role: reader.lstring(),
  };
  switch (kind) {
    case ComponentKind.Button:
      return { kind, ...base, led: noneToNull(reader.u14()) };
    case ComponentKind.KeyMatrix:
      return { kind, ...base, keys: reader.u14(), firstNote: reader.byte() };
    case ComponentKind.Analog:
      return { kind, ...base, takeover: optionalByte(reader) };
    case ComponentKind.Encoder: {
      const ring = reader.byte();
      const center = noneToNull(reader.u14());
      return { kind, ...base, ring, center, mode: optionalByte(reader) };
    }
    case ComponentKind.Led: {
      const colours = reader.byte();
      return { kind, ...base, colours, owner: optionalByte(reader) };
    }
    case ComponentKind.Display:
      return { kind, ...base, cols: reader.byte(), rows: reader.byte(), type: reader.byte() };
    case ComponentKind.Pad: {
      const led = noneToNull(reader.u14());
      return { kind, ...base, led, layer: optionalByte(reader) };
    }
    default:
      throw new DecodeError(`unknown component kind ${kind}`);
  }
}

/**
 * Asks for one component's description.
 *
 * @param client - The device, which has HELLO caps bit 6.
 * @param kind - The kind.
 * @param index - Which one of that kind.
 * @param options - An abort signal.
 * @returns The component.
 */
export async function getComponent(
  client: SysexClient,
  kind: ComponentKind,
  index: number,
  options: RequestOptions = {},
): Promise<Component> {
  const reply = await client.request(Command.GetComponent, getComponentPayload(kind, index), options);
  let component: Component;
  try {
    component = parseComponent(reply);
  } catch (error) {
    throw new RequestError(RequestFailure.BadReply, `GET_COMPONENT's reply could not be read: ${String(error)}`);
  }
  const isEcho = component.kind === kind && component.index === index;
  if (isEcho === false) {
    throw new RequestError(
      RequestFailure.BadReply,
      `GET_COMPONENT for kind ${kind} index ${index} answered kind ${component.kind} index ${component.index}.`,
    );
  }
  return component;
}

/**
 * Reads every component: index 0 of each kind gives that kind's `count`, then the rest in order.
 * BAD_PARAM at index 0 means the device has none of that kind.
 *
 * @param client - The device, which has HELLO caps bit 6.
 * @param options - An abort signal.
 * @returns The components, by kind and then index.
 */
export async function listComponents(client: SysexClient, options: RequestOptions = {}): Promise<Component[]> {
  const all: Component[] = [];
  for (const kind of COMPONENT_KINDS) {
    let first: Component;
    try {
      first = await getComponent(client, kind, 0, options);
    } catch (error) {
      const isNone = error instanceof RequestError && error.status === Status.BadParam;
      if (isNone) {
        continue;
      }
      throw error;
    }
    all.push(first);
    for (let index = 1; index < first.count; index++) {
      all.push(await getComponent(client, kind, index, options));
    }
  }
  return all;
}

/**
 * The FB_LED payload: `led` u14, `level`.
 *
 * @param led - The LED's index.
 * @param level - 0 off, 1 blink, 2..127 on (on a bicolour LED, 2..63 colour 1, 64..127 colour 2).
 * @returns The payload.
 */
export function fbLedPayload(led: number, level: number): number[] {
  checkRange('level', level, 0, DATA_MASK);
  return [...encodeU14(led), level];
}

/** An RGB colour, each part 0..127. */
export interface Rgb {
  r: number;
  g: number;
  b: number;
}

/**
 * The FB_LED_RGB payload: `led` u14, `r`, `g`, `b`, `flags` (bit 0 blink).
 *
 * @param led - The LED's index.
 * @param colour - Each part 0..127.
 * @param blink - Whether it blinks.
 * @returns The payload.
 */
export function fbLedRgbPayload(led: number, { r, g, b }: Rgb, blink = false): number[] {
  checkRange('r', r, 0, RGB_MAX);
  checkRange('g', g, 0, RGB_MAX);
  checkRange('b', b, 0, RGB_MAX);
  return [...encodeU14(led), r, g, b, blink ? RGB_FLAG_BLINK : 0];
}

/**
 * The FB_LEDS payload: `first` u14, `n` (1..64), then `n` levels.
 *
 * @param first - The first LED's index.
 * @param levels - One level per LED from `first` on, 1..64 of them.
 * @returns The payload.
 */
export function fbLedsPayload(first: number, levels: readonly number[]): number[] {
  checkRange('LED count', levels.length, 1, FB_LEDS_MAX);
  checkRange('last LED', first + levels.length - 1, 0, FEEDBACK_VALUE_MAX);
  for (const level of levels) {
    checkRange('level', level, 0, DATA_MASK);
  }
  return [...encodeU14(first), levels.length, ...levels];
}

/** The kinds FB_VALUE addresses. */
export type ValueKind = typeof ComponentKind.Analog | typeof ComponentKind.Encoder;

/**
 * The FB_VALUE payload: `kind` (3 analog, 4 encoder), `index` u14, `value` u14.
 *
 * @param kind - Analog or encoder.
 * @param index - Which one of that kind.
 * @param value - 0..16383.
 * @returns The payload.
 */
export function fbValuePayload(kind: ValueKind, index: number, value: number): number[] {
  const isValueKind = kind === ComponentKind.Analog || kind === ComponentKind.Encoder;
  if (isValueKind === false) {
    throw new RangeError(`FB_VALUE kind ${kind} is not 3 (analog) or 4 (encoder)`);
  }
  checkRange('value', value, 0, FEEDBACK_VALUE_MAX);
  return [kind, ...encodeU14(index), ...encodeU14(value)];
}

/**
 * The FB_RING payload: `encoder` u14, `mode`, `value` u14, `flags` (bit 0 centre LED).
 *
 * @param encoder - The encoder's index.
 * @param mode - 0 dot, 1 boost/cut, 2 wrap, 3 spread.
 * @param value - 0..16383.
 * @param centre - Whether to light the centre LED.
 * @returns The payload.
 */
export function fbRingPayload(encoder: number, mode: RingMode, value: number, centre = false, off = false): number[] {
  checkRange('mode', mode, RingMode.Dot, RingMode.Spread);
  checkRange('value', value, 0, FEEDBACK_VALUE_MAX);
  const flags = (centre ? RING_FLAG_CENTRE : 0) | (off ? RING_FLAG_OFF : 0);
  return [...encodeU14(encoder), mode, ...encodeU14(value), flags];
}

/**
 * The FB_TEXT payload: `display` u14, `row`, `col`, then the text.
 *
 * @param display - The display's index.
 * @param row - The first row, 0..127.
 * @param col - The first column, 0..127.
 * @param text - 7-bit ASCII, at most 64 characters.
 * @returns The payload.
 */
export function fbTextPayload(display: number, row: number, col: number, text: string): number[] {
  checkRange('row', row, 0, DATA_MASK);
  checkRange('col', col, 0, DATA_MASK);
  checkRange('text length', text.length, 0, FB_TEXT_MAX);
  const codes = Array.from(text, (c) => c.charCodeAt(0));
  const isAscii = codes.every((code) => code <= DATA_MASK);
  if (isAscii === false) {
    throw new RangeError(`"${text}" is not all 7-bit ASCII`);
  }
  return [...encodeU14(display), row, col, ...codes];
}

/**
 * The FB_RELEASE payload: `what`.
 *
 * @param what - 0 everything, 1 LEDs, 2 values, 3 text.
 * @returns The payload.
 */
export function fbReleasePayload(what: ReleaseWhat): number[] {
  checkRange('what', what, ReleaseWhat.Everything, ReleaseWhat.Text);
  return [what];
}

/**
 * The ?demo device's controls, for the input tester and MIDI learn: a Code 61-like layout (§4.7),
 * GET_COMPONENT descriptions with the adc ranges, and a pretend player who moves a control every
 * second or so. While MONITOR runs it streams RAW (§4.8, stopping 5 s after the last MONITOR);
 * while `debug.events` is 1 it sends the EVENTs the same moves would. Never in a production build.
 */

import { encodeLstring, encodeU14, encodeU28 } from '../protocol/encoding.ts';
import { encodeFrame } from '../protocol/frame.ts';
import type { DeviceLayout, LayoutItem } from '../protocol/layout.ts';
import type { RawReading } from '../protocol/monitor.ts';
import { encodeLayout, encodeRaw } from '../protocol/test-support.ts';
import { ComponentKind, EventType, LayoutShape, MonitorKind, NONE_INDEX, Unsolicited } from '../protocol/wire.ts';

/** The demo's buttons, in index order. */
export const DEMO_BUTTONS = [
  'Oct -',
  'Oct +',
  'Sustain',
  'Shift',
  'Rewind',
  'Fast forward',
  'Stop',
  'Play',
  'Record',
  ...Array.from({ length: 9 }, (_, i) => `B${i + 1}`),
];
/** The demo's analog inputs: wheels, faders, the master and the XY pad's two axes. */
export const DEMO_ANALOGS = [
  'Pitch',
  'Mod',
  ...Array.from({ length: 8 }, (_, i) => `Fader ${i + 1}`),
  'Master',
  'XY X',
  'XY Y',
];
export const DEMO_ENCODERS = Array.from({ length: 8 }, (_, i) => `E${i + 1}`);
export const DEMO_PADS = Array.from({ length: 16 }, (_, i) => `Pad ${i + 1}`);
export const DEMO_KEYS = 61;
export const DEMO_FIRST_NOTE = 36;

const PITCH = 0;
const MOD = 1;
const FADER_1 = 2;
const MASTER = 10;
const XY_X = 11;
const XY_Y = 12;
const B1 = 9;
/** The YAML ranges (adc_min, adc_max) of each analog input, as the real Code 61's. */
const ADC_RANGES: readonly (readonly [number, number])[] = DEMO_ANALOGS.map((_, i) => {
  if (i === PITCH || i === MOD) {
    return [1420, 2360] as const;
  }
  if (i === XY_X || i === XY_Y) {
    return [830, 3100] as const;
  }
  return [80, 4000] as const;
});
/** The YAML ids, as GET_COMPONENT gives them. */
const ANALOG_IDS = [
  'pitch_wheel',
  'mod_wheel',
  ...Array.from({ length: 8 }, (_, i) => `fader_${i + 1}`),
  'fader_9',
  'xy_x',
  'xy_y',
];

/** @returns The Code 61, roughly: XY pad, display and transport left, pads, encoders over faders, keys. */
function demoLayout(): DeviceLayout {
  const items: LayoutItem[] = [];
  const add = (kind: number, index: number, shape: number, x: number, y: number, w: number, h: number) => {
    items.push({ kind, index, shape, x, y, w, h });
  };
  items.push({
    kind: ComponentKind.Analog,
    index: XY_X,
    shape: LayoutShape.Xy,
    x: 2,
    y: 3,
    w: 16,
    h: 16,
    yIndex: XY_Y,
  });
  add(ComponentKind.Button, 0, LayoutShape.Button, 2, 23, 7, 3);
  add(ComponentKind.Button, 1, LayoutShape.Button, 11, 23, 7, 3);
  add(ComponentKind.Button, 3, LayoutShape.Button, 2, 29, 7, 3);
  add(ComponentKind.Display, 0, LayoutShape.Display, 24, 3, 24, 7);
  for (let i = 0; i < 5; i++) {
    add(ComponentKind.Button, 4 + i, LayoutShape.Button, 24 + i * 5, 14, 4, 3);
  }
  for (let i = 0; i < 16; i++) {
    const col = i % 4;
    const rowFromBottom = Math.floor(i / 4);
    add(ComponentKind.Pad, i, LayoutShape.Pad, 54 + col * 8, 3 + (3 - rowFromBottom) * 8, 7, 7);
  }
  for (let i = 0; i < 8; i++) {
    add(ComponentKind.Encoder, i, LayoutShape.Encoder, 95 + i * 9, 2, 5, 5);
    add(ComponentKind.Analog, FADER_1 + i, LayoutShape.Fader, 96 + i * 9, 10, 3, 17);
    add(ComponentKind.Button, B1 + 1 + i, LayoutShape.Button, 95 + i * 9, 30, 5, 3);
  }
  add(ComponentKind.Analog, MASTER, LayoutShape.Fader, 96 + 8 * 9, 10, 3, 17);
  add(ComponentKind.Button, B1, LayoutShape.Button, 95 + 8 * 9, 30, 5, 3);
  add(ComponentKind.Analog, PITCH, LayoutShape.Wheel, 4, 44, 5, 18);
  add(ComponentKind.Analog, MOD, LayoutShape.Wheel, 11, 44, 5, 18);
  items.push({
    kind: ComponentKind.KeyMatrix,
    index: 0,
    shape: LayoutShape.Keys,
    x: 20,
    y: 38,
    w: 156,
    h: 28,
    keys: { count: DEMO_KEYS, firstNote: DEMO_FIRST_NOTE },
  });
  return { width: 178, height: 68, items };
}

/** Matrix rows and columns: two contacts per key on rows 2p and 2p+1, as the Code 61's keybed. */
const MATRIX_ROWS = 16;
const MATRIX_COLS = 8;
/** How often a RAW message goes, as the firmware (§4.8). */
const RAW_INTERVAL_MS = 20;
/** The stream stops this long after the last MONITOR (§4.8). */
const MONITOR_TIMEOUT_MS = 5000;
/** A new move starts this often. */
const MOVE_EVERY_MS = 1300;
const ADC_TOP = 4095;
const EVENT_TOP = 0x3fff;

/** What the pretend player is doing. */
type Move =
  | { kind: 'sweep'; analog: number; from: number; to: number }
  | { kind: 'button'; index: number }
  | { kind: 'encoder'; index: number; step: number }
  | { kind: 'pad'; index: number }
  | { kind: 'key'; index: number }
  | { kind: 'xy' };

/** The demo's controls and the RAW / EVENT messages they send. */
export class DemoTools {
  readonly layoutBytes = encodeLayout(demoLayout());
  readonly #emit: (bytes: Uint8Array) => void;
  readonly #isEventsOn: () => boolean;
  #monitorKinds = 0;
  #monitorUntil = 0;
  #timer: ReturnType<typeof setInterval> | null = null;
  #rawTurn = 0;
  #t = 0;
  #moveAt = 0;
  #moveN = 0;
  #move: Move | null = null;
  readonly #analog: number[] = ADC_RANGES.map(([low, high], i) =>
    i === PITCH ? Math.round((low + high) / 2) : i === XY_X || i === XY_Y ? -1 : low + 200,
  );
  readonly #analogSent: number[] = this.#analog.map(() => -1);
  readonly #buttons: boolean[] = DEMO_BUTTONS.map(() => false);
  readonly #encoders: number[] = DEMO_ENCODERS.map((_, i) => i * 100);
  readonly #pads: number[] = DEMO_PADS.map(() => 0);
  #key: number | null = null;
  #keyContacts = 0;

  /**
   * @param emit - Sends a message to the page.
   * @param isEventsOn - Whether `debug.events` is 1 now.
   */
  constructor(emit: (bytes: Uint8Array) => void, isEventsOn: () => boolean) {
    this.#emit = emit;
    this.#isEventsOn = isEventsOn;
  }

  /**
   * MONITOR (§4.8): starts, renews or stops the stream.
   *
   * @param kinds - The kinds asked for.
   * @returns The reply fields.
   */
  monitor(kinds: number): number[] {
    const has =
      MonitorKind.Buttons | MonitorKind.KeyMatrix | MonitorKind.Analog | MonitorKind.Encoders | MonitorKind.Pads;
    this.#monitorKinds = kinds & has;
    this.#monitorUntil = Date.now() + MONITOR_TIMEOUT_MS;
    this.wake();
    return [this.#monitorKinds, ...encodeU14(RAW_INTERVAL_MS), ...encodeU14(MONITOR_TIMEOUT_MS)];
  }

  /** Starts the pretend player if anything listens (called after MONITOR and after a SET of debug.events). */
  wake(): void {
    if (this.#timer === null && this.#isRunning()) {
      this.#timer = setInterval(() => this.#tick(), RAW_INTERVAL_MS);
    }
  }

  /** @returns Whether the stream or the EVENTs want the player. */
  #isRunning(): boolean {
    const isMonitoring = this.#monitorKinds !== 0 && Date.now() < this.#monitorUntil;
    return isMonitoring || this.#isEventsOn();
  }

  /**
   * GET_COMPONENT for the controls (§4.6.1).
   *
   * @param kind - The kind.
   * @param index - The index.
   * @returns The reply fields, or null when there is no such component.
   */
  component(kind: number, index: number): number[] | null {
    const head = (count: number, id: string, name: string) => [
      kind,
      ...encodeU14(index),
      ...encodeU14(count),
      ...encodeLstring(id),
      ...encodeLstring(name),
      ...encodeLstring(''),
    ];
    const slug = (name: string) => name.toLowerCase().replace(/[^a-z0-9]+/g, '_');
    if (kind === ComponentKind.Button && index < DEMO_BUTTONS.length) {
      const name = DEMO_BUTTONS[index] ?? '';
      return [...head(DEMO_BUTTONS.length, slug(name), name), ...encodeU14(NONE_INDEX), 0];
    }
    if (kind === ComponentKind.KeyMatrix && index === 0) {
      return [...head(1, 'keys', 'Keys'), ...encodeU14(DEMO_KEYS), DEMO_FIRST_NOTE];
    }
    const range = ADC_RANGES[index];
    if (kind === ComponentKind.Analog && range !== undefined) {
      const [low, high] = range;
      const name = DEMO_ANALOGS[index] ?? '';
      return [
        ...head(DEMO_ANALOGS.length, ANALOG_IDS[index] ?? slug(name), name),
        0,
        0,
        ...encodeU14(low),
        ...encodeU14(high),
      ];
    }
    if (kind === ComponentKind.Encoder && index < DEMO_ENCODERS.length) {
      return [
        ...head(DEMO_ENCODERS.length, `encoder_${index + 1}`, DEMO_ENCODERS[index] ?? ''),
        0,
        ...encodeU14(NONE_INDEX),
        0,
        0,
      ];
    }
    if (kind === ComponentKind.Pad && index < DEMO_PADS.length) {
      return [...head(DEMO_PADS.length, `pad_${index + 1}`, DEMO_PADS[index] ?? ''), ...encodeU14(NONE_INDEX), 0];
    }
    return null;
  }

  /** One 20 ms step: the current move, a RAW message, EVENTs. */
  #tick(): void {
    if (this.#isRunning() === false) {
      if (this.#timer !== null) {
        clearInterval(this.#timer);
        this.#timer = null;
      }
      this.#monitorKinds = 0;
      return;
    }
    this.#t += RAW_INTERVAL_MS;
    if (this.#t >= this.#moveAt) {
      this.#endMove();
      this.#startMove();
      this.#moveAt = this.#t + MOVE_EVERY_MS;
    }
    this.#play((MOVE_EVERY_MS - (this.#moveAt - this.#t)) / MOVE_EVERY_MS);
    this.#sendAnalogEvents();
    if (this.#monitorKinds !== 0 && Date.now() < this.#monitorUntil) {
      this.#sendRaw();
    }
  }

  #startMove(): void {
    this.#moveN += 1;
    const n = this.#moveN;
    const pick = (list: readonly unknown[]) => (n * 7) % list.length;
    switch (n % 6) {
      case 0: {
        const analog = n % 12 === 0 ? MOD : FADER_1 + (pick(DEMO_ENCODERS) % 9);
        const [low, high] = ADC_RANGES[analog] ?? [0, ADC_TOP];
        const up = (this.#analog[analog] ?? low) < (low + high) / 2;
        this.#move = { kind: 'sweep', analog, from: this.#analog[analog] ?? low, to: up ? high + 40 : low - 40 };
        return;
      }
      case 1:
        this.#move = { kind: 'button', index: pick(DEMO_BUTTONS) };
        this.#setButton(this.#move.index, true);
        return;
      case 2:
        this.#move = { kind: 'encoder', index: pick(DEMO_ENCODERS), step: n % 4 === 0 ? -1 : 1 };
        return;
      case 3:
        this.#move = { kind: 'pad', index: pick(DEMO_PADS) };
        return;
      case 4:
        this.#move = { kind: 'key', index: (n * 11) % DEMO_KEYS };
        return;
      default:
        this.#move = { kind: 'xy' };
    }
  }

  #endMove(): void {
    const move = this.#move;
    if (move?.kind === 'button') {
      this.#setButton(move.index, false);
    }
    if (move?.kind === 'pad') {
      this.#pads[move.index] = 0;
    }
    if (move?.kind === 'key') {
      this.#setKey(move.index, 0);
      this.#key = null;
    }
    if (move?.kind === 'xy') {
      this.#analog[XY_X] = -1;
      this.#analog[XY_Y] = -1;
    }
    this.#move = null;
  }

  /**
   * @param done - How far through the move, 0..1.
   */
  #play(done: number): void {
    const move = this.#move;
    if (move === null) {
      return;
    }
    const noise = () => Math.round((Math.random() - 0.5) * 6);
    switch (move.kind) {
      case 'sweep': {
        const share = Math.min(1, done * 1.4);
        this.#analog[move.analog] = Math.max(
          0,
          Math.min(ADC_TOP, Math.round(move.from + (move.to - move.from) * share) + noise()),
        );
        return;
      }
      case 'encoder': {
        const STEPS = 12;
        const due = Math.floor(done * STEPS);
        const turned = this.#encoderTurned(move);
        if (due > turned) {
          this.#encoders[move.index] = ((this.#encoders[move.index] ?? 0) + move.step + 0x4000) % 0x4000;
          this.#event(EventType.Encoder, move.index, move.step < 0 ? 0x4000 + move.step : move.step);
          this.#turned.set(move, turned + 1);
        }
        return;
      }
      case 'pad': {
        const HIT_SHARE = 0.25;
        const PEAK = 3200;
        this.#pads[move.index] = done < HIT_SHARE ? Math.round(PEAK * Math.sin((done / HIT_SHARE) * Math.PI)) : 0;
        return;
      }
      case 'key': {
        const FIRST_CONTACT = 0.05;
        const BOTH = 0.08;
        const RELEASE = 0.6;
        const contacts = done < FIRST_CONTACT ? 0 : done < BOTH ? 1 : done < RELEASE ? 3 : 0;
        if (contacts !== this.#keyContacts) {
          const VELOCITY = 96;
          if (contacts === 3) {
            this.#event(EventType.Key, move.index, VELOCITY);
          } else if (contacts === 0 && this.#keyContacts === 3) {
            this.#event(EventType.Key, move.index, 0);
          }
          this.#setKey(move.index, contacts);
        }
        return;
      }
      case 'xy': {
        const [low, high] = ADC_RANGES[XY_X] ?? [0, ADC_TOP];
        const mid = (low + high) / 2;
        const radius = (high - low) / 3;
        const angle = done * 2 * Math.PI;
        this.#analog[XY_X] = Math.round(mid + radius * Math.cos(angle));
        this.#analog[XY_Y] = Math.round(mid + radius * Math.sin(angle));
        return;
      }
      default:
    }
  }

  readonly #turned = new WeakMap<Move, number>();

  /** @returns Detents this encoder move has turned so far. */
  #encoderTurned(move: Move): number {
    return this.#turned.get(move) ?? 0;
  }

  #setButton(index: number, down: boolean): void {
    this.#buttons[index] = down;
    this.#event(EventType.Button, index, down ? 1 : 0);
  }

  #setKey(index: number, contacts: number): void {
    this.#key = index;
    this.#keyContacts = contacts;
  }

  /** EVENT 3 when an analog input's 7-bit value changes, as the firmware does. */
  #sendAnalogEvents(): void {
    this.#analog.forEach((raw, i) => {
      if (raw < 0) {
        return;
      }
      const [low, high] = ADC_RANGES[i] ?? [0, ADC_TOP];
      const position = Math.max(0, Math.min(1, (raw - low) / (high - low)));
      const SEVEN_BIT = 127;
      const q = Math.round(position * SEVEN_BIT);
      if (q !== this.#analogSent[i]) {
        const isFirst = this.#analogSent[i] === -1;
        this.#analogSent[i] = q;
        if (isFirst === false) {
          this.#event(EventType.Analog, i, Math.round(position * EVENT_TOP));
        }
      }
    });
  }

  /**
   * @param type - EVENT type.
   * @param index - Its index.
   * @param value - Its value.
   */
  #event(type: number, index: number, value: number): void {
    if (this.#isEventsOn() === false) {
      return;
    }
    const U28 = 0x0fff_ffff;
    this.#emit(
      encodeFrame(Unsolicited.Event, 0, [type, ...encodeU14(index), ...encodeU14(value), ...encodeU28(this.#t & U28)]),
    );
  }

  /** One RAW message, the asked-for kinds in turn. */
  #sendRaw(): void {
    const kinds = [
      MonitorKind.Buttons,
      MonitorKind.KeyMatrix,
      MonitorKind.Analog,
      MonitorKind.Encoders,
      MonitorKind.Pads,
    ].filter((bit) => (this.#monitorKinds & bit) !== 0);
    const bit = kinds[this.#rawTurn % kinds.length];
    this.#rawTurn += 1;
    let reading: RawReading;
    switch (bit) {
      case MonitorKind.Buttons:
        reading = { kind: 'buttons', first: 0, pressed: [...this.#buttons] };
        break;
      case MonitorKind.KeyMatrix: {
        const closed = Array.from({ length: MATRIX_ROWS * MATRIX_COLS }, () => false);
        if (this.#key !== null && this.#keyContacts !== 0) {
          const pair = this.#key % (MATRIX_ROWS / 2);
          const col = Math.floor(this.#key / (MATRIX_ROWS / 2)) % MATRIX_COLS;
          closed[2 * pair * MATRIX_COLS + col] = true;
          if (this.#keyContacts === 3) {
            closed[(2 * pair + 1) * MATRIX_COLS + col] = true;
          }
        }
        reading = { kind: 'matrix', row: 0, rows: MATRIX_ROWS, cols: MATRIX_COLS, closed };
        break;
      }
      case MonitorKind.Analog:
        reading = { kind: 'analog', first: 0, values: this.#analog.map((v) => (v < 0 ? null : v)) };
        break;
      case MonitorKind.Encoders:
        reading = { kind: 'encoders', first: 0, counts: [...this.#encoders] };
        break;
      case MonitorKind.Pads:
        reading = { kind: 'pads', first: 0, values: [...this.#pads] };
        break;
      default:
        return;
    }
    this.#emit(encodeFrame(Unsolicited.Raw, 0, encodeRaw(reading)));
  }
}

/**
 * A pretend Code 61 for `npm run dev` with `?demo` in the URL: HELLO, the config (§4.3) and the
 * presets (§4.5), in memory, so the Presets view can be worked on without a keyboard. It is never
 * part of a production build (see `useDemoClient`).
 */

import { SysexClient, type Transport } from '../protocol/client.ts';
import {
  encodeLstring,
  encodeS32,
  encodeU14,
  encodeU28,
  encodeU32,
  PayloadReader,
  pack,
} from '../protocol/encoding.ts';
import { decodeFrame, encodeFrame } from '../protocol/frame.ts';
import type { ParamSpec } from '../protocol/schema.ts';
import { encodeSchema, makeParam } from '../protocol/test-support.ts';
import {
  Capability,
  Command,
  ComponentKind,
  LedColours,
  LedOwner,
  NOTIFICATION_FIRST,
  NOTIFICATION_LAST,
  ParamFlag,
  ParamType,
  PresetFlag,
  PresetParam,
  Role,
  Status,
  Unsolicited,
} from '../protocol/wire.ts';
import { withDemoMidi } from './demo-midi.ts';
import { DEMO_ANALOGS, DEMO_BUTTONS, DEMO_ENCODERS, DEMO_PADS, DemoTools } from './demo-tools.ts';

const P = ParamFlag.Preset;
const BUTTONS = DEMO_BUTTONS;
const ANALOGS = DEMO_ANALOGS;
const PADS = DEMO_PADS;
/** Button actions: octave down / up, sustain as CC momentary, the rest CC momentary or toggle (B1-B9). */
const BUTTON_ACTIONS = BUTTONS.map((name, i) =>
  i === 0 ? 5 : i === 1 ? 6 : i === 3 ? 0 : name.startsWith('B') ? 2 : 1,
);
const BUTTON_NUMBERS = BUTTONS.map((_, i) => (i < 2 ? 0 : i === 2 ? 64 : i === 3 ? 0 : i < 9 ? 106 + i : 76 + i));
const ANALOG_CCS = [0, 1, 20, 21, 22, 23, 24, 25, 26, 27, 7, 16, 17];

/** Host feedback (§4.6): the Code 61's LEDs, roughly, and its 6-character display. */
const DEMO_LEDS: readonly { id: string; name: string; colours: number }[] = [
  ...['Channel', 'Edit', 'Zone', '1', '2', '3', '4'].map((name) => ({
    id: `${name.toLowerCase()}_led`,
    name,
    colours: LedColours.Single,
  })),
  ...Array.from({ length: 9 }, (_, i) => ({ id: `b${i + 1}_led`, name: `B${i + 1}`, colours: LedColours.Rgb })),
  ...Array.from({ length: 16 }, (_, i) => ({ id: `pad${i + 1}_led`, name: `Pad ${i + 1}`, colours: LedColours.Rgb })),
];
const DEMO_DISPLAY_COLS = 6;

/** Roughly the Code 61's groups and params, with the per-preset ones flagged. */
const GROUPS = ['MIDI', 'Velocity', 'Buttons', 'Analog', 'Encoders', 'Pads', 'Presets', 'Debug'];
const PARAMS: ParamSpec[] = [
  makeParam({ id: 1, name: 'midi.channel', label: 'MIDI channel', group: 0, min: 1, max: 16, defaults: [1], flags: P }),
  makeParam({
    id: 2,
    name: 'midi.transpose',
    label: 'Transpose',
    group: 0,
    min: -24,
    max: 24,
    defaults: [0],
    flags: P,
  }),
  makeParam({ id: 3, name: 'midi.octave', label: 'Octave', group: 0, min: -4, max: 4, defaults: [0], flags: P }),
  makeParam({ id: 30, name: 'midi.thru', label: 'MIDI thru', group: 0, type: ParamType.Bool, max: 1, defaults: [1] }),
  makeParam({
    id: 4,
    name: 'velocity.curve',
    label: 'Velocity curve',
    group: 1,
    type: ParamType.Enum,
    max: 3,
    options: ['Normal', 'Soft', 'Hard', 'Fixed'],
    flags: P,
  }),
  makeParam({ id: 5, name: 'velocity.fixed', label: 'Fixed velocity', group: 1, min: 1, defaults: [100], flags: P }),
  makeParam({ id: 6, name: 'velocity.fast', label: 'Fastest hit', group: 1, max: 5000, defaults: [400], unit: 'µs' }),
  makeParam({
    id: 7,
    name: 'velocity.slow',
    label: 'Slowest hit',
    group: 1,
    max: 60000,
    defaults: [30000],
    unit: 'µs',
  }),
  makeParam({
    id: 9,
    name: 'button.action',
    label: 'Action',
    group: 2,
    type: ParamType.Enum,
    count: BUTTONS.length,
    max: 6,
    defaults: BUTTON_ACTIONS,
    indexLabels: BUTTONS,
    options: ['None', 'CC momentary', 'CC toggle', 'CC ramp', 'Note', 'Octave down', 'Octave up'],
    flags: P,
  }),
  makeParam({
    id: 10,
    name: 'button.number',
    label: 'Number',
    group: 2,
    count: BUTTONS.length,
    defaults: BUTTON_NUMBERS,
    indexLabels: BUTTONS,
    flags: P,
  }),
  makeParam({
    id: 8,
    name: 'analog.cc',
    label: 'CC',
    group: 3,
    count: ANALOGS.length,
    defaults: ANALOG_CCS,
    indexLabels: ANALOGS,
    flags: P,
  }),
  makeParam({
    id: 29,
    name: 'analog.mode',
    label: 'Sends',
    group: 3,
    type: ParamType.Enum,
    count: ANALOGS.length,
    max: 2,
    defaults: ANALOGS.map((_, i) => (i === 0 ? 1 : 0)),
    indexLabels: ANALOGS,
    options: ['CC', 'Pitch bend', 'Channel pressure'],
    flags: P,
  }),
  makeParam({
    id: 15,
    name: 'encoder.cc',
    label: 'CC',
    group: 4,
    count: DEMO_ENCODERS.length,
    defaults: DEMO_ENCODERS.map((_, i) => 70 + i),
    indexLabels: DEMO_ENCODERS,
    flags: P,
  }),
  makeParam({
    id: 20,
    name: 'pad.note',
    label: 'Note',
    group: 5,
    type: ParamType.Note,
    count: PADS.length,
    defaults: PADS.map((_, i) => 36 + i),
    indexLabels: PADS,
    flags: P,
  }),
  makeParam({ id: 24, name: 'pad.threshold', label: 'Hit threshold', group: 5, max: 4095, defaults: [300] }),
  makeParam({
    id: PresetParam.Active,
    name: 'preset.active',
    label: 'Active preset',
    group: 6,
    min: 1,
    max: 3,
    defaults: [1],
  }),
  makeParam({
    id: PresetParam.Program,
    name: 'preset.program',
    label: 'Program change',
    group: 6,
    min: -1,
    defaults: [-1],
    flags: P,
  }),
  makeParam({
    id: PresetParam.Dirty,
    name: 'preset.dirty',
    label: 'Unsaved edits',
    group: 6,
    type: ParamType.Bool,
    max: 1,
    flags: ParamFlag.Volatile | ParamFlag.ReadOnly,
  }),
  makeParam({
    id: PresetParam.Count,
    name: 'preset.count',
    label: 'Presets in use',
    group: 6,
    min: 1,
    max: 3,
    defaults: [1],
    flags: ParamFlag.ReadOnly,
  }),
  makeParam({
    id: 13,
    name: 'debug.events',
    label: 'Send events',
    group: 7,
    type: ParamType.Bool,
    max: 1,
    flags: ParamFlag.Volatile,
  }),
];
const SLOTS = 3;
/** Slot 1 is the device file's; the others start as copies of it. */
const YAML_NAMES = ['Default'];

/** What a slot has stored. */
interface StoredSlot {
  name: string | null;
  values: Map<string, number>;
}

/** @returns `id:index` for every value of the params `keep` takes. */
function keysOf(keep: (param: ParamSpec) => boolean): Array<{ key: string; param: ParamSpec; index: number }> {
  return PARAMS.filter(keep).flatMap((param) =>
    Array.from({ length: param.count }, (_, index) => ({ key: `${param.id}:${index}`, param, index })),
  );
}

const isPerPreset = (param: ParamSpec) => (param.flags & ParamFlag.Preset) !== 0;

/** The pretend device behind an in-memory transport. */
class DemoDevice {
  readonly #schemaBytes = encodeSchema({ groups: GROUPS, params: PARAMS });
  readonly #live = new Map<string, number>();
  readonly #stored = new Map<number, StoredSlot>();
  #active = 1;
  #dirty = false;
  /** Presets in use: slots 1..used; the rest are free (§4.5). */
  #used = 2;
  #listeners = new Set<(bytes: Uint8Array) => void>();
  /** The controls: layout, components, RAW and EVENTs (demo-tools.ts). */
  readonly #tools = new DemoTools(
    (bytes) => this.#emit(bytes),
    () => this.#live.get('13:0') === 1,
  );

  constructor() {
    for (const { key, param, index } of keysOf(() => true)) {
      this.#live.set(key, param.defaults[index] ?? 0);
    }
    // A second preset to start with, so there is something to switch to and delete.
    this.#stored.set(2, { name: 'Drums', values: new Map([['1:0', 10]]) });
  }

  /** @returns The live values of `slot` as it would load: stored, else the defaults. */
  #valuesOf(slot: number): Map<string, number> {
    const stored = this.#stored.get(slot)?.values;
    return new Map(
      keysOf(isPerPreset).map(({ key, param, index }) => [key, stored?.get(key) ?? param.defaults[index] ?? 0]),
    );
  }

  /** PRESET_MOVE: the preset in `from` goes to `to`, the ones between shift towards `from`. */
  #move(from: number, to: number): void {
    const order = Array.from({ length: this.#used }, (_, i) => ({
      name: this.#stored.get(i + 1)?.name ?? (i < YAML_NAMES.length ? (YAML_NAMES[i] ?? null) : null),
      values: this.#valuesOf(i + 1),
      isActive: i + 1 === this.#active,
    }));
    const [moving] = order.splice(from - 1, 1);
    if (moving === undefined) {
      return;
    }
    order.splice(to - 1, 0, moving);
    order.forEach((preset, i) => {
      this.#stored.set(i + 1, { name: preset.name, values: preset.values });
    });
    this.#load(order.findIndex((preset) => preset.isActive) + 1);
  }

  /** PRESET_DELETE: the later presets move up, the last one in use is freed. */
  #delete(slot: number): void {
    for (let s = slot; s < this.#used; s++) {
      const next = this.#stored.get(s + 1);
      const name = next?.name ?? (s + 1 <= YAML_NAMES.length ? (YAML_NAMES[s] ?? null) : null);
      this.#stored.set(s, { name, values: this.#valuesOf(s + 1) });
    }
    this.#stored.delete(this.#used);
    const was = this.#active;
    this.#used -= 1;
    const next = was === slot ? Math.min(slot, this.#used) : was > slot ? was - 1 : was;
    this.#load(next);
  }

  transport(): Transport {
    return {
      send: (bytes) => setTimeout(() => this.#handle(bytes), 15),
      subscribe: (listener) => {
        this.#listeners.add(listener);
        return () => this.#listeners.delete(listener);
      },
    };
  }

  #emit(bytes: Uint8Array): void {
    for (const listener of this.#listeners) {
      listener(bytes);
    }
  }

  #changed(): void {
    this.#emit(encodeFrame(Unsolicited.PresetChanged, 0, [...encodeU14(this.#active), this.#dirty ? 1 : 0]));
  }

  #load(slot: number): void {
    for (const [key, value] of this.#valuesOf(slot)) {
      this.#live.set(key, value);
    }
    this.#active = slot;
    this.#live.set(`${PresetParam.Active}:0`, slot);
    this.#dirty = false;
  }

  #name(slot: number): string {
    return this.#stored.get(slot)?.name ?? YAML_NAMES[slot - 1] ?? `Preset ${slot}`;
  }

  #info(slot: number): number[] {
    const flags =
      (this.#stored.has(slot) ? PresetFlag.Stored : 0) |
      (slot === this.#active && this.#dirty ? PresetFlag.Dirty : 0) |
      (slot <= YAML_NAMES.length ? PresetFlag.Yaml : 0) |
      (slot <= this.#used ? PresetFlag.InUse : 0);
    const program = slot === this.#active ? (this.#live.get(`${PresetParam.Program}:0`) ?? -1) : -1;
    return [
      ...encodeU14(slot),
      ...encodeU14(SLOTS),
      ...encodeU14(this.#active),
      flags,
      ...encodeU14(1),
      ...encodeU14(1),
      ...encodeS32(program),
      ...encodeLstring(this.#name(slot)),
    ];
  }

  #handle(bytes: Uint8Array): void {
    const result = decodeFrame(bytes);
    if (result.ok === false) {
      return;
    }
    const { cmd, seq, payload } = result.frame;
    const reply = (status: number, fields: number[] = []) =>
      this.#emit(encodeFrame(cmd | 0x40, seq, [status, ...fields]));
    const reader = new PayloadReader(payload);
    const isNotification = cmd >= NOTIFICATION_FIRST && cmd <= NOTIFICATION_LAST;
    if (isNotification) {
      return; // FB_*: never answered; the demo has no LEDs to light
    }
    switch (cmd) {
      case Command.Hello:
        reply(Status.Ok, [
          Role.App,
          ...encodeU14(2),
          ...encodeU32(0x0002_003e),
          0x01,
          ...encodeU14(256),
          ...encodeU14(
            Capability.Config |
              Capability.Log |
              Capability.Event |
              Capability.Presets |
              Capability.Feedback |
              Capability.Layout |
              Capability.Monitor,
          ),
          ...pack([0xde, 0x70, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]),
          14,
          ...Array.from('m-audio-code61', (c) => c.charCodeAt(0)),
        ]);
        return;
      case Command.GetLayout: {
        const offset = reader.u28();
        const max = reader.u14();
        const bytes = this.#tools.layoutBytes;
        reply(Status.Ok, [
          ...encodeU28(bytes.length),
          ...encodeU28(offset),
          ...pack(bytes.slice(offset, offset + max)),
        ]);
        return;
      }
      case Command.Monitor:
        reply(Status.Ok, this.#tools.monitor(reader.byte()));
        return;
      case Command.GetSchema: {
        const offset = reader.u28();
        const max = reader.u14();
        const data = this.#schemaBytes.slice(offset, offset + max);
        reply(Status.Ok, [...encodeU28(this.#schemaBytes.length), ...encodeU28(offset), ...pack(data)]);
        return;
      }
      case Command.GetParam:
      case Command.SetParam: {
        const id = reader.u14();
        const index = reader.u14();
        const param = PARAMS.find((p) => p.id === id);
        if (param === undefined || index >= param.count) {
          reply(Status.BadParam);
          return;
        }
        const key = `${id}:${index}`;
        if (cmd === Command.SetParam) {
          const value = reader.s32();
          if (value < param.min || value > param.max || (param.flags & ParamFlag.ReadOnly) !== 0) {
            reply(Status.BadValue);
            return;
          }
          if (id === PresetParam.Active) {
            if (value > this.#used) {
              reply(Status.BadValue);
              return;
            }
            this.#load(value);
            reply(Status.Ok, [...encodeU14(id), ...encodeU14(index), ...encodeS32(value)]);
            this.#changed();
            return;
          }
          this.#live.set(key, value);
          this.#tools.wake();
          if (isPerPreset(param) && this.#dirty === false) {
            this.#dirty = true;
            setTimeout(() => this.#changed(), 0);
          }
        }
        const now =
          id === PresetParam.Dirty
            ? Number(this.#dirty)
            : id === PresetParam.Count
              ? this.#used
              : (this.#live.get(key) ?? 0);
        reply(Status.Ok, [...encodeU14(id), ...encodeU14(index), ...encodeS32(now)]);
        return;
      }
      case Command.Commit:
      case Command.PresetSave: {
        const slot = cmd === Command.Commit ? this.#active : reader.u14();
        if (slot < 1 || slot > SLOTS) {
          reply(Status.BadParam);
          return;
        }
        const values = new Map(keysOf(isPerPreset).map(({ key }) => [key, this.#live.get(key) ?? 0]));
        this.#stored.set(slot, { name: this.#stored.get(slot)?.name ?? null, values });
        this.#used = Math.max(this.#used, slot);
        this.#active = slot;
        this.#live.set(`${PresetParam.Active}:0`, slot);
        this.#dirty = false;
        reply(Status.Ok);
        this.#changed();
        return;
      }
      case Command.PresetReset: {
        const slot = reader.u14();
        this.#stored.delete(slot);
        reply(Status.Ok);
        if (slot === this.#active) {
          this.#load(slot);
          this.#changed();
        }
        return;
      }
      case Command.PresetName: {
        const slot = reader.u14();
        const name = reader.lstring();
        const old = this.#stored.get(slot);
        if (slot > this.#used) {
          // Naming a free slot makes it a preset: a copy of preset 1's firmware values.
          this.#stored.set(slot, { name: name === '' ? null : name, values: new Map() });
          this.#used = slot;
        } else if (name === '' && (old === undefined || old.values.size === 0)) {
          this.#stored.delete(slot);
        } else {
          this.#stored.set(slot, { name: name === '' ? null : name, values: old?.values ?? new Map() });
        }
        reply(Status.Ok);
        return;
      }
      case Command.PresetInfo: {
        const asked = reader.u14();
        const slot = asked === 0 ? this.#active : asked;
        if (slot < 1 || slot > SLOTS) {
          reply(Status.BadParam);
          return;
        }
        reply(Status.Ok, this.#info(slot));
        return;
      }
      case Command.PresetDelete: {
        const slot = reader.u14();
        if (slot < 1 || slot > this.#used) {
          reply(Status.BadParam);
          return;
        }
        if (this.#used === 1) {
          reply(Status.BadState);
          return;
        }
        this.#delete(slot);
        reply(Status.Ok);
        this.#changed();
        return;
      }
      case Command.PresetMove: {
        const from = reader.u14();
        const to = reader.u14();
        if (from < 1 || from > this.#used || to < 1 || to > this.#used) {
          reply(Status.BadParam);
          return;
        }
        this.#move(from, to);
        reply(Status.Ok);
        this.#changed();
        return;
      }
      case Command.GetComponent: {
        const kind = reader.byte();
        const index = reader.u14();
        const head = (count: number, id: string, name: string) => [
          kind,
          ...encodeU14(index),
          ...encodeU14(count),
          ...encodeLstring(id),
          ...encodeLstring(name),
          ...encodeLstring(''),
        ];
        const led = DEMO_LEDS[index];
        if (kind === ComponentKind.Led && led !== undefined) {
          reply(Status.Ok, [...head(DEMO_LEDS.length, led.id, led.name), led.colours, LedOwner.HostThenLocal]);
        } else if (kind === ComponentKind.Display && index === 0) {
          reply(Status.Ok, [...head(1, 'display', 'Display'), DEMO_DISPLAY_COLS, 1, 1]);
        } else if (this.#tools.component(kind, index) !== null) {
          reply(Status.Ok, this.#tools.component(kind, index) ?? []);
        } else {
          reply(Status.BadParam);
        }
        return;
      }
      case Command.FactoryReset:
        this.#stored.clear();
        this.#used = YAML_NAMES.length;
        for (const { key, param, index } of keysOf(() => true)) {
          this.#live.set(key, param.defaults[index] ?? 0);
        }
        this.#load(1);
        reply(Status.Ok);
        this.#changed();
        return;
      default:
        reply(Status.BadCmd);
    }
  }
}

/** @returns A client talking to a new pretend Code 61. */
export function demoClient(): SysexClient {
  // withDemoMidi (demo-midi.ts): notes and CCs now and then, for the MIDI monitor.
  return new SysexClient(withDemoMidi(new DemoDevice().transport()));
}

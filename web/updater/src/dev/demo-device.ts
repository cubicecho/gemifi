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
  ParamFlag,
  ParamType,
  PresetFlag,
  PresetParam,
  Role,
  Status,
  Unsolicited,
} from '../protocol/wire.ts';

const P = ParamFlag.Preset;
const BUTTONS = ['Oct -', 'Oct +', 'Sustain', 'Shift'];
const ANALOGS = ['Mod wheel', 'Expression'];
const PADS = ['Pad 1', 'Pad 2', 'Pad 3', 'Pad 4'];

/** Roughly the Code 61's groups and params, with the per-preset ones flagged. */
const GROUPS = ['MIDI', 'Velocity', 'Buttons', 'Analog', 'Pads', 'Presets', 'Debug'];
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
    count: 4,
    max: 6,
    defaults: [5, 6, 1, 0],
    indexLabels: BUTTONS,
    options: ['None', 'CC momentary', 'CC toggle', 'CC ramp', 'Note', 'Octave down', 'Octave up'],
    flags: P,
  }),
  makeParam({
    id: 10,
    name: 'button.number',
    label: 'Number',
    group: 2,
    count: 4,
    defaults: [0, 0, 64, 0],
    indexLabels: BUTTONS,
    flags: P,
  }),
  makeParam({
    id: 8,
    name: 'analog.cc',
    label: 'CC',
    group: 3,
    count: 2,
    defaults: [1, 11],
    indexLabels: ANALOGS,
    flags: P,
  }),
  makeParam({
    id: 29,
    name: 'analog.mode',
    label: 'Sends',
    group: 3,
    type: ParamType.Enum,
    count: 2,
    max: 2,
    indexLabels: ANALOGS,
    options: ['CC', 'Pitch bend', 'Channel pressure'],
    flags: P,
  }),
  makeParam({
    id: 20,
    name: 'pad.note',
    label: 'Note',
    group: 4,
    type: ParamType.Note,
    count: 4,
    defaults: [36, 38, 42, 46],
    indexLabels: PADS,
    flags: P,
  }),
  makeParam({ id: 24, name: 'pad.threshold', label: 'Hit threshold', group: 4, max: 4095, defaults: [300] }),
  makeParam({
    id: PresetParam.Active,
    name: 'preset.active',
    label: 'Active preset',
    group: 5,
    min: 1,
    max: 3,
    defaults: [1],
  }),
  makeParam({
    id: PresetParam.Program,
    name: 'preset.program',
    label: 'Program change',
    group: 5,
    min: -1,
    defaults: [-1],
    flags: P,
  }),
  makeParam({
    id: PresetParam.Dirty,
    name: 'preset.dirty',
    label: 'Unsaved edits',
    group: 5,
    type: ParamType.Bool,
    max: 1,
    flags: ParamFlag.Volatile | ParamFlag.ReadOnly,
  }),
  makeParam({
    id: 13,
    name: 'debug.events',
    label: 'Send events',
    group: 6,
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
  #listeners = new Set<(bytes: Uint8Array) => void>();

  constructor() {
    for (const { key, param, index } of keysOf(() => true)) {
      this.#live.set(key, param.defaults[index] ?? 0);
    }
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
    const stored = this.#stored.get(slot)?.values;
    for (const { key, param, index } of keysOf(isPerPreset)) {
      this.#live.set(key, stored?.get(key) ?? param.defaults[index] ?? 0);
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
      (slot <= YAML_NAMES.length ? PresetFlag.Yaml : 0);
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
    switch (cmd) {
      case Command.Hello:
        reply(Status.Ok, [
          Role.App,
          ...encodeU14(2),
          ...encodeU32(0x0002_003e),
          0x01,
          ...encodeU14(256),
          ...encodeU14(Capability.Config | Capability.Log | Capability.Event | Capability.Presets),
          ...pack([0xde, 0x70, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]),
          14,
          ...Array.from('m-audio-code61', (c) => c.charCodeAt(0)),
        ]);
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
            this.#load(value);
            reply(Status.Ok, [...encodeU14(id), ...encodeU14(index), ...encodeS32(value)]);
            this.#changed();
            return;
          }
          this.#live.set(key, value);
          if (isPerPreset(param) && this.#dirty === false) {
            this.#dirty = true;
            setTimeout(() => this.#changed(), 0);
          }
        }
        const now = id === PresetParam.Dirty ? Number(this.#dirty) : (this.#live.get(key) ?? 0);
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
        if (name === '' && (old === undefined || old.values.size === 0)) {
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
      case Command.FactoryReset:
        this.#stored.clear();
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
  return new SysexClient(new DemoDevice().transport());
}

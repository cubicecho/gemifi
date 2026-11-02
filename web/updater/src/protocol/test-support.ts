/**
 * Helpers shared by the protocol tests: a synthetic `.gfw` image built the way mkimage would, a
 * schema encoder, the example schema and a fake config device.
 */

import type { Transport } from './client.ts';
import { encodeS32, encodeU14, encodeU28, PayloadReader, pack } from './encoding.ts';
import { decodeFrame, encodeFrame } from './frame.ts';
import { HEADER_OFFSET, IMAGE_MAGIC, imageCrc } from './image.ts';
import type { ParamSpec, Schema } from './schema.ts';
import { Command, ParamFlag, ParamType, Status } from './wire.ts';

/** Fields a test may change in the synthetic image. */
export interface TestImageOptions {
  size?: number;
  boardId?: number;
  fwVersion?: number;
  stackPointer?: number;
  resetVector?: number;
  magic?: number;
  /** Where the header goes: 0xC0 (NUC1xx) unless given. */
  headerOffset?: number;
}

/**
 * Builds a valid image: a vector table, a filled header with a correct CRC, and patterned code.
 *
 * @param options - Fields to change.
 * @returns The file's bytes.
 */
export function makeTestImage(options: TestImageOptions = {}): Uint8Array {
  const size = options.size ?? 1024;
  const bytes = new Uint8Array(size);
  for (let i = 0; i < size; i++) {
    bytes[i] = (i * 37 + 11) & 0xff;
  }
  const view = new DataView(bytes.buffer);
  view.setUint32(0, options.stackPointer ?? 0x2000_0ff0, true);
  view.setUint32(4, options.resetVector ?? 0x0000_0101, true);
  const at = options.headerOffset ?? HEADER_OFFSET;
  view.setUint32(at, options.magic ?? IMAGE_MAGIC, true);
  view.setUint16(at + 4, 1, true);
  view.setUint16(at + 6, options.boardId ?? 1, true);
  view.setUint32(at + 8, size, true);
  view.setUint32(at + 16, options.fwVersion ?? 0x0102_0003, true);
  view.setUint32(at + 20, 1, true);
  view.setUint32(at + 24, 0xffff_ffff, true);
  view.setUint32(at + 28, 0xffff_ffff, true);
  view.setUint32(at + 12, imageCrc(bytes, size, at), true);
  return bytes;
}

/** Fields a test gives for one param; the rest default and the layout flags are worked out. */
export type TestParam = Pick<ParamSpec, 'id' | 'name' | 'label'> & Partial<ParamSpec>;

/**
 * Fills a param the way the parser will return it: PER_INDEX_DEFAULT, INDEX_LABELS and HAS_UNIT
 * set from the data, `defaults` repeated to `count`.
 *
 * @param fields - What the test cares about.
 * @returns The whole param.
 */
export function makeParam(fields: TestParam): ParamSpec {
  const count = fields.count ?? 1;
  const defaults = fields.defaults ?? [fields.min ?? 0];
  const isPerIndex = defaults.length > 1 && defaults.some((d) => d !== defaults[0]);
  let flags = fields.flags ?? 0;
  flags |= isPerIndex ? ParamFlag.PerIndexDefault : 0;
  flags |= fields.indexLabels ? ParamFlag.IndexLabels : 0;
  flags |= fields.unit ? ParamFlag.HasUnit : 0;
  const type = fields.type ?? ParamType.Int;
  return {
    type,
    rawType: fields.rawType ?? type,
    group: 0,
    min: 0,
    max: 127,
    unit: null,
    indexLabels: null,
    options: null,
    ...fields,
    count,
    flags,
    defaults: isPerIndex ? defaults : new Array(count).fill(defaults[0]),
  };
}

/**
 * Writes a schema as §4.4 lays it out (the inverse of `parseSchema`).
 *
 * @param schema - Groups and params.
 * @returns The unpacked bytes.
 */
export function encodeSchema(schema: { version?: number; groups: string[]; params: ParamSpec[] }): number[] {
  const out: number[] = [];
  const u16 = (v: number) => out.push(v & 0xff, (v >> 8) & 0xff);
  const i32 = (v: number) => out.push(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff);
  const str = (s: string) => out.push(s.length, ...Array.from(s, (c) => c.charCodeAt(0)));
  out.push(schema.version ?? 1, schema.groups.length);
  u16(schema.params.length);
  schema.groups.forEach(str);
  for (const p of schema.params) {
    u16(p.id);
    out.push(p.rawType, p.group);
    u16(p.count);
    out.push(p.flags);
    i32(p.min);
    i32(p.max);
    const defaults = p.flags & ParamFlag.PerIndexDefault ? p.defaults : p.defaults.slice(0, 1);
    defaults.forEach(i32);
    str(p.name);
    str(p.label);
    if (p.flags & ParamFlag.HasUnit) {
      str(p.unit ?? '');
    }
    if (p.flags & ParamFlag.IndexLabels) {
      (p.indexLabels ?? []).forEach(str);
    }
    if (p.rawType === ParamType.Enum) {
      out.push((p.options ?? []).length);
      (p.options ?? []).forEach(str);
    }
  }
  return out;
}

/** Button index labels of the example schema. */
const BUTTONS = ['Sustain', 'Edit', 'Oct -', 'Oct +', 'Pitch down', 'Pitch up', 'Mod'];

/** The Keystation example schema from the task (and, roughly, the firmware). */
export const EXAMPLE_SCHEMA: Schema = {
  version: 1,
  groups: ['MIDI', 'Velocity', 'Knob', 'Buttons', 'Debug'],
  params: [
    makeParam({ id: 1, name: 'midi.channel', label: 'MIDI channel', group: 0, min: 1, max: 16, defaults: [1] }),
    makeParam({ id: 2, name: 'midi.transpose', label: 'Transpose', group: 0, min: -24, max: 24, defaults: [0] }),
    makeParam({ id: 3, name: 'midi.octave', label: 'Octave', group: 0, min: -4, max: 4, defaults: [0] }),
    makeParam({
      id: 4,
      name: 'velocity.curve',
      label: 'Velocity curve',
      group: 1,
      type: ParamType.Enum,
      min: 0,
      max: 3,
      defaults: [0],
      options: ['Normal', 'Soft', 'Hard', 'Fixed'],
    }),
    makeParam({ id: 5, name: 'velocity.fixed', label: 'Fixed velocity', group: 1, min: 1, max: 127, defaults: [100] }),
    makeParam({ id: 8, name: 'knob.cc', label: 'Knob CC', group: 2, min: 0, max: 127, defaults: [1] }),
    makeParam({
      id: 9,
      name: 'button.action',
      label: 'Action',
      group: 3,
      type: ParamType.Enum,
      count: 7,
      min: 0,
      max: 12,
      defaults: [1, 0, 5, 6, 9, 10, 1],
      indexLabels: BUTTONS,
      options: [
        'None',
        'CC momentary',
        'CC toggle',
        'CC ramp',
        'Note',
        'Octave down',
        'Octave up',
        'Transpose down',
        'Transpose up',
        'Pitch bend down',
        'Pitch bend up',
        'Program next',
        'Program previous',
      ],
    }),
    makeParam({
      id: 10,
      name: 'button.number',
      label: 'Number',
      group: 3,
      count: 7,
      min: 0,
      max: 127,
      defaults: [64, 0, 0, 0, 0, 0, 1],
      indexLabels: BUTTONS,
    }),
    makeParam({
      id: 13,
      name: 'debug.events',
      label: 'Send events',
      group: 4,
      type: ParamType.Bool,
      min: 0,
      max: 1,
      defaults: [0],
      flags: ParamFlag.Volatile,
    }),
  ],
};

/** How a {@link FakeConfigDevice} behaves. */
export interface FakeConfigOptions {
  schema?: Schema;
  /** Answer GET_PARAMS and SET_PARAMS; off, they get BAD_CMD. */
  supportsRuns?: boolean;
  /** Replies to drop, by request command, once each. */
  dropReplies?: number[];
}

/** An app with a config, following §4.3, behind an in-memory transport. */
export class FakeConfigDevice {
  readonly schema: Schema;
  readonly schemaBytes: number[];
  readonly values = new Map<string, number>();
  stored: Map<string, number> | null = null;
  supportsRuns: boolean;
  dropReplies: number[];
  /** Every request command received, in order. */
  received: number[] = [];
  /** Statuses to answer the next requests of a command with, once each. */
  forcedStatus = new Map<number, number>();
  #listeners = new Set<(bytes: Uint8Array) => void>();
  #unsolicitedSeq = 0;

  /**
   * @param options - Schema and behaviour.
   */
  constructor(options: FakeConfigOptions = {}) {
    this.schema = options.schema ?? EXAMPLE_SCHEMA;
    this.schemaBytes = encodeSchema(this.schema);
    this.supportsRuns = options.supportsRuns ?? true;
    this.dropReplies = options.dropReplies ?? [];
    this.#loadDefaults();
  }

  /** @returns A transport to this device. */
  transport(): Transport {
    return {
      send: (bytes) => queueMicrotask(() => this.#handle(bytes)),
      subscribe: (listener) => {
        this.#listeners.add(listener);
        return () => this.#listeners.delete(listener);
      },
    };
  }

  /**
   * @param id - A param.
   * @param index - An index.
   * @returns The live value.
   */
  value(id: number, index = 0): number | undefined {
    return this.values.get(`${id}:${index}`);
  }

  /**
   * Changes a value as the device itself would, and says so with PARAM_CHANGED.
   *
   * @param id - The param.
   * @param index - The index.
   * @param value - The new value.
   */
  changeByItself(id: number, index: number, value: number): void {
    this.values.set(`${id}:${index}`, value);
    this.#emit(
      encodeFrame(0x62, this.#unsolicitedSeq++ & 0x7f, [...encodeU14(id), ...encodeU14(index), ...encodeS32(value)]),
    );
  }

  #loadDefaults(): void {
    for (const p of this.schema.params) {
      p.defaults.forEach((d, i) => {
        this.values.set(`${p.id}:${i}`, d);
      });
    }
  }

  #emit(bytes: Uint8Array): void {
    for (const listener of this.#listeners) {
      listener(bytes);
    }
  }

  #reply(cmd: number, seq: number, payload: number[]): void {
    const dropAt = this.dropReplies.indexOf(cmd);
    if (dropAt >= 0) {
      this.dropReplies.splice(dropAt, 1);
      return;
    }
    this.#emit(encodeFrame(cmd | 0x40, seq, payload));
  }

  /**
   * @param id - A param id.
   * @param index - First index.
   * @param n - How many.
   * @param isWrite - Whether it is a write (READ_ONLY refuses).
   * @returns The param, or null when the address is BAD_PARAM.
   */
  #address(id: number, index: number, n: number, isWrite: boolean): ParamSpec | null {
    const p = this.schema.params.find((q) => q.id === id);
    const isBad = p === undefined || index + n > p.count || (isWrite && (p.flags & ParamFlag.ReadOnly) !== 0);
    return isBad ? null : p;
  }

  #handle(bytes: Uint8Array): void {
    const result = decodeFrame(bytes);
    if (result.ok === false) {
      return;
    }
    const { cmd, seq, payload } = result.frame;
    this.received.push(cmd);
    const forced = this.forcedStatus.get(cmd);
    if (forced !== undefined) {
      this.forcedStatus.delete(cmd);
      this.#reply(cmd, seq, [forced]);
      return;
    }
    const reader = new PayloadReader(payload);
    const isRun = cmd === Command.GetParams || cmd === Command.SetParams;
    if (isRun && this.supportsRuns === false) {
      this.#reply(cmd, seq, [Status.BadCmd]);
      return;
    }
    switch (cmd) {
      case Command.GetSchema: {
        const offset = reader.u28();
        const max = reader.u14();
        if (max === 0 || max > 64) {
          this.#reply(cmd, seq, [Status.BadLen]);
          return;
        }
        const data = this.schemaBytes.slice(offset, offset + max);
        this.#reply(cmd, seq, [Status.Ok, ...encodeU28(this.schemaBytes.length), ...encodeU28(offset), ...pack(data)]);
        return;
      }
      case Command.GetParam:
      case Command.SetParam: {
        const id = reader.u14();
        const index = reader.u14();
        const isSet = cmd === Command.SetParam;
        const p = this.#address(id, index, 1, isSet);
        if (p === null) {
          this.#reply(cmd, seq, [Status.BadParam]);
          return;
        }
        if (isSet) {
          const value = reader.s32();
          if (value < p.min || value > p.max) {
            this.#reply(cmd, seq, [Status.BadValue]);
            return;
          }
          this.values.set(`${id}:${index}`, value);
        }
        const now = this.values.get(`${id}:${index}`) ?? 0;
        this.#reply(cmd, seq, [Status.Ok, ...encodeU14(id), ...encodeU14(index), ...encodeS32(now)]);
        return;
      }
      case Command.GetParams:
      case Command.SetParams: {
        const id = reader.u14();
        const index = reader.u14();
        const n = reader.u14();
        const isSet = cmd === Command.SetParams;
        const p = this.#address(id, index, n, isSet);
        if (p === null || n === 0 || n > 16) {
          this.#reply(cmd, seq, [p === null ? Status.BadParam : Status.BadLen]);
          return;
        }
        if (isSet) {
          const values = Array.from({ length: n }, () => reader.s32());
          if (values.some((v) => v < p.min || v > p.max)) {
            this.#reply(cmd, seq, [Status.BadValue]);
            return;
          }
          values.forEach((v, i) => {
            this.values.set(`${id}:${index + i}`, v);
          });
          this.#reply(cmd, seq, [Status.Ok, ...encodeU14(id), ...encodeU14(index), ...encodeU14(n)]);
          return;
        }
        const values = Array.from({ length: n }, (_, i) => this.values.get(`${id}:${index + i}`) ?? 0);
        this.#reply(cmd, seq, [
          Status.Ok,
          ...encodeU14(id),
          ...encodeU14(index),
          ...encodeU14(n),
          ...values.flatMap(encodeS32),
        ]);
        return;
      }
      case Command.Commit:
        this.stored = new Map(this.values);
        this.#reply(cmd, seq, [Status.Ok]);
        return;
      case Command.FactoryReset:
        this.stored = null;
        this.#loadDefaults();
        this.#reply(cmd, seq, [Status.Ok]);
        return;
      default:
        this.#reply(cmd, seq, [Status.BadCmd]);
    }
  }
}

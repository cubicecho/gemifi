import { describe, expect, it } from 'vitest';
import {
  BACKUP_FORMAT,
  BACKUP_FORMAT_PRESETS,
  type BackupDoc,
  backupFileName,
  backupSummary,
  isPresetsBackup,
  readBackup,
  restoreBackup,
  stalePresets,
} from './backup.ts';
import { SysexClient, type Transport } from './client.ts';
import { getSchema } from './config.ts';
import { encodeLstring, encodeS32, encodeU14, encodeU28, PayloadReader, pack } from './encoding.ts';
import { decodeFrame, encodeFrame } from './frame.ts';
import type { HelloInfo } from './messages.ts';
import type { PresetInfo } from './presets.ts';
import type { ParamSpec } from './schema.ts';
import { encodeSchema, makeParam } from './test-support.ts';
import { Capability, Command, ParamFlag, ParamType, PresetFlag, PresetParam, Role, Status } from './wire.ts';

const FAST = { replyTimeoutMs: 15, longReplyTimeoutMs: 60, maxRetries: 2 };
const P = ParamFlag.Preset;

const PARAMS: ParamSpec[] = [
  makeParam({ id: 1, name: 'midi.channel', label: 'Channel', min: 1, max: 16, defaults: [1], flags: P }),
  makeParam({ id: 9, name: 'button.number', label: 'Number', count: 3, defaults: [64, 0, 1], flags: P }),
  makeParam({ id: 6, name: 'velocity.fast', label: 'Fast', max: 5000, defaults: [400] }),
  makeParam({ id: PresetParam.Active, name: 'preset.active', label: 'Active', min: 1, max: 4, defaults: [1] }),
  makeParam({
    id: PresetParam.Dirty,
    name: 'preset.dirty',
    label: 'Dirty',
    type: ParamType.Bool,
    max: 1,
    flags: ParamFlag.Volatile | ParamFlag.ReadOnly,
  }),
  makeParam({
    id: PresetParam.Count,
    name: 'preset.count',
    label: 'In use',
    min: 1,
    max: 4,
    defaults: [1],
    flags: ParamFlag.ReadOnly,
  }),
];
const SLOTS = 4;
const isPerPreset = (param: ParamSpec) => (param.flags & P) !== 0;
const keysOf = (keep: (param: ParamSpec) => boolean) =>
  PARAMS.filter(keep).flatMap((param) =>
    Array.from({ length: param.count }, (_, index) => ({ key: `${param.id}:${index}`, param, index })),
  );

/** An app with config and presets (§4.3, §4.5), as the firmware behaves, in memory. */
class FakePresetsDevice {
  readonly schemaBytes = encodeSchema({ groups: ['All'], params: PARAMS });
  readonly live = new Map<string, number>();
  /** Stored edits per slot: values and name. */
  readonly stored = new Map<number, { name: string | null; values: Map<string, number>; revision: number }>();
  revision = 1;
  active = 1;
  dirty = false;
  used = 1;
  /** Every request command, in order. */
  received: number[] = [];
  #listeners = new Set<(bytes: Uint8Array) => void>();

  constructor() {
    for (const { key, param, index } of keysOf(() => true)) {
      this.live.set(key, param.defaults[index] ?? 0);
    }
  }

  transport(): Transport {
    return {
      send: (bytes) => queueMicrotask(() => this.#handle(bytes)),
      subscribe: (listener) => {
        this.#listeners.add(listener);
        return () => this.#listeners.delete(listener);
      },
    };
  }

  valuesOf(slot: number): Map<string, number> {
    const stored = this.stored.get(slot)?.values;
    return new Map(
      keysOf(isPerPreset).map(({ key, param, index }) => [key, stored?.get(key) ?? param.defaults[index] ?? 0]),
    );
  }

  nameOf(slot: number): string {
    return this.stored.get(slot)?.name ?? (slot === 1 ? 'Default' : `Preset ${slot}`);
  }

  #load(slot: number): void {
    for (const [key, value] of this.valuesOf(slot)) {
      this.live.set(key, value);
    }
    this.active = slot;
    this.live.set(`${PresetParam.Active}:0`, slot);
    this.dirty = false;
  }

  #store(slot: number): void {
    const values = new Map(keysOf(isPerPreset).map(({ key }) => [key, this.live.get(key) ?? 0]));
    this.stored.set(slot, { name: this.stored.get(slot)?.name ?? null, values, revision: this.revision });
  }

  #info(slot: number): number[] {
    const stored = this.stored.get(slot);
    const flags =
      (stored === undefined ? 0 : PresetFlag.Stored) |
      (slot === this.active && this.dirty ? PresetFlag.Dirty : 0) |
      (slot === 1 ? PresetFlag.Yaml : 0) |
      (slot <= this.used ? PresetFlag.InUse : 0);
    return [
      ...encodeU14(slot),
      ...encodeU14(SLOTS),
      ...encodeU14(this.active),
      flags,
      ...encodeU14(this.revision),
      ...encodeU14(stored?.revision ?? 0),
      ...encodeS32(-1),
      ...encodeLstring(this.nameOf(slot)),
    ];
  }

  #set(param: ParamSpec, key: string, value: number): void {
    this.live.set(key, value);
    if (isPerPreset(param)) {
      this.dirty = true;
    }
  }

  #handle(bytes: Uint8Array): void {
    const result = decodeFrame(bytes);
    if (result.ok === false) {
      return;
    }
    const { cmd, seq, payload } = result.frame;
    this.received.push(cmd);
    const reply = (status: number, fields: number[] = []) => {
      const frame = encodeFrame(cmd | 0x40, seq, [status, ...fields]);
      for (const listener of this.#listeners) {
        listener(frame);
      }
    };
    const reader = new PayloadReader(payload);
    switch (cmd) {
      case Command.GetSchema: {
        const offset = reader.u28();
        const max = reader.u14();
        const data = this.schemaBytes.slice(offset, offset + max);
        reply(Status.Ok, [...encodeU28(this.schemaBytes.length), ...encodeU28(offset), ...pack(data)]);
        return;
      }
      case Command.GetParam:
      case Command.SetParam:
      case Command.GetParams:
      case Command.SetParams: {
        const id = reader.u14();
        const index = reader.u14();
        const isRun = cmd === Command.GetParams || cmd === Command.SetParams;
        const n = isRun ? reader.u14() : 1;
        const param = PARAMS.find((p) => p.id === id);
        if (param === undefined || index + n > param.count) {
          reply(Status.BadParam);
          return;
        }
        const isSet = cmd === Command.SetParam || cmd === Command.SetParams;
        if (isSet) {
          const values = Array.from({ length: n }, () => reader.s32());
          if ((param.flags & ParamFlag.ReadOnly) !== 0) {
            reply(Status.BadParam);
            return;
          }
          if (values.some((v) => v < param.min || v > param.max)) {
            reply(Status.BadValue);
            return;
          }
          if (id === PresetParam.Active) {
            if ((values[0] ?? 0) > this.used) {
              reply(Status.BadValue);
              return;
            }
            this.#load(values[0] ?? 1);
          } else {
            values.forEach((v, i) => {
              this.#set(param, `${id}:${index + i}`, v);
            });
          }
          if (isRun) {
            reply(Status.Ok, [...encodeU14(id), ...encodeU14(index), ...encodeU14(n)]);
            return;
          }
        }
        const read = (i: number) => {
          if (id === PresetParam.Dirty) {
            return Number(this.dirty);
          }
          if (id === PresetParam.Count) {
            return this.used;
          }
          return this.live.get(`${id}:${i}`) ?? 0;
        };
        const values = Array.from({ length: n }, (_, i) => read(index + i));
        const head = [...encodeU14(id), ...encodeU14(index)];
        reply(
          Status.Ok,
          isRun ? [...head, ...encodeU14(n), ...values.flatMap(encodeS32)] : [...head, ...encodeS32(values[0] ?? 0)],
        );
        return;
      }
      case Command.Commit:
        this.#store(this.active);
        this.dirty = false;
        reply(Status.Ok);
        return;
      case Command.PresetInfo: {
        const asked = reader.u14();
        reply(Status.Ok, this.#info(asked === 0 ? this.active : asked));
        return;
      }
      case Command.PresetSave: {
        const slot = reader.u14();
        this.#store(slot);
        this.used = Math.max(this.used, slot);
        this.active = slot;
        this.dirty = false;
        reply(Status.Ok);
        return;
      }
      case Command.PresetReset: {
        const slot = reader.u14();
        this.stored.delete(slot);
        if (slot === this.active) {
          this.#load(slot);
        }
        reply(Status.Ok);
        return;
      }
      case Command.PresetName: {
        const slot = reader.u14();
        const name = reader.lstring();
        const old = this.stored.get(slot);
        this.stored.set(slot, {
          name: name === '' ? null : name,
          values: old?.values ?? new Map(),
          revision: old?.revision ?? this.revision,
        });
        this.used = Math.max(this.used, slot);
        reply(Status.Ok);
        return;
      }
      case Command.PresetDelete: {
        const slot = reader.u14();
        for (let s = slot; s < this.used; s++) {
          const next = this.stored.get(s + 1);
          if (next === undefined) {
            this.stored.delete(s);
          } else {
            this.stored.set(s, next);
          }
        }
        this.stored.delete(this.used);
        this.used -= 1;
        this.#load(Math.min(this.active, this.used));
        reply(Status.Ok);
        return;
      }
      default:
        reply(Status.BadCmd);
    }
  }
}

/** HELLO of an app with config, and with presets unless told otherwise. */
function helloOf(hasPresets = true): HelloInfo {
  return {
    role: Role.App,
    boardId: 2,
    fwVersion: 0x0002_003e,
    protoVersion: 1,
    maxChunk: 0,
    caps: Capability.Config | (hasPresets ? Capability.Presets : 0),
    uid: [],
    nodeName: 'test',
  } as unknown as HelloInfo;
}

/** A device with three presets in use: 2 stored with edits, 3 named, the active one (2) dirty. */
function threePresets(): FakePresetsDevice {
  const device = new FakePresetsDevice();
  device.used = 3;
  device.stored.set(2, { name: 'Drums', values: new Map([['1:0', 10]]), revision: 1 });
  device.stored.set(3, { name: 'Pads', values: new Map(), revision: 1 });
  device.live.set('6:0', 900);
  return device;
}

describe('readBackup', () => {
  it('reads every preset in use and leaves the device as it was, unsaved edits included', async () => {
    const device = threePresets();
    const client = new SysexClient(device.transport(), FAST);
    const schema = await getSchema(client);
    await client.request(Command.SetParam, [...encodeU14(PresetParam.Active), ...encodeU14(0), ...encodeS32(2)]);
    await client.request(Command.SetParam, [...encodeU14(9), ...encodeU14(1), ...encodeS32(77)]);
    expect(device.dirty).toBe(true);
    const progress: number[] = [];
    const doc = await readBackup(client, helloOf(), schema, { onProgress: ({ done }) => progress.push(done) });
    expect(doc.gemifi_backup).toBe(BACKUP_FORMAT_PRESETS);
    expect(doc.board_id).toBe(2);
    expect(doc.fw_version).toBe('0.2.62');
    expect(doc.active).toBe(2);
    expect(doc.presets_revision).toBe(1);
    expect(doc.values).toEqual({ 'velocity.fast': 900, 'preset.count': 3 });
    expect(doc.presets?.map((p) => [p.slot, p.name, p.stored, p.unsaved ?? false])).toEqual([
      [1, 'Default', false, false],
      [2, 'Drums', true, true],
      [3, 'Pads', true, false],
    ]);
    expect(doc.presets?.[1]?.values).toEqual({ 'midi.channel': 10, 'button.number': [64, 77, 1] });
    expect(doc.presets?.[0]?.values).toEqual({ 'midi.channel': 1, 'button.number': [64, 0, 1] });
    expect(progress).toEqual([0, 1, 2, 3]);
    // Back on preset 2 with its unsaved edit.
    expect(device.active).toBe(2);
    expect(device.live.get('9:1')).toBe(77);
    expect(device.dirty).toBe(true);
    expect(backupSummary(doc)).toBe('8 params, 3 presets');
    expect(isPresetsBackup(JSON.parse(JSON.stringify(doc)))).toBe(true);
  });

  it('writes format 1 for a device without presets', async () => {
    const device = new FakePresetsDevice();
    const client = new SysexClient(device.transport(), FAST);
    const schema = await getSchema(client);
    const doc = await readBackup(client, helloOf(false), schema);
    expect(doc.gemifi_backup).toBe(BACKUP_FORMAT);
    expect(doc.presets).toBeUndefined();
    expect(doc.values['button.number']).toEqual([64, 0, 1]);
    expect(isPresetsBackup(doc)).toBe(false);
  });
});

describe('restoreBackup', () => {
  it('puts every preset back, deletes the extra ones and stores the result', async () => {
    const source = threePresets();
    const sourceClient = new SysexClient(source.transport(), FAST);
    const doc: BackupDoc = await readBackup(sourceClient, helloOf(), await getSchema(sourceClient));
    // A device whose presets went: four in use, other values.
    const target = new FakePresetsDevice();
    target.used = 4;
    target.stored.set(2, { name: 'Other', values: new Map([['1:0', 5]]), revision: 1 });
    target.live.set('6:0', 100);
    const client = new SysexClient(target.transport(), FAST);
    const report = await restoreBackup(client, helloOf(), await getSchema(client), JSON.parse(JSON.stringify(doc)));
    expect(report.problems).toEqual([]);
    expect(target.used).toBe(3);
    expect(target.live.get('6:0')).toBe(900);
    expect(target.nameOf(2)).toBe('Drums');
    expect(target.nameOf(3)).toBe('Pads');
    expect(target.valuesOf(2).get('1:0')).toBe(10);
    expect(target.active).toBe(1);
    expect(target.received.at(-1)).toBe(Command.Commit);
  });

  it('makes free slots presets first', async () => {
    const source = threePresets();
    const sourceClient = new SysexClient(source.transport(), FAST);
    const doc = await readBackup(sourceClient, helloOf(), await getSchema(sourceClient));
    const target = new FakePresetsDevice();
    const client = new SysexClient(target.transport(), FAST);
    await restoreBackup(client, helloOf(), await getSchema(client), doc);
    expect(target.used).toBe(3);
    expect(target.nameOf(3)).toBe('Pads');
  });

  it('writes a format-1 file to a device without presets, and reports unknown names', async () => {
    const target = new FakePresetsDevice();
    const client = new SysexClient(target.transport(), FAST);
    const report = await restoreBackup(client, helloOf(false), await getSchema(client), {
      gemifi_backup: 1,
      board_id: 1,
      values: { 'midi.channel': 7, 'gone.param': 3 },
    });
    expect(target.live.get('1:0')).toBe(7);
    expect(report.applied).toBe(1);
    expect(report.problems).toHaveLength(2);
    expect(report.problems[0]).toMatch(/board 1/);
    expect(report.problems[1]).toMatch(/gone\.param/);
  });

  it('refuses a file without values', async () => {
    const client = new SysexClient(new FakePresetsDevice().transport(), FAST);
    const schema = await getSchema(client);
    await expect(restoreBackup(client, helloOf(), schema, { presets: [] })).rejects.toThrow(/not a gemifi backup/);
  });
});

describe('backup helpers', () => {
  it('names the file as the host tool does', () => {
    const name = backupFileName({ boardId: 2, fwVersion: 0x0002_003e }, new Date(2026, 10, 5, 14, 2, 33));
    expect(name).toBe('gemifi-backup-0002-0.2.62-20261105-140233.json');
  });

  it('finds presets stored under another revision', () => {
    const slot = (n: number, flags: number, storedRevision: number): PresetInfo => ({
      slot: n,
      count: 3,
      active: 1,
      flags,
      revision: 2,
      storedRevision,
      program: -1,
      name: '',
    });
    const stale = stalePresets([slot(1, 0, 0), slot(2, PresetFlag.Stored, 1), slot(3, PresetFlag.Stored, 2)]);
    expect(stale.map((info) => info.slot)).toEqual([2]);
  });
});

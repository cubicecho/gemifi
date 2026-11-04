import { describe, expect, it } from 'vitest';
import { RequestError, SysexClient, type Transport } from './client.ts';
import { decodeU14, encodeLstring, encodeS32, encodeU14, PayloadReader } from './encoding.ts';
import { decodeFrame, encodeFrame } from './frame.ts';
import { parseDeviceMessage } from './messages.ts';
import {
  deletePreset,
  getPresetInfo,
  isPresetDirty,
  isPresetStored,
  offersPresets,
  type PresetInfo,
  parsePresetInfo,
  presetNameProblem,
  presetSlots,
  readPresets,
  renamePreset,
  resetPreset,
  savePreset,
  selectPreset,
} from './presets.ts';
import { Capability, Command, PresetFlag, PresetParam, Role, Status, Unsolicited } from './wire.ts';

const FAST = { replyTimeoutMs: 15, longReplyTimeoutMs: 60, maxRetries: 2 };

/**
 * @param info - A slot's facts.
 * @returns PRESET_INFO's reply fields, as the firmware sends them.
 */
function presetInfoBody(info: PresetInfo): number[] {
  return [
    ...encodeU14(info.slot),
    ...encodeU14(info.count),
    ...encodeU14(info.active),
    info.flags,
    ...encodeU14(info.revision),
    ...encodeU14(info.storedRevision),
    ...encodeS32(info.program),
    ...encodeLstring(info.name),
  ];
}

/** A device with three presets, as the firmware answers the preset commands (§4.5). */
class FakePresetDevice {
  names = ['Default', 'Preset 2', 'Preset 3'];
  active = 1;
  dirty = false;
  stored = new Set<number>();
  /** Every request: its command and payload. */
  received: Array<{ cmd: number; payload: number[] }> = [];
  #listeners = new Set<(bytes: Uint8Array) => void>();

  transport(): Transport {
    return {
      send: (bytes) => queueMicrotask(() => this.#handle(bytes)),
      subscribe: (listener) => {
        this.#listeners.add(listener);
        return () => this.#listeners.delete(listener);
      },
    };
  }

  info(slot: number): PresetInfo {
    const flags =
      (this.stored.has(slot) ? PresetFlag.Stored : 0) | (slot === this.active && this.dirty ? PresetFlag.Dirty : 0);
    return {
      slot,
      count: this.names.length,
      active: this.active,
      flags: flags | PresetFlag.Yaml,
      revision: 0,
      storedRevision: 0,
      program: -1,
      name: this.names[slot - 1] ?? '',
    };
  }

  #emit(bytes: Uint8Array): void {
    for (const listener of this.#listeners) {
      listener(bytes);
    }
  }

  #changed(): void {
    this.#emit(encodeFrame(Unsolicited.PresetChanged, 0, [...encodeU14(this.active), this.dirty ? 1 : 0]));
  }

  #handle(bytes: Uint8Array): void {
    const result = decodeFrame(bytes);
    if (result.ok === false) {
      return;
    }
    const { cmd, seq, payload } = result.frame;
    this.received.push({ cmd, payload: [...payload] });
    const reply = (fields: number[], status: number = Status.Ok) =>
      this.#emit(encodeFrame(cmd | 0x40, seq, [status, ...fields]));
    const slot = payload.length >= 2 ? decodeU14(payload, 0) : -1;
    const isSlot = slot >= 1 && slot <= this.names.length;
    switch (cmd) {
      case Command.PresetInfo:
        if (slot !== 0 && isSlot === false) {
          reply([], Status.BadParam);
          return;
        }
        reply(presetInfoBody(this.info(slot === 0 ? this.active : slot)));
        return;
      case Command.SetParam: {
        const reader = new PayloadReader(payload);
        const id = reader.u14();
        const index = reader.u14();
        const value = reader.s32();
        if (id === PresetParam.Active && value >= 1 && value <= this.names.length) {
          this.active = value;
          this.dirty = false;
          reply(payload);
          this.#changed();
          return;
        }
        reply([...encodeU14(id), ...encodeU14(index)], Status.BadValue);
        return;
      }
      case Command.PresetSave:
        if (isSlot === false) {
          reply([], Status.BadParam);
          return;
        }
        this.stored.add(slot);
        this.active = slot;
        this.dirty = false;
        reply([]);
        this.#changed();
        return;
      case Command.PresetReset:
        this.stored.delete(slot);
        reply([]);
        return;
      case Command.PresetName: {
        const name = new PayloadReader(payload.slice(2)).lstring();
        this.names[slot - 1] = name === '' ? `Preset ${slot}` : name;
        this.stored.add(slot);
        reply([]);
        return;
      }
      case Command.PresetDelete:
        if (isSlot === false) {
          reply([], Status.BadParam);
          return;
        }
        this.names.splice(slot - 1, 1);
        this.active = Math.min(this.active, this.names.length);
        reply([]);
        this.#changed();
        return;
      default:
        reply([], Status.BadCmd);
    }
  }
}

describe('presets (§4.5)', () => {
  it('parses PRESET_INFO, including a signed program and the name', () => {
    const info: PresetInfo = {
      slot: 2,
      count: 3,
      active: 2,
      flags: PresetFlag.Stored | PresetFlag.Dirty,
      revision: 7,
      storedRevision: 5,
      program: -1,
      name: 'Drums',
    };
    expect(parsePresetInfo(presetInfoBody(info))).toEqual(info);
    expect(isPresetDirty(info)).toBe(true);
    expect(isPresetStored(info)).toBe(true);
    expect(parsePresetInfo(presetInfoBody({ ...info, program: 127, name: '' })).program).toBe(127);
  });

  it('parses PRESET_CHANGED', () => {
    const frame = decodeFrame(encodeFrame(Unsolicited.PresetChanged, 0, [...encodeU14(3), 1]));
    expect(frame.ok && parseDeviceMessage(frame.frame)).toEqual({ kind: 'preset', slot: 3, dirty: true });
  });

  it('knows a device with presets from HELLO caps bit 5', () => {
    const hello = { role: Role.App, boardId: 2, fwVersion: 0, protoVersion: 1, maxChunk: 0, uid: [], nodeName: null };
    expect(offersPresets({ ...hello, caps: Capability.Config | Capability.Presets })).toBe(true);
    expect(offersPresets({ ...hello, caps: Capability.Config })).toBe(false);
    expect(offersPresets({ ...hello, role: Role.Boot, caps: Capability.Presets })).toBe(false);
    expect(offersPresets(null)).toBe(false);
  });

  it('checks names as the firmware does', () => {
    expect(presetNameProblem('Drums')).toBeNull();
    expect(presetNameProblem('')).toBeNull();
    expect(presetNameProblem('0123456789abcdef')).toBeNull();
    expect(presetNameProblem('0123456789abcdefg')).toMatch(/16 characters/);
    expect(presetNameProblem('Café')).toMatch(/ASCII/);
  });

  it('reads every slot, the active one first', async () => {
    const device = new FakePresetDevice();
    device.active = 2;
    const client = new SysexClient(device.transport(), FAST);
    const presets = await readPresets(client);
    expect(presets.map((info) => [info.slot, info.name])).toEqual([
      [1, 'Default'],
      [2, 'Preset 2'],
      [3, 'Preset 3'],
    ]);
    // 0 (the active one, for the count), then the two others: slot 2 is not asked for twice.
    expect(device.received.map(({ payload }) => decodeU14(payload, 0))).toEqual([0, 1, 3]);
  });

  it('selects with SET_PARAM preset.active and hears PRESET_CHANGED', async () => {
    const device = new FakePresetDevice();
    const client = new SysexClient(device.transport(), FAST);
    const heard: unknown[] = [];
    client.onMessage((message) => heard.push(message));
    await selectPreset(client, 3);
    expect(device.active).toBe(3);
    await Promise.resolve();
    expect(heard).toContainEqual({ kind: 'preset', slot: 3, dirty: false });
  });

  it('saves, renames and resets', async () => {
    const device = new FakePresetDevice();
    const client = new SysexClient(device.transport(), FAST);
    await savePreset(client, 2);
    expect(device.active).toBe(2);
    expect(device.stored.has(2)).toBe(true);
    await renamePreset(client, 2, 'Synth');
    expect((await getPresetInfo(client, 2)).name).toBe('Synth');
    await resetPreset(client, 2);
    expect(device.stored.has(2)).toBe(false);
    const name = device.received.find(({ cmd }) => cmd === Command.PresetName);
    expect(name?.payload).toEqual([2, 0, 5, ...Array.from('Synth', (c) => c.charCodeAt(0))]);
  });

  it('deletes with PRESET_DELETE and the slot', async () => {
    const device = new FakePresetDevice();
    const client = new SysexClient(device.transport(), FAST);
    await deletePreset(client, 2);
    expect(device.names).toEqual(['Default', 'Preset 3']);
    expect(device.received.at(-1)).toEqual({ cmd: Command.PresetDelete, payload: [2, 0] });
    await expect(deletePreset(client, 9)).rejects.toBeInstanceOf(RequestError);
  });

  it('sorts the slots into presets in use and free ones', () => {
    const slot = (n: number, flags: number): PresetInfo => ({
      slot: n,
      count: 4,
      active: 1,
      flags,
      revision: 0,
      storedRevision: 0,
      program: -1,
      name: `Preset ${n}`,
    });
    const { InUse, Yaml, Stored } = PresetFlag;
    const now = presetSlots([slot(1, InUse | Yaml), slot(2, InUse | Stored), slot(3, Yaml), slot(4, 0)], 1);
    expect([now.inUse.map((i) => i.slot), now.free.map((i) => i.slot), now.canDelete]).toEqual([[1, 2], [3, 4], true]);
    // Before PRESET_DELETE no slot reports bit 3: all are in use, the empty ones are where new presets go.
    const old = presetSlots([slot(1, Yaml), slot(2, Stored), slot(3, 0), slot(4, 0)], 3);
    expect([old.inUse.length, old.free.map((i) => i.slot), old.canDelete]).toEqual([4, [4], false]);
  });

  it('refuses a bad name before sending it, and passes on BAD_PARAM', async () => {
    const device = new FakePresetDevice();
    const client = new SysexClient(device.transport(), FAST);
    await expect(renamePreset(client, 1, 'x'.repeat(17))).rejects.toThrow(RangeError);
    expect(device.received).toHaveLength(0);
    await expect(getPresetInfo(client, 9)).rejects.toBeInstanceOf(RequestError);
  });
});

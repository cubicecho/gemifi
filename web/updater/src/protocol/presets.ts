/**
 * The preset requests (`protocol/sysex.md` §4.5): PRESET_INFO, PRESET_SAVE, PRESET_RESET and
 * PRESET_NAME, and selecting a preset (SET_PARAM `preset.active`). Framework-free: the caller
 * supplies the client. Only for a device with HELLO caps bit 5.
 */

import { RequestError, RequestFailure, type RequestOptions, type SysexClient } from './client.ts';
import { setParam } from './config.ts';
import { encodeLstring, encodeU14, PayloadReader } from './encoding.ts';
import type { HelloInfo } from './messages.ts';
import { Capability, Command, PRESET_ALL, PRESET_NAME_MAX, PresetFlag, PresetParam, Role } from './wire.ts';

/** What PRESET_INFO tells about one slot. */
export interface PresetInfo {
  /** The slot, 1-based. */
  slot: number;
  /** How many slots the device has. */
  count: number;
  /** The active slot. */
  active: number;
  /** {@link PresetFlag} bits. */
  flags: number;
  /** The firmware's presets revision. */
  revision: number;
  /** The revision the slot's stored edits were saved under; 0 without stored edits. */
  storedRevision: number;
  /** The Program Change sent when it is selected; -1 = none. */
  program: number;
  /** Its name: the stored one, else the YAML's, else "Preset N". */
  name: string;
}

/**
 * @param info - HELLO from the connected device, or null.
 * @returns Whether it is the app and has presets (§4.1 caps bit 5).
 */
export function offersPresets(info: HelloInfo | null): boolean {
  return info !== null && info.role === Role.App && (info.caps & Capability.Presets) !== 0;
}

/**
 * Parses PRESET_INFO's reply fields.
 *
 * @param body - The reply payload without its status byte.
 * @returns The slot's facts.
 */
export function parsePresetInfo(body: ArrayLike<number>): PresetInfo {
  const reader = new PayloadReader(body);
  return {
    slot: reader.u14(),
    count: reader.u14(),
    active: reader.u14(),
    flags: reader.byte(),
    revision: reader.u14(),
    storedRevision: reader.u14(),
    program: reader.s32(),
    name: reader.lstring(),
  };
}

/** A name with a character PRESET_NAME refuses (printable ASCII only, §2). */
const NOT_PRINTABLE = /[^\x20-\x7e]/;

/**
 * @param name - A proposed preset name.
 * @returns Why the device would refuse it, or null when it is fine. Empty is fine: it means the
 *   firmware's name again.
 */
export function presetNameProblem(name: string): string | null {
  if (name.length > PRESET_NAME_MAX) {
    return `At most ${PRESET_NAME_MAX} characters (this is ${name.length}).`;
  }
  if (NOT_PRINTABLE.test(name)) {
    return 'Letters, digits, spaces and ASCII punctuation only.';
  }
  return null;
}

/**
 * @param info - A slot's facts.
 * @returns Whether it has unsaved edits.
 */
export function isPresetDirty(info: PresetInfo): boolean {
  return (info.flags & PresetFlag.Dirty) !== 0;
}

/**
 * @param info - A slot's facts.
 * @returns Whether it has stored edits (changed values or a name).
 */
export function isPresetStored(info: PresetInfo): boolean {
  return (info.flags & PresetFlag.Stored) !== 0;
}

/**
 * Reads one slot's facts.
 *
 * @param client - The device.
 * @param slot - The slot, or {@link PRESET_ALL} (0) for the active one.
 * @param options - An abort signal.
 * @returns The facts.
 */
export async function getPresetInfo(
  client: SysexClient,
  slot: number,
  options: RequestOptions = {},
): Promise<PresetInfo> {
  const info = parsePresetInfo(await client.request(Command.PresetInfo, encodeU14(slot), options));
  if (slot !== PRESET_ALL && info.slot !== slot) {
    throw new RequestError(RequestFailure.BadReply, `PRESET_INFO for slot ${slot} answered for slot ${info.slot}.`);
  }
  return info;
}

/**
 * Reads every slot's facts: the active one first (for the count), then each slot in order.
 *
 * @param client - The device.
 * @param options - An abort signal.
 * @returns One entry per slot, slot 1 first.
 */
export async function readPresets(client: SysexClient, options: RequestOptions = {}): Promise<PresetInfo[]> {
  const first = await getPresetInfo(client, PRESET_ALL, options);
  const slots: PresetInfo[] = [];
  for (let slot = 1; slot <= first.count; slot++) {
    slots.push(slot === first.slot ? first : await getPresetInfo(client, slot, options));
  }
  return slots;
}

/**
 * Selects a preset (SET_PARAM `preset.active`). The device drops the active one's unsaved edits,
 * loads the slot and sends PRESET_CHANGED; selecting the active slot reloads it.
 *
 * @param client - The device.
 * @param slot - The slot, 1..count.
 * @param options - An abort signal.
 */
export async function selectPreset(client: SysexClient, slot: number, options: RequestOptions = {}): Promise<void> {
  await setParam(client, { id: PresetParam.Active, index: 0, value: slot }, options);
}

/**
 * Stores the live values into `slot` and makes it active ("save as"; the active slot = COMMIT).
 *
 * @param client - The device.
 * @param slot - The slot, 1..count.
 * @param options - An abort signal.
 */
export async function savePreset(client: SysexClient, slot: number, options: RequestOptions = {}): Promise<void> {
  await client.request(Command.PresetSave, encodeU14(slot), options);
}

/**
 * Drops a slot's stored edits and name, so it is the firmware's preset again; the active one is
 * reloaded, unsaved edits included.
 *
 * @param client - The device.
 * @param slot - The slot, or {@link PRESET_ALL} (0) for every slot.
 * @param options - An abort signal.
 */
export async function resetPreset(client: SysexClient, slot: number, options: RequestOptions = {}): Promise<void> {
  await client.request(Command.PresetReset, encodeU14(slot), options);
}

/**
 * Stores a new name for a slot at once (no COMMIT needed). Empty goes back to the firmware's name.
 *
 * @param client - The device.
 * @param slot - The slot, 1..count.
 * @param name - At most 16 printable ASCII characters.
 * @param options - An abort signal.
 */
export async function renamePreset(
  client: SysexClient,
  slot: number,
  name: string,
  options: RequestOptions = {},
): Promise<void> {
  const problem = presetNameProblem(name);
  if (problem !== null) {
    throw new RangeError(problem);
  }
  await client.request(Command.PresetName, [...encodeU14(slot), ...encodeLstring(name)], options);
}

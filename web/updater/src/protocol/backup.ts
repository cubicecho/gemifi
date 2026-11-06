/**
 * Backups of a device's settings and presets, in the host tool's format (`tools/host/gemifi.py
 * backup` / `restore`, `protocol/sysex.md` §4.5 "Backups"). Format 1 holds the values of a device
 * without presets (or the settings as they are now); format 2 holds the shared settings and every
 * preset in use. Framework-free: the caller supplies the client.
 */

import type { RequestOptions, SysexClient } from './client.ts';
import { commit, readParamValues, writeParamValues } from './config.ts';
import { formatVersion, type HelloInfo } from './messages.ts';
import {
  deletePreset,
  getPresetInfo,
  isPresetDirty,
  isPresetStored,
  offersPresets,
  type PresetInfo,
  presetSlots,
  readPresets,
  renamePreset,
  resetPreset,
  savePreset,
  selectPreset,
} from './presets.ts';
import { applyImport, planImport } from './profile.ts';
import { isPreset, isReadOnly, type ParamSpec, type Schema } from './schema.ts';
import { BOARD_ID_DIGITS, HEX_RADIX, PresetParam } from './wire.ts';

/** `gemifi_backup` of a file without presets: `values` only. */
export const BACKUP_FORMAT = 1;
/** `gemifi_backup` of a file with presets: the shared `values`, `active` and `presets`. */
export const BACKUP_FORMAT_PRESETS = 2;

/** Values by param name: a number for a plain param, a list for an array. */
export type BackupValues = Record<string, number | number[]>;

/** One preset in a format-2 backup. */
export interface BackupPreset {
  slot: number;
  name: string;
  /** The slot had stored edits (changed values, a name, or both). */
  stored: boolean;
  /** The presets revision those edits were stored under. */
  stored_revision: number;
  /** Its per-preset values. */
  values: BackupValues;
  /** Set on the active preset when it had unsaved edits: `values` has them. */
  unsaved?: boolean;
}

/** A backup file. */
export interface BackupDoc {
  gemifi_backup: number;
  board_id: number;
  /** As text, e.g. `0.2.62`. */
  fw_version: string;
  /** Format 2: the firmware's presets revision. */
  presets_revision?: number;
  /** Format 2: the preset that was active. */
  active?: number;
  /** Format 1: every value. Format 2: the shared ones. */
  values: BackupValues;
  /** Format 2: every preset in use. */
  presets?: BackupPreset[];
}

/** Where reading a backup is. */
export interface BackupProgress {
  /** Presets read so far. */
  done: number;
  /** Presets to read; 0 for a device without presets. */
  total: number;
}

/**
 * @param params - The params to read.
 * @param client - The device.
 * @param options - An abort signal.
 * @returns Their values by name.
 */
async function readValues(
  client: SysexClient,
  params: readonly ParamSpec[],
  options: RequestOptions,
): Promise<BackupValues> {
  const out: BackupValues = {};
  for (const param of params) {
    const values = await readParamValues(client, param, options);
    out[param.name] = param.count === 1 ? (values[0] ?? 0) : values;
  }
  return out;
}

/**
 * @param schema - The device's schema.
 * @returns The params each preset has its own value of.
 */
function presetParams(schema: Schema): ParamSpec[] {
  return schema.params.filter(isPreset);
}

/**
 * @param schema - The device's schema.
 * @returns The rest, without `preset.active` (a backup keeps that as `active`) and `preset.dirty`.
 */
function globalParams(schema: Schema): ParamSpec[] {
  return schema.params.filter(
    (param) => isPreset(param) === false && param.id !== PresetParam.Active && param.id !== PresetParam.Dirty,
  );
}

/**
 * @param infos - Every slot, from {@link readPresets}.
 * @returns The presets in use (every slot on a firmware from before PRESET_DELETE).
 */
function presetsInUse(infos: PresetInfo[]): PresetInfo[] {
  return presetSlots(infos, infos[0]?.active ?? 1).inUse;
}

/**
 * Writes the values in `want` that differ from what the device has now.
 *
 * @param client - The device.
 * @param params - The params to compare.
 * @param want - The values to end with.
 * @param options - An abort signal.
 */
async function putBack(
  client: SysexClient,
  params: readonly ParamSpec[],
  want: BackupValues,
  options: RequestOptions,
): Promise<void> {
  for (const param of params) {
    const entry = want[param.name];
    if (entry === undefined || isReadOnly(param)) {
      continue;
    }
    const wanted = Array.isArray(entry) ? entry : [entry];
    const now = await readParamValues(client, param, options);
    const differs = wanted.some((value, i) => now[i] !== value);
    if (differs) {
      await writeParamValues(client, param.id, 0, wanted.slice(0, param.count), options);
    }
  }
}

/**
 * Reads the device into a backup, as `gemifi.py backup` does. With presets it selects every preset
 * in use in turn and reads its values (free slots are not backed up), then selects the one that was
 * active again and puts its unsaved edits back, so the device ends as it started.
 *
 * @param client - The device.
 * @param info - Its HELLO.
 * @param schema - Its schema.
 * @param options - An abort signal, and a progress callback.
 * @returns The backup.
 */
export async function readBackup(
  client: SysexClient,
  info: HelloInfo,
  schema: Schema,
  options: RequestOptions & { onProgress?: (progress: BackupProgress) => void } = {},
): Promise<BackupDoc> {
  const { onProgress, ...request } = options;
  const head = { board_id: info.boardId, fw_version: formatVersion(info.fwVersion) };
  const hasPresets = offersPresets(info) && schema.params.some((param) => param.id === PresetParam.Active);
  if (hasPresets === false) {
    onProgress?.({ done: 0, total: 0 });
    return { gemifi_backup: BACKUP_FORMAT, ...head, values: await readValues(client, schema.params, request) };
  }
  const infos = await readPresets(client, request);
  const first = infos[0];
  if (first === undefined) {
    throw new Error('The device reported no preset slots.');
  }
  const { active, revision } = first;
  const pp = presetParams(schema);
  const values = await readValues(client, globalParams(schema), request);
  const live = await readValues(client, pp, request);
  const activeInfo = infos.find((slot) => slot.slot === active);
  const isDirty = activeInfo !== undefined && isPresetDirty(activeInfo);
  const used = presetsInUse(infos);
  const presets: BackupPreset[] = [];
  let hasSwitched = false;
  onProgress?.({ done: 0, total: used.length });
  try {
    for (const slot of used) {
      let slotValues = live;
      if (slot.slot !== active) {
        await selectPreset(client, slot.slot, request);
        hasSwitched = true;
        slotValues = await readValues(client, pp, request);
      }
      presets.push({
        slot: slot.slot,
        name: slot.name,
        stored: isPresetStored(slot),
        stored_revision: slot.storedRevision,
        values: slotValues,
        ...(slot.slot === active && isDirty ? { unsaved: true } : {}),
      });
      onProgress?.({ done: presets.length, total: used.length });
    }
  } finally {
    if (hasSwitched) {
      await selectPreset(client, active, request);
      if (isDirty) {
        await putBack(client, pp, live, request);
      }
    }
  }
  return { gemifi_backup: BACKUP_FORMAT_PRESETS, ...head, presets_revision: revision, active, values, presets };
}

/** Two digits, for the file name's date. */
const pad2 = (n: number) => String(n).padStart(2, '0');

/**
 * The file name `gemifi.py flash` gives its automatic backup.
 *
 * @param info - The device's HELLO.
 * @param now - When.
 * @returns E.g. `gemifi-backup-0002-0.2.62-20261105-142233.json`.
 */
export function backupFileName(info: Pick<HelloInfo, 'boardId' | 'fwVersion'>, now: Date): string {
  const board = info.boardId.toString(HEX_RADIX).padStart(BOARD_ID_DIGITS, '0');
  const date = `${now.getFullYear()}${pad2(now.getMonth() + 1)}${pad2(now.getDate())}`;
  const time = `${pad2(now.getHours())}${pad2(now.getMinutes())}${pad2(now.getSeconds())}`;
  return `gemifi-backup-${board}-${formatVersion(info.fwVersion)}-${date}-${time}.json`;
}

/**
 * @param doc - A backup.
 * @returns Its JSON text, as the host tool writes it.
 */
export function backupText(doc: BackupDoc): string {
  return `${JSON.stringify(doc, null, 1)}\n`;
}

/**
 * @param doc - A backup.
 * @returns E.g. `24 params, 3 presets`.
 */
export function backupSummary(doc: BackupDoc): string {
  const count = (values: BackupValues) => Object.keys(values).length;
  const params = count(doc.values) + (doc.presets ?? []).reduce((n, preset) => n + count(preset.values), 0);
  return doc.presets === undefined ? `${params} params` : `${params} params, ${doc.presets.length} presets`;
}

/**
 * @param value - Anything.
 * @returns Whether it is a plain object.
 */
function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && Array.isArray(value) === false;
}

/**
 * @param json - A parsed file.
 * @returns Whether it is a format-2 backup (with presets) that {@link restoreBackup} can write back.
 */
export function isPresetsBackup(json: unknown): json is BackupDoc & { presets: BackupPreset[] } {
  return isRecord(json) && isRecord(json.values) && Array.isArray(json.presets);
}

/** What writing a backup back did. */
export interface RestoreReport {
  /** Values the device took. */
  applied: number;
  /** What was skipped or refused, one sentence each. */
  problems: string[];
}

/**
 * Writes values by name, run by run, as `gemifi.py restore` does: names the schema does not have
 * and read-only params are skipped and reported.
 *
 * @param client - The device.
 * @param params - The params the values may be for.
 * @param values - The values by name.
 * @param where - Put before each problem, e.g. `preset 2: `.
 * @param report - Where to add what happened.
 * @param options - An abort signal.
 */
async function restoreValues(
  client: SysexClient,
  params: readonly ParamSpec[],
  values: unknown,
  where: string,
  report: RestoreReport,
  options: RequestOptions,
): Promise<void> {
  const plan = planImport({ version: 1, groups: [], params: [...params] }, { values });
  const result = await applyImport(client, plan, options);
  report.applied += result.applied.length;
  const unknown = plan.unknownNames.map((name) => `${name}: not in this firmware's schema; skipped.`);
  report.problems.push(...[...unknown, ...plan.problems, ...result.failures].map((line) => `${where}${line}`));
}

/**
 * Writes a backup back, as `gemifi.py restore` does. With presets on both sides: the shared
 * values, then every preset in the backup (a free slot is named first, so it is a preset; then it
 * is reset, and if the backup has stored edits for it, selected, set, saved and named), then the
 * presets in use past the backup's last one are deleted, the preset that was active is selected
 * and everything is stored (COMMIT). Without presets on either side, the values are written and
 * stored; per-preset values go into the active preset.
 *
 * @param client - The device.
 * @param info - Its HELLO.
 * @param schema - Its schema.
 * @param doc - The backup (a parsed file).
 * @param options - An abort signal.
 * @returns What was written and what was not.
 */
export async function restoreBackup(
  client: SysexClient,
  info: HelloInfo,
  schema: Schema,
  doc: unknown,
  options: RequestOptions = {},
): Promise<RestoreReport> {
  if (isRecord(doc) === false || isRecord(doc.values) === false) {
    throw new Error('The file is not a gemifi backup: it has no "values" object.');
  }
  const report: RestoreReport = { applied: 0, problems: [] };
  if (typeof doc.board_id === 'number' && doc.board_id !== info.boardId) {
    report.problems.push(
      `The backup is from board ${doc.board_id}, this device is board ${info.boardId}; matched by name anyway.`,
    );
  }
  const hasPresets = offersPresets(info) && schema.params.some((param) => param.id === PresetParam.Active);
  const presets = Array.isArray(doc.presets) ? doc.presets.filter(isRecord) : null;
  if (presets === null || hasPresets === false) {
    let values: Record<string, unknown> = doc.values;
    if (presets !== null) {
      // Presets in the file but not on the device: the shared values and the active preset's.
      const active = presets.find((preset) => preset.slot === doc.active) ?? presets[0];
      values = { ...values, ...(isRecord(active?.values) ? active.values : {}) };
      report.problems.push('This firmware has no presets: only the active preset of the backup was written.');
    }
    await restoreValues(client, schema.params, values, '', report, options);
    await commit(client, options);
    return report;
  }
  await restorePresets(client, schema, doc, presets, report, options);
  return report;
}

/**
 * The format-2 half of {@link restoreBackup}.
 *
 * @param client - The device.
 * @param schema - Its schema.
 * @param doc - The backup.
 * @param presets - Its presets.
 * @param report - Where to add what happened.
 * @param options - An abort signal.
 */
async function restorePresets(
  client: SysexClient,
  schema: Schema,
  doc: Record<string, unknown>,
  presets: Record<string, unknown>[],
  report: RestoreReport,
  options: RequestOptions,
): Promise<void> {
  const infos = await readPresets(client, options);
  const count = infos[0]?.count ?? 1;
  const isTracked = presetSlots(infos, infos[0]?.active ?? 1).canDelete;
  let used = presetsInUse(infos).length;
  const shared: Record<string, unknown> = { ...(doc.values as Record<string, unknown>) };
  delete shared['preset.active'];
  delete shared['preset.count'];
  await restoreValues(client, globalParams(schema), shared, '', report, options);
  const pp = presetParams(schema);
  const done = new Set<number>();
  const slotOf = (preset: Record<string, unknown>) => (typeof preset.slot === 'number' ? preset.slot : 0);
  for (const preset of [...presets].sort((a, b) => slotOf(a) - slotOf(b))) {
    const slot = slotOf(preset);
    if (Number.isInteger(slot) === false || slot < 1 || slot > count) {
      report.problems.push(`Preset ${String(preset.slot)}: this device has presets 1..${count}; skipped.`);
      continue;
    }
    if (isTracked && slot > used) {
      await renamePreset(client, slot, '', options); // a free slot: now a preset (and the slots before it)
      used = slot;
    }
    await resetPreset(client, slot, options);
    done.add(slot);
    if (preset.stored !== true) {
      continue; // no stored edits in the backup: the firmware's preset as it is
    }
    await selectPreset(client, slot, options);
    await restoreValues(client, pp, preset.values ?? {}, `Preset ${slot}: `, report, options);
    await savePreset(client, slot, options);
    const name = preset.name;
    if (typeof name === 'string' && name !== (await getPresetInfo(client, slot, options)).name) {
      try {
        await renamePreset(client, slot, name, options);
      } catch (error) {
        report.problems.push(`Preset ${slot}: name "${name}": ${error instanceof Error ? error.message : error}`);
      }
    }
  }
  const last = Math.max(0, ...done);
  if (isTracked && last > 0) {
    for (let slot = used; slot > last; slot--) {
      await deletePreset(client, slot, options); // presets the backup doesn't have, past its last one
    }
    used = Math.min(used, last);
  }
  const left = Array.from({ length: used }, (_, i) => i + 1).filter((slot) => done.has(slot) === false);
  if (left.length > 0) {
    report.problems.push(`Presets not in the backup, left as they are: ${left.join(', ')}.`);
  }
  const active = typeof doc.active === 'number' && doc.active >= 1 && doc.active <= used ? doc.active : 1;
  await selectPreset(client, active, options);
  await commit(client, options);
}

/**
 * @param infos - Every slot, read after a firmware upload.
 * @returns The slots whose stored edits were made under another presets revision (§4.5 "After a
 *   firmware upload"): the firmware's presets changed since, and the page asks what to keep.
 */
export function stalePresets(infos: readonly PresetInfo[]): PresetInfo[] {
  return infos.filter((slot) => isPresetStored(slot) && slot.storedRevision !== slot.revision);
}

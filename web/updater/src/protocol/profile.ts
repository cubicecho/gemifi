/**
 * A profile as JSON, the host tool's backup format 1 (`gemifi.py backup`, `./backup.ts`):
 * `{ gemifi_backup: 1, board_id, fw_version, values: { "<name>": value | [values...] } }`. Export
 * reads it off the loaded values; import checks a file against the schema and writes what passes.
 */

import { describeStatus, RequestError, RequestFailure, type RequestOptions, type SysexClient } from './client.ts';
import { type ParamValues, valueKey, writeParamValues } from './config.ts';
import { isDiagnostic } from './diagnostics.ts';
import { formatVersion, type ParamValue } from './messages.ts';
import { isReadOnly, type ParamSpec, type Schema } from './schema.ts';
import { S32_MAX, S32_MIN } from './wire.ts';

/** A profile file. */
export interface ProfileJson {
  /** The host tool's backup format: 1, values only. */
  gemifi_backup: 1;
  board_id: number;
  /** As text, e.g. `0.2.62` (files from older pages have the number). */
  fw_version: string;
  /** Plain params as a number, array params as a list of `count` numbers. */
  values: Record<string, number | number[]>;
}

/**
 * Builds the JSON for the values shown. The diagnostics (§4.10) are left out: they are facts about
 * this start of the device, not settings.
 *
 * @param schema - The device's schema.
 * @param values - Its values.
 * @param device - Its board id and firmware version.
 * @returns The profile.
 */
export function exportProfile(
  schema: Schema,
  values: ParamValues,
  device: { boardId: number; fwVersion: number },
): ProfileJson {
  const out: Record<string, number | number[]> = {};
  for (const param of schema.params.filter((p) => isDiagnostic(p) === false)) {
    const read = Array.from(
      { length: param.count },
      (_, i) => values.get(valueKey(param.id, i)) ?? param.defaults[i] ?? 0,
    );
    out[param.name] = param.count === 1 ? (read[0] ?? 0) : read;
  }
  return { gemifi_backup: 1, board_id: device.boardId, fw_version: formatVersion(device.fwVersion), values: out };
}

/** Consecutive values of one param to write. */
export interface ImportWrite {
  param: ParamSpec;
  /** The first index. */
  first: number;
  /** The values from `first` on. */
  values: number[];
}

/** What a profile file would change, before anything is sent. */
export interface ImportPlan {
  writes: ImportWrite[];
  /** Names the schema does not have; skipped. */
  unknownNames: string[];
  /** Read-only params in the file; skipped. */
  readOnlyNames: string[];
  /** Values that cannot be written, one sentence each. */
  problems: string[];
  /** Set when the file was saved from another board. */
  boardWarning: string | null;
}

/**
 * @param raw - One value from the file.
 * @returns It as an integer, or null when it is not one.
 */
function asInteger(raw: unknown): number | null {
  if (typeof raw === 'boolean') {
    return raw ? 1 : 0;
  }
  const isInt = typeof raw === 'number' && Number.isInteger(raw) && raw >= S32_MIN && raw <= S32_MAX;
  return isInt ? raw : null;
}

/**
 * @param value - Anything.
 * @returns Whether it is a plain object.
 */
function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && Array.isArray(value) === false;
}

/**
 * Checks one param's entry and splits the good values into consecutive runs.
 *
 * @param param - The param.
 * @param raw - Its entry in the file.
 * @param problems - Where to say what is wrong.
 * @returns The runs to write.
 */
function planParam(param: ParamSpec, raw: unknown, problems: string[]): ImportWrite[] {
  const isList = Array.isArray(raw);
  if (param.count > 1 && isList === false) {
    problems.push(`${param.name}: expected a list of ${param.count} values.`);
    return [];
  }
  const list: unknown[] = isList ? raw : [raw];
  if (list.length > param.count) {
    problems.push(`${param.name}: has ${list.length} values, but the device has ${param.count}; the rest are ignored.`);
  }
  const writes: ImportWrite[] = [];
  let run: ImportWrite | null = null;
  const count = Math.min(list.length, param.count);
  for (let index = 0; index < count; index++) {
    const entry = list[index];
    const value = asInteger(entry);
    const at = param.count === 1 ? param.name : `${param.name}[${index}]`;
    if (value === null) {
      problems.push(`${at}: ${JSON.stringify(entry)} is not a whole number.`);
      run = null;
      continue;
    }
    if (value < param.min || value > param.max) {
      problems.push(`${at}: ${value} is outside ${param.min}..${param.max}.`);
      run = null;
      continue;
    }
    if (run === null) {
      run = { param, first: index, values: [] };
      writes.push(run);
    }
    run.values.push(value);
  }
  return writes;
}

/**
 * Checks a parsed profile file against the schema.
 *
 * @param schema - The device's schema.
 * @param json - The parsed file.
 * @param boardId - The device's board id, to warn about a file from another board.
 * @returns What would be written, and what is skipped or wrong.
 */
export function planImport(schema: Schema, json: unknown, boardId?: number): ImportPlan {
  const plan: ImportPlan = { writes: [], unknownNames: [], readOnlyNames: [], problems: [], boardWarning: null };
  if (isRecord(json) === false || isRecord(json.values) === false) {
    plan.problems.push('The file is not a gemifi profile: it has no "values" object.');
    return plan;
  }
  const isOtherBoard = boardId !== undefined && typeof json.board_id === 'number' && json.board_id !== boardId;
  if (isOtherBoard) {
    plan.boardWarning = `The file was saved from board ${json.board_id}; this device is board ${boardId}.`;
  }
  const byName = new Map(schema.params.map((param) => [param.name, param]));
  for (const [name, raw] of Object.entries(json.values)) {
    const param = byName.get(name);
    if (param === undefined) {
      plan.unknownNames.push(name);
    } else if (isReadOnly(param)) {
      plan.readOnlyNames.push(name);
    } else {
      plan.writes.push(...planParam(param, raw, plan.problems));
    }
  }
  return plan;
}

/** What writing an import did. */
export interface ImportResult {
  /** The values the device took. */
  applied: ParamValue[];
  /** Runs the device refused, one sentence each. */
  failures: string[];
}

/**
 * Writes an import plan, run by run. A run the device refuses is reported and the rest still go.
 *
 * @param client - The device.
 * @param plan - From {@link planImport}.
 * @param options - An abort signal.
 * @returns What was applied and what failed.
 */
export async function applyImport(
  client: SysexClient,
  plan: ImportPlan,
  options: RequestOptions = {},
): Promise<ImportResult> {
  const result: ImportResult = { applied: [], failures: [] };
  for (const { param, first, values } of plan.writes) {
    try {
      await writeParamValues(client, param.id, first, values, options);
      for (const [i, value] of values.entries()) {
        result.applied.push({ id: param.id, index: first + i, value });
      }
    } catch (error) {
      const isStopped = error instanceof RequestError && error.failure === RequestFailure.Aborted;
      if (isStopped) {
        throw error;
      }
      const last = first + values.length - 1;
      const at = param.count === 1 ? param.name : `${param.name}[${first === last ? first : `${first}..${last}`}]`;
      const reason =
        error instanceof RequestError && error.status !== undefined
          ? describeStatus(error.status)
          : error instanceof Error
            ? error.message
            : String(error);
      result.failures.push(`${at}: ${reason}`);
    }
  }
  return result;
}

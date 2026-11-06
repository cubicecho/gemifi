/**
 * The diagnostics params (`protocol/sysex.md` §4.10): read-only, VOLATILE values in the group
 * "Debug" that say why the app started and how long its scans take. A host shows them with the
 * device info, not as settings. Framework-free: the caller supplies the client.
 */

import { RequestError, RequestFailure, type RequestOptions, type SysexClient } from './client.ts';
import { getParam } from './config.ts';
import type { ParamSpec, Schema } from './schema.ts';
import { DIAG_PARAM_FIRST, DIAG_PARAM_LAST, DiagParam, ParamType, Status } from './wire.ts';

/** One diagnostic as read from the device. */
export interface Diagnostic {
  /** Its schema entry: id, name, label, unit and enum options. */
  param: ParamSpec;
  /** The value read. */
  value: number;
  /** The value in words, e.g. `watchdog`, `412 µs`. */
  text: string;
}

/**
 * @param param - A param.
 * @returns Whether it is one of the diagnostics (§4.10, ids 65–74).
 */
export function isDiagnostic(param: Pick<ParamSpec, 'id'>): boolean {
  return param.id >= DIAG_PARAM_FIRST && param.id <= DIAG_PARAM_LAST;
}

/** Seconds in a minute. */
const SECONDS_PER_MINUTE = 60;
/** Seconds in an hour. */
const SECONDS_PER_HOUR = 3600;
/** Seconds in a day. */
const SECONDS_PER_DAY = 86_400;

/**
 * @param seconds - A duration.
 * @returns E.g. `2 d 3 h`, `1 h 5 min`, `4 min 10 s`, `42 s`.
 */
export function formatDuration(seconds: number): string {
  const days = Math.floor(seconds / SECONDS_PER_DAY);
  const hours = Math.floor((seconds % SECONDS_PER_DAY) / SECONDS_PER_HOUR);
  const minutes = Math.floor((seconds % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE);
  const rest = seconds % SECONDS_PER_MINUTE;
  if (days > 0) {
    return `${days} d ${hours} h`;
  }
  if (hours > 0) {
    return `${hours} h ${minutes} min`;
  }
  if (minutes > 0) {
    return `${minutes} min ${rest} s`;
  }
  return `${rest} s`;
}

/**
 * Words for one diagnostic's value: an enum's option label from the schema, yes / no for a bool,
 * the number with its unit otherwise.
 *
 * @param param - Its schema entry.
 * @param value - The value read.
 * @returns The value in words.
 */
export function formatDiagnostic(param: ParamSpec, value: number): string {
  if (param.type === ParamType.Enum) {
    return param.options?.[value - param.min] ?? String(value);
  }
  if (param.type === ParamType.Bool) {
    return value === 0 ? 'no' : 'yes';
  }
  if (param.id === DiagParam.ScanMax && value === 0) {
    return 'not timed on this platform';
  }
  if (param.id === DiagParam.Uptime) {
    return formatDuration(value);
  }
  return param.unit === null ? String(value) : `${value} ${param.unit}`;
}

/**
 * @param error - What a request threw.
 * @returns Whether the device does not have the param (BAD_PARAM): "not reported".
 */
function isMissing(error: unknown): boolean {
  return error instanceof RequestError && error.failure === RequestFailure.Status && error.status === Status.BadParam;
}

/**
 * Reads every diagnostic the schema lists, in id order. One the schema leaves out, or the device
 * answers BAD_PARAM for, is not reported and left out.
 *
 * @param client - The device.
 * @param schema - Its schema.
 * @param options - An abort signal.
 * @returns The diagnostics it reports.
 */
export async function readDiagnostics(
  client: SysexClient,
  schema: Schema,
  options: RequestOptions = {},
): Promise<Diagnostic[]> {
  const params = schema.params.filter(isDiagnostic).sort((a, b) => a.id - b.id);
  const read: Diagnostic[] = [];
  for (const param of params) {
    try {
      const value = await getParam(client, param.id, 0, options);
      read.push({ param, value, text: formatDiagnostic(param, value) });
    } catch (error) {
      if (isMissing(error) === false) {
        throw error;
      }
    }
  }
  return read;
}

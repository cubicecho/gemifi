/**
 * The config requests (`protocol/sysex.md` §4.3): read the schema in windows, read and write
 * values one at a time or in runs, COMMIT and FACTORY_RESET. Framework-free: the caller supplies
 * the client.
 */

import { RequestError, RequestFailure, type RequestOptions, type SysexClient } from './client.ts';
import {
  getParamPayload,
  getParamsPayload,
  getSchemaPayload,
  type ParamValue,
  parseParamRun,
  parseParamRunEcho,
  parseParamValue,
  parseSchemaWindow,
  setParamPayload,
  setParamsPayload,
} from './messages.ts';
import { type ParamSpec, parseSchema, type Schema } from './schema.ts';
import { Command, PARAMS_RUN_MAX, SCHEMA_WINDOW_BYTES, Status } from './wire.ts';

/** Every value of a profile, by {@link valueKey}. */
export type ParamValues = ReadonlyMap<string, number>;

/**
 * @param id - A param's id.
 * @param index - An index within it.
 * @returns The key of that value in {@link ParamValues}.
 */
export function valueKey(id: number, index: number): string {
  return `${id}:${index}`;
}

/**
 * @param message - What the reply got wrong.
 * @returns The error to throw.
 */
function badReply(message: string): RequestError {
  return new RequestError(RequestFailure.BadReply, message);
}

/**
 * Reads the whole schema, 64 bytes at a time, until the device's `total` is reached.
 *
 * @param client - The device.
 * @param options - An abort signal.
 * @returns The unpacked schema bytes.
 */
export async function readSchemaBytes(client: SysexClient, options: RequestOptions = {}): Promise<number[]> {
  const bytes: number[] = [];
  let total = Number.POSITIVE_INFINITY;
  while (bytes.length < total) {
    const offset = bytes.length;
    const reply = await client.request(Command.GetSchema, getSchemaPayload(offset, SCHEMA_WINDOW_BYTES), options);
    const window = parseSchemaWindow(reply);
    if (window.offset !== offset) {
      throw badReply(`GET_SCHEMA for offset ${offset} answered offset ${window.offset}.`);
    }
    if (window.data.length > SCHEMA_WINDOW_BYTES) {
      throw badReply(`GET_SCHEMA sent ${window.data.length} bytes when ${SCHEMA_WINDOW_BYTES} were asked for.`);
    }
    total = window.total;
    const isStuck = window.data.length === 0 && offset < total;
    if (isStuck) {
      throw badReply(`GET_SCHEMA sent no data at offset ${offset} of ${total}.`);
    }
    bytes.push(...window.data);
  }
  return bytes.slice(0, total);
}

/**
 * Reads and parses the schema.
 *
 * @param client - The device.
 * @param options - An abort signal.
 * @returns The groups and params.
 */
export async function getSchema(client: SysexClient, options: RequestOptions = {}): Promise<Schema> {
  return parseSchema(await readSchemaBytes(client, options));
}

/**
 * @param asked - The address requested.
 * @param got - The address the reply names.
 * @param command - The command's name, for the error.
 */
function checkEcho(asked: { id: number; index: number }, got: { id: number; index: number }, command: string): void {
  const isSame = asked.id === got.id && asked.index === got.index;
  if (isSame === false) {
    throw badReply(`${command} for ${asked.id}[${asked.index}] answered for ${got.id}[${got.index}].`);
  }
}

/**
 * Reads one value.
 *
 * @param client - The device.
 * @param id - The param.
 * @param index - The index within it.
 * @param options - An abort signal.
 * @returns The value.
 */
export async function getParam(
  client: SysexClient,
  id: number,
  index: number,
  options: RequestOptions = {},
): Promise<number> {
  const reply = parseParamValue(await client.request(Command.GetParam, getParamPayload(id, index), options));
  checkEcho({ id, index }, reply, 'GET_PARAM');
  return reply.value;
}

/**
 * Sets one value, live. BAD_VALUE and BAD_PARAM come back as a `status` {@link RequestError}.
 *
 * @param client - The device.
 * @param target - The param, the index and the new value.
 * @param options - An abort signal.
 * @returns The value now in effect.
 */
export async function setParam(client: SysexClient, target: ParamValue, options: RequestOptions = {}): Promise<number> {
  const reply = parseParamValue(await client.request(Command.SetParam, setParamPayload(target), options));
  checkEcho(target, reply, 'SET_PARAM');
  return reply.value;
}

/**
 * Reads a run of up to 16 values of one param.
 *
 * @param client - The device.
 * @param id - The param.
 * @param index - The first index.
 * @param n - How many, 1..16.
 * @param options - An abort signal.
 * @returns The values.
 */
export async function getParams(
  client: SysexClient,
  id: number,
  index: number,
  n: number,
  options: RequestOptions = {},
): Promise<number[]> {
  const reply = parseParamRun(await client.request(Command.GetParams, getParamsPayload(id, index, n), options));
  checkEcho({ id, index }, reply, 'GET_PARAMS');
  if (reply.values.length !== n) {
    throw badReply(`GET_PARAMS asked for ${n} values and got ${reply.values.length}.`);
  }
  return reply.values;
}

/**
 * Sets a run of up to 16 values of one param, all or none.
 *
 * @param client - The device.
 * @param id - The param.
 * @param index - The first index.
 * @param values - The values, 1..16.
 * @param options - An abort signal.
 */
export async function setParams(
  client: SysexClient,
  id: number,
  index: number,
  values: number[],
  options: RequestOptions = {},
): Promise<void> {
  const reply = parseParamRunEcho(
    await client.request(Command.SetParams, setParamsPayload({ id, index, values }), options),
  );
  checkEcho({ id, index }, reply, 'SET_PARAMS');
  if (reply.n !== values.length) {
    throw badReply(`SET_PARAMS sent ${values.length} values and the device acknowledged ${reply.n}.`);
  }
}

/** Clients whose device answered BAD_CMD to GET_PARAMS or SET_PARAMS, so runs are not tried again. */
const singleValueOnly = new WeakSet<SysexClient>();

/**
 * @param error - What a request threw.
 * @returns Whether the device does not know the command.
 */
function isBadCmd(error: unknown): boolean {
  return error instanceof RequestError && error.failure === RequestFailure.Status && error.status === Status.BadCmd;
}

/**
 * Splits `count` indices from `first` into runs of at most 16.
 *
 * @param first - The first index.
 * @param count - How many.
 * @returns `[index, n]` pairs.
 */
function runsOf(first: number, count: number): Array<[number, number]> {
  const runs: Array<[number, number]> = [];
  for (let index = first; index < first + count; index += PARAMS_RUN_MAX) {
    runs.push([index, Math.min(PARAMS_RUN_MAX, first + count - index)]);
  }
  return runs;
}

/**
 * Reads every value of one param: GET_PARAMS in runs of 16 for an array, falling back to
 * GET_PARAM one by one when the device answers BAD_CMD.
 *
 * @param client - The device.
 * @param param - The param.
 * @param options - An abort signal.
 * @returns `count` values.
 */
export async function readParamValues(
  client: SysexClient,
  param: ParamSpec,
  options: RequestOptions = {},
): Promise<number[]> {
  const useRuns = param.count > 1 && singleValueOnly.has(client) === false;
  if (useRuns) {
    try {
      const values: number[] = [];
      for (const [index, n] of runsOf(0, param.count)) {
        values.push(...(await getParams(client, param.id, index, n, options)));
      }
      return values;
    } catch (error) {
      if (isBadCmd(error) === false) {
        throw error;
      }
      singleValueOnly.add(client);
    }
  }
  const values: number[] = [];
  for (let index = 0; index < param.count; index++) {
    values.push(await getParam(client, param.id, index, options));
  }
  return values;
}

/**
 * Reads every value the schema lists.
 *
 * @param client - The device.
 * @param schema - Its schema.
 * @param options - An abort signal.
 * @returns The values.
 */
export async function readAllValues(
  client: SysexClient,
  schema: Schema,
  options: RequestOptions = {},
): Promise<Map<string, number>> {
  const values = new Map<string, number>();
  for (const param of schema.params) {
    const read = await readParamValues(client, param, options);
    for (const [index, value] of read.entries()) {
      values.set(valueKey(param.id, index), value);
    }
  }
  return values;
}

/**
 * Writes consecutive values of one param: SET_PARAMS in runs of 16, all or none per run, falling
 * back to SET_PARAM one by one when the device answers BAD_CMD. A single value is a SET_PARAM.
 *
 * @param client - The device.
 * @param id - The param.
 * @param first - The first index.
 * @param values - The values from `first` on.
 * @param options - An abort signal.
 */
export async function writeParamValues(
  client: SysexClient,
  id: number,
  first: number,
  values: number[],
  options: RequestOptions = {},
): Promise<void> {
  let written = 0;
  const useRuns = values.length > 1 && singleValueOnly.has(client) === false;
  if (useRuns) {
    try {
      for (const [index, n] of runsOf(first, values.length)) {
        await setParams(client, id, index, values.slice(index - first, index - first + n), options);
        written = index - first + n;
      }
      return;
    } catch (error) {
      if (isBadCmd(error) === false) {
        throw error;
      }
      singleValueOnly.add(client);
    }
  }
  for (let i = written; i < values.length; i++) {
    await setParam(client, { id, index: first + i, value: values[i] ?? 0 }, options);
  }
}

/**
 * Stores every non-VOLATILE value in data flash (COMMIT). Uses the long timeout.
 *
 * @param client - The device.
 * @param options - An abort signal.
 */
export async function commit(client: SysexClient, options: RequestOptions = {}): Promise<void> {
  await client.request(Command.Commit, [], options);
}

/**
 * Sets every value back to its default and erases the stored profile (FACTORY_RESET). Uses the
 * long timeout.
 *
 * @param client - The device.
 * @param options - An abort signal.
 */
export async function factoryReset(client: SysexClient, options: RequestOptions = {}): Promise<void> {
  await client.request(Command.FactoryReset, [], options);
}

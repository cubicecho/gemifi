import { describe, expect, it } from 'vitest';
import { RequestFailure, SysexClient, type Transport } from './client.ts';
import {
  commit,
  factoryReset,
  getParam,
  getParams,
  getSchema,
  readAllValues,
  readParamValues,
  readSchemaBytes,
  setParam,
  setParams,
  valueKey,
  writeParamValues,
} from './config.ts';
import { encodeS32, encodeU14, encodeU28, pack } from './encoding.ts';
import { decodeFrame, encodeFrame } from './frame.ts';
import {
  type DeviceMessage,
  encoderDelta,
  getParamPayload,
  getParamsPayload,
  getSchemaPayload,
  parseDeviceMessage,
  parseParamRun,
  parseParamRunEcho,
  parseParamValue,
  parseSchemaWindow,
  setParamPayload,
  setParamsPayload,
} from './messages.ts';
import { EXAMPLE_SCHEMA, FakeConfigDevice, makeParam } from './test-support.ts';
import { Command, Status } from './wire.ts';

const FAST = { replyTimeoutMs: 15, longReplyTimeoutMs: 60, maxRetries: 2 };

/**
 * @param device - The fake.
 * @returns A client on it.
 */
function clientFor(device: FakeConfigDevice): SysexClient {
  return new SysexClient(device.transport(), FAST);
}

describe('config payloads (§4)', () => {
  it('builds GET_SCHEMA with a u28 offset and a u14 max', () => {
    expect(getSchemaPayload(0x200, 64)).toEqual([0x00, 0x04, 0x00, 0x00, 0x40, 0x00]);
  });

  it('parses a GET_SCHEMA window', () => {
    const body = [...encodeU28(300), ...encodeU28(64), ...pack([0xff, 1, 2])];
    expect(parseSchemaWindow(body)).toEqual({ total: 300, offset: 64, data: [0xff, 1, 2] });
    expect(parseSchemaWindow([...encodeU28(10), ...encodeU28(10)])).toEqual({ total: 10, offset: 10, data: [] });
  });

  it('round-trips GET_PARAM, SET_PARAM and PARAM_CHANGED fields', () => {
    expect(getParamPayload(9, 6)).toEqual([9, 0, 6, 0]);
    const value = { id: 2, index: 0, value: -24 };
    const payload = setParamPayload(value);
    expect(payload).toEqual([2, 0, 0, 0, ...encodeS32(-24)]);
    expect(parseParamValue(payload)).toEqual(value);
    expect(parseParamValue(setParamPayload({ id: 0x3fff, index: 0x3fff, value: 0x7fff_ffff }))).toEqual({
      id: 0x3fff,
      index: 0x3fff,
      value: 0x7fff_ffff,
    });
  });

  it('round-trips GET_PARAMS and SET_PARAMS fields', () => {
    expect(getParamsPayload(10, 16, 7)).toEqual([10, 0, 16, 0, 7, 0]);
    const run = { id: 10, index: 2, values: [64, -1, 0] };
    const payload = setParamsPayload(run);
    expect(payload).toHaveLength(6 + 15);
    expect(parseParamRun(payload)).toEqual(run);
    expect(parseParamRunEcho([10, 0, 2, 0, 3, 0])).toEqual({ id: 10, index: 2, n: 3 });
  });

  it('refuses a short reply', () => {
    expect(() => parseParamValue([1, 0, 0, 0, 0x7f])).toThrow();
    expect(() => parseParamRun([...encodeU14(1), ...encodeU14(0), ...encodeU14(2), ...encodeS32(1)])).toThrow();
  });

  it('decodes PARAM_CHANGED as an unsolicited message', () => {
    const frame = { version: 1, cmd: 0x62, seq: 3, payload: [3, 0, 0, 0, ...encodeS32(-2)] };
    expect(parseDeviceMessage(frame)).toEqual({ kind: 'param', id: 3, index: 0, value: -2 });
  });

  it("reads an encoder delta as 14-bit two's complement", () => {
    expect(encoderDelta(1)).toBe(1);
    expect(encoderDelta(0x3fff)).toBe(-1);
    expect(encoderDelta(0x2000)).toBe(-0x2000);
    expect(encoderDelta(0x1fff)).toBe(0x1fff);
  });
});

describe('reading the schema', () => {
  it('reads it in windows of 64 until total', async () => {
    const device = new FakeConfigDevice();
    const client = clientFor(device);
    const bytes = await readSchemaBytes(client);
    expect(bytes).toEqual(device.schemaBytes);
    const windows = device.received.filter((c) => c === Command.GetSchema).length;
    expect(windows).toBe(Math.ceil(device.schemaBytes.length / 64));
    expect(await getSchema(client)).toEqual(EXAMPLE_SCHEMA);
  });

  it('retries a lost window with the same request', async () => {
    const device = new FakeConfigDevice({ dropReplies: [Command.GetSchema] });
    expect(await getSchema(clientFor(device))).toEqual(EXAMPLE_SCHEMA);
  });

  it('fails clearly on a wrong offset echo or a window with no data', async () => {
    const stuck: Transport = replyingTransport((cmd, seq) =>
      cmd === Command.GetSchema ? encodeFrame(0x42, seq, [Status.Ok, ...encodeU28(100), ...encodeU28(0)]) : null,
    );
    await expect(readSchemaBytes(new SysexClient(stuck, FAST))).rejects.toThrow(/no data at offset 0 of 100/);
    const wrong: Transport = replyingTransport((cmd, seq) =>
      cmd === Command.GetSchema ? encodeFrame(0x42, seq, [Status.Ok, ...encodeU28(100), ...encodeU28(5), 0, 1]) : null,
    );
    await expect(readSchemaBytes(new SysexClient(wrong, FAST))).rejects.toMatchObject({
      failure: RequestFailure.BadReply,
    });
  });
});

describe('values', () => {
  it('gets and sets one value, live', async () => {
    const device = new FakeConfigDevice();
    const client = clientFor(device);
    expect(await getParam(client, 1, 0)).toBe(1);
    expect(await setParam(client, { id: 2, index: 0, value: -12 })).toBe(-12);
    expect(device.value(2)).toBe(-12);
    expect(await getParam(client, 2, 0)).toBe(-12);
  });

  it('turns BAD_VALUE and BAD_PARAM into status errors', async () => {
    const client = clientFor(new FakeConfigDevice());
    await expect(setParam(client, { id: 1, index: 0, value: 17 })).rejects.toMatchObject({
      failure: RequestFailure.Status,
      status: Status.BadValue,
    });
    await expect(getParam(client, 99, 0)).rejects.toMatchObject({ status: Status.BadParam });
    await expect(getParam(client, 9, 7)).rejects.toMatchObject({ status: Status.BadParam });
  });

  it('reads and writes runs, all or none', async () => {
    const device = new FakeConfigDevice();
    const client = clientFor(device);
    expect(await getParams(client, 9, 0, 7)).toEqual([1, 0, 5, 6, 9, 10, 1]);
    await setParams(client, 10, 2, [3, 4]);
    expect([device.value(10, 2), device.value(10, 3)]).toEqual([3, 4]);
    await expect(setParams(client, 10, 0, [1, 200])).rejects.toMatchObject({ status: Status.BadValue });
    expect(device.value(10, 0)).toBe(64);
  });

  it('reads every value, arrays with GET_PARAMS in runs of 16', async () => {
    const big = makeParam({ id: 20, name: 'key.note', label: 'Note', count: 40, defaults: [60] });
    const schema = { ...EXAMPLE_SCHEMA, params: [...EXAMPLE_SCHEMA.params, big] };
    const device = new FakeConfigDevice({ schema });
    const values = await readAllValues(clientFor(device), schema);
    expect(values.get(valueKey(9, 4))).toBe(9);
    expect(values.get(valueKey(20, 39))).toBe(60);
    expect(values.size).toBe(7 + 7 + 7 + 40);
    // 2 arrays of 7 (1 run each) + 40 keys (3 runs) = 5 GET_PARAMS; 7 plain params by GET_PARAM.
    expect(device.received.filter((c) => c === Command.GetParams)).toHaveLength(5);
    expect(device.received.filter((c) => c === Command.GetParam)).toHaveLength(7);
  });

  it('falls back to single GET/SET_PARAM when the device answers BAD_CMD', async () => {
    const device = new FakeConfigDevice({ supportsRuns: false });
    const client = clientFor(device);
    const action = EXAMPLE_SCHEMA.params.find((p) => p.id === 9) ?? EXAMPLE_SCHEMA.params[0];
    if (action === undefined) {
      throw new Error('no param');
    }
    expect(await readParamValues(client, action)).toEqual([1, 0, 5, 6, 9, 10, 1]);
    await writeParamValues(client, 10, 0, [1, 2, 3]);
    expect([device.value(10, 0), device.value(10, 1), device.value(10, 2)]).toEqual([1, 2, 3]);
    // One probe of each, then no more runs.
    expect(device.received.filter((c) => c === Command.GetParams || c === Command.SetParams)).toHaveLength(1);
  });
});

describe('COMMIT and FACTORY_RESET', () => {
  it('commits and resets', async () => {
    const device = new FakeConfigDevice();
    const client = clientFor(device);
    await setParam(client, { id: 1, index: 0, value: 5 });
    await commit(client);
    expect(device.stored?.get('1:0')).toBe(5);
    await factoryReset(client);
    expect(device.stored).toBeNull();
    expect(device.value(1)).toBe(1);
  });

  it('reports FLASH_ERR', async () => {
    const device = new FakeConfigDevice();
    device.forcedStatus.set(Command.Commit, Status.FlashErr);
    await expect(commit(clientFor(device))).rejects.toMatchObject({ status: Status.FlashErr });
  });

  it('waits the long timeout for both, and the short one for SET_PARAM', async () => {
    const SLOW_MS = 35; // longer than replyTimeoutMs, shorter than longReplyTimeoutMs
    const sent: number[] = [];
    const slow = replyingTransport((cmd, seq) => {
      sent.push(cmd);
      return encodeFrame(
        cmd | 0x40,
        seq,
        cmd === Command.SetParam ? [Status.Ok, 1, 0, 0, 0, 1, 0, 0, 0, 0] : [Status.Ok],
      );
    }, SLOW_MS);
    const client = new SysexClient(slow, FAST);
    await commit(client);
    await factoryReset(client);
    expect(sent).toEqual([Command.Commit, Command.FactoryReset]);
    sent.length = 0;
    // The short timeout runs out first, so SET_PARAM is resent (same seq) before its reply lands.
    expect(await setParam(client, { id: 1, index: 0, value: 1 })).toBe(1);
    expect(sent.length).toBeGreaterThan(1);
  });
});

describe('PARAM_CHANGED through the client', () => {
  it('reaches message listeners', async () => {
    const device = new FakeConfigDevice();
    const client = clientFor(device);
    const seen: DeviceMessage[] = [];
    client.onMessage((m) => seen.push(m));
    device.changeByItself(3, 0, -1);
    expect(seen).toEqual([{ kind: 'param', id: 3, index: 0, value: -1 }]);
  });
});

/**
 * A transport whose device answers each request with `answer`, after a delay.
 *
 * @param answer - The reply frame for a request, or null for none.
 * @param delayMs - How long the device takes.
 * @returns The transport.
 */
function replyingTransport(answer: (cmd: number, seq: number) => Uint8Array | null, delayMs = 0): Transport {
  const listeners = new Set<(bytes: Uint8Array) => void>();
  return {
    send: (bytes) => {
      const result = decodeFrame(bytes);
      if (result.ok === false) {
        return;
      }
      const reply = answer(result.frame.cmd, result.frame.seq);
      if (reply !== null) {
        setTimeout(() => {
          for (const listener of listeners) {
            listener(reply);
          }
        }, delayMs);
      }
    },
    subscribe: (listener) => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}

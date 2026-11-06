import { describe, expect, it } from 'vitest';
import { SysexClient } from './client.ts';
import { readAllValues, valueKey } from './config.ts';
import { applyImport, exportProfile, planImport } from './profile.ts';
import { EXAMPLE_SCHEMA, FakeConfigDevice, makeParam } from './test-support.ts';
import { Command, ParamFlag, Status } from './wire.ts';

const FAST = { replyTimeoutMs: 15, longReplyTimeoutMs: 60, maxRetries: 2 };
const DEVICE = { boardId: 1, fwVersion: 0x0001_0000 };

describe('exportProfile', () => {
  it('writes plain params as numbers and arrays as lists, by name', async () => {
    const device = new FakeConfigDevice();
    const values = await readAllValues(new SysexClient(device.transport(), FAST), EXAMPLE_SCHEMA);
    const profile = exportProfile(EXAMPLE_SCHEMA, values, DEVICE);
    expect(profile.board_id).toBe(1);
    expect(profile.fw_version).toBe('0.1.0');
    expect(profile.gemifi_backup).toBe(1);
    expect(profile.values['midi.channel']).toBe(1);
    expect(profile.values['button.action']).toEqual([1, 0, 5, 6, 9, 10, 1]);
    expect(Object.keys(profile.values)).toHaveLength(EXAMPLE_SCHEMA.params.length);
  });

  it('round-trips through planImport', () => {
    const values = new Map([[valueKey(2, 0), -3]]);
    const profile = JSON.parse(JSON.stringify(exportProfile(EXAMPLE_SCHEMA, values, DEVICE)));
    const plan = planImport(EXAMPLE_SCHEMA, profile, 1);
    expect(plan.problems).toEqual([]);
    expect(plan.unknownNames).toEqual([]);
    const transpose = plan.writes.find((w) => w.param.name === 'midi.transpose');
    expect(transpose?.values).toEqual([-3]);
    expect(plan.writes.find((w) => w.param.name === 'button.number')?.values).toEqual([64, 0, 0, 0, 0, 0, 1]);
  });
});

describe('planImport', () => {
  it('skips unknown and read-only names, and warns about another board', () => {
    const ro = makeParam({ id: 30, name: 'fw.build', label: 'Build', flags: ParamFlag.ReadOnly });
    const schema = { ...EXAMPLE_SCHEMA, params: [...EXAMPLE_SCHEMA.params, ro] };
    const plan = planImport(schema, { board_id: 2, values: { 'midi.channel': 3, 'no.such': 1, 'fw.build': 5 } }, 1);
    expect(plan.unknownNames).toEqual(['no.such']);
    expect(plan.readOnlyNames).toEqual(['fw.build']);
    expect(plan.boardWarning).toMatch(/board 2/);
    expect(plan.writes.map((w) => [w.param.name, w.first, w.values])).toEqual([['midi.channel', 0, [3]]]);
  });

  it('reports bad values and splits the good ones into runs', () => {
    const plan = planImport(EXAMPLE_SCHEMA, {
      values: {
        'button.number': [1, 2, 300, 4, 'x', 6, 7, 8],
        'midi.channel': 0,
        'velocity.curve': 1.5,
        'knob.cc': [1],
        'debug.events': true,
      },
    });
    expect(plan.writes.map((w) => [w.param.name, w.first, w.values])).toEqual([
      ['button.number', 0, [1, 2]],
      ['button.number', 3, [4]],
      ['button.number', 5, [6, 7]],
      ['knob.cc', 0, [1]],
      ['debug.events', 0, [1]],
    ]);
    expect(plan.problems).toEqual([
      'button.number: has 8 values, but the device has 7; the rest are ignored.',
      'button.number[2]: 300 is outside 0..127.',
      'button.number[4]: "x" is not a whole number.',
      'midi.channel: 0 is outside 1..16.',
      'velocity.curve: 1.5 is not a whole number.',
    ]);
  });

  it('wants a list for an array, and a values object', () => {
    expect(planImport(EXAMPLE_SCHEMA, { values: { 'button.action': 3 } }).problems).toEqual([
      'button.action: expected a list of 7 values.',
    ]);
    expect(planImport(EXAMPLE_SCHEMA, [1, 2]).problems[0]).toMatch(/no "values" object/);
    expect(planImport(EXAMPLE_SCHEMA, null).writes).toEqual([]);
  });
});

describe('applyImport', () => {
  it('writes with SET_PARAMS and reports what the device refused', async () => {
    const device = new FakeConfigDevice();
    const client = new SysexClient(device.transport(), FAST);
    const plan = planImport(EXAMPLE_SCHEMA, { values: { 'button.number': [9, 8, 7, 6, 5, 4, 3], 'midi.octave': 2 } });
    device.forcedStatus.set(Command.SetParam, Status.BadValue);
    const result = await applyImport(client, plan);
    expect(result.failures).toEqual(["midi.octave: BAD_VALUE (the value is outside the param's range)"]);
    expect(result.applied).toHaveLength(7);
    expect(device.value(10, 6)).toBe(3);
    expect(device.received).toContain(Command.SetParams);
  });

  it('falls back to SET_PARAM on a device without runs', async () => {
    const device = new FakeConfigDevice({ supportsRuns: false });
    const client = new SysexClient(device.transport(), FAST);
    const plan = planImport(EXAMPLE_SCHEMA, { values: { 'button.number': [9, 8, 7, 6, 5, 4, 3] } });
    const result = await applyImport(client, plan);
    expect(result.failures).toEqual([]);
    expect(device.value(10, 0)).toBe(9);
    expect(device.received.filter((c) => c === Command.SetParam)).toHaveLength(7);
  });
});

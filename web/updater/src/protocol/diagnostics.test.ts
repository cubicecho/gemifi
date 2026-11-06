import { describe, expect, it } from 'vitest';
import { SysexClient } from './client.ts';
import { formatDiagnostic, formatDuration, isDiagnostic, readDiagnostics } from './diagnostics.ts';
import { exportProfile } from './profile.ts';
import { EXAMPLE_SCHEMA, FakeConfigDevice, makeParam } from './test-support.ts';
import { Command, DiagParam, ParamFlag, ParamType, Status } from './wire.ts';

const FAST = { replyTimeoutMs: 15, longReplyTimeoutMs: 60, maxRetries: 2 };
const RO = ParamFlag.ReadOnly | ParamFlag.Volatile;

const RESET_REASONS = ['unknown', 'power-on', 'reset pin', 'software', 'watchdog', 'brown-out', 'crash', 'low-power'];

/** The §4.8 params as a device YAML build lists them, in the Debug group (4). */
const DIAG_PARAMS = [
  makeParam({
    id: DiagParam.ResetReason,
    name: 'diag.reset_reason',
    label: 'Reset reason',
    group: 4,
    type: ParamType.Enum,
    max: 7,
    options: RESET_REASONS,
    flags: RO,
  }),
  makeParam({
    id: DiagParam.BootLoop,
    name: 'diag.boot_loop',
    label: 'Boot loop',
    group: 4,
    type: ParamType.Bool,
    max: 1,
    flags: RO,
  }),
  makeParam({
    id: DiagParam.ScanMax,
    name: 'diag.scan_max',
    label: 'Longest scan',
    group: 4,
    max: 100_000,
    unit: 'µs',
    flags: RO,
  }),
  makeParam({
    id: DiagParam.Uptime,
    name: 'diag.uptime',
    label: 'Uptime',
    group: 4,
    max: 0x7fff_ffff,
    unit: 's',
    flags: RO,
  }),
];

describe('diagnostics (§4.8)', () => {
  it('knows the diagnostics ids', () => {
    expect(isDiagnostic({ id: 65 })).toBe(true);
    expect(isDiagnostic({ id: 74 })).toBe(true);
    expect(isDiagnostic({ id: 13 })).toBe(false);
    expect(isDiagnostic({ id: 75 })).toBe(false);
  });

  it('words the values: enum labels from the schema, yes / no, units, a duration', () => {
    const [reason, bootLoop, scan, uptime] = DIAG_PARAMS;
    if (reason === undefined || bootLoop === undefined || scan === undefined || uptime === undefined) {
      throw new Error('missing params');
    }
    expect(formatDiagnostic(reason, 4)).toBe('watchdog');
    expect(formatDiagnostic(reason, 9)).toBe('9');
    expect(formatDiagnostic(bootLoop, 1)).toBe('yes');
    expect(formatDiagnostic(scan, 412)).toBe('412 µs');
    expect(formatDiagnostic(scan, 0)).toBe('not timed on this platform');
    expect(formatDiagnostic(uptime, 3725)).toBe('1 h 2 min');
    expect(formatDuration(42)).toBe('42 s');
    expect(formatDuration(250)).toBe('4 min 10 s');
    expect(formatDuration(2 * 86_400 + 3 * 3600)).toBe('2 d 3 h');
  });

  it('reads what the schema lists and leaves out what the device does not report', async () => {
    const schema = { ...EXAMPLE_SCHEMA, params: [...EXAMPLE_SCHEMA.params, ...DIAG_PARAMS] };
    const device = new FakeConfigDevice({ schema });
    device.values.set(`${DiagParam.ResetReason}:0`, 3);
    device.values.set(`${DiagParam.ScanMax}:0`, 380);
    device.values.set(`${DiagParam.Uptime}:0`, 61);
    const client = new SysexClient(device.transport(), FAST);
    // Answered BAD_PARAM (an id in the schema the device does not have after all): not reported.
    device.forcedStatus.set(Command.GetParam, Status.BadParam);
    const read = await readDiagnostics(client, schema);
    expect(read.map(({ param, value, text }) => [param.name, value, text])).toEqual([
      ['diag.boot_loop', 0, 'no'],
      ['diag.scan_max', 380, '380 µs'],
      ['diag.uptime', 61, '1 min 1 s'],
    ]);
  });

  it('reads nothing from a schema without them', async () => {
    const device = new FakeConfigDevice();
    const client = new SysexClient(device.transport(), FAST);
    await expect(readDiagnostics(client, EXAMPLE_SCHEMA)).resolves.toEqual([]);
    expect(device.received).toEqual([]);
  });

  it('keeps them out of an exported profile', () => {
    const schema = { ...EXAMPLE_SCHEMA, params: [...EXAMPLE_SCHEMA.params, ...DIAG_PARAMS] };
    const profile = exportProfile(schema, new Map(), { boardId: 1, fwVersion: 0 });
    expect(Object.keys(profile.values).some((name) => name.startsWith('diag.'))).toBe(false);
    expect(profile.values['debug.events']).toBe(0);
  });
});

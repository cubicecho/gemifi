import { describe, expect, it } from 'vitest';
import { decodeFrame, encodeFrame } from './frame.ts';
import { parseDeviceMessage } from './messages.ts';
import { countDelta, monitorPayload, parseMonitorReply, parseRaw, type RawReading } from './monitor.ts';
import { encodeRaw } from './test-support.ts';
import { MonitorKind, Unsolicited } from './wire.ts';

const READINGS: RawReading[] = [
  { kind: 'buttons', first: 0, pressed: [true, false, false, true, false, false, false, true, true] },
  {
    kind: 'matrix',
    row: 2,
    rows: 2,
    cols: 8,
    closed: Array.from({ length: 16 }, (_, i) => i === 3 || i === 9),
  },
  { kind: 'analog', first: 8, values: [0, 4095, null, 2048] },
  { kind: 'encoders', first: 0, counts: [0, 16383, 5] },
  { kind: 'pads', first: 32, values: [12, 3000] },
  { kind: 'unknown', type: 9 },
];

describe('MONITOR and RAW (§4.8)', () => {
  it('builds the request and reads the reply', () => {
    expect(monitorPayload(MonitorKind.Analog | MonitorKind.KeyMatrix)).toEqual([0x06]);
    expect(monitorPayload(0)).toEqual([0]);
    expect(parseMonitorReply([0x1f, 20, 0, 0x08, 0x27])).toEqual({ kinds: 0x1f, intervalMs: 20, timeoutMs: 5000 });
  });

  it('round-trips every kind', () => {
    for (const reading of READINGS) {
      expect(parseRaw(encodeRaw(reading)), reading.kind).toEqual(reading);
    }
  });

  it('reads bits 7 to a byte, low bit first', () => {
    expect(parseRaw([1, 0, 0, 8, 0x41, 0x01])).toEqual({
      kind: 'buttons',
      first: 0,
      pressed: [true, false, false, false, false, false, true, true],
    });
  });

  it('turns 0x3FFF into no reading', () => {
    expect(parseRaw([3, 0, 0, 1, 0x7f, 0x7f])).toEqual({ kind: 'analog', first: 0, values: [null] });
  });

  it('arrives as a raw message', () => {
    const payload = encodeRaw({ kind: 'pads', first: 0, values: [7] });
    const result = decodeFrame(encodeFrame(Unsolicited.Raw, 0, payload));
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(parseDeviceMessage(result.frame)).toEqual({
        kind: 'raw',
        reading: { kind: 'pads', first: 0, values: [7] },
      });
    }
  });

  it('counts encoder detents across the 14-bit wrap', () => {
    expect(countDelta(10, 13)).toBe(3);
    expect(countDelta(16382, 1)).toBe(3);
    expect(countDelta(1, 16382)).toBe(-3);
  });
});

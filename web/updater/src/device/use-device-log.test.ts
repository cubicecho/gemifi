import { describe, expect, it } from 'vitest';
import { Role } from '../protocol/wire.ts';
import { formatDeviceMessage } from './use-device-log.ts';

describe('formatDeviceMessage', () => {
  it('formats LOG, EVENT and PARAM_CHANGED', () => {
    expect(formatDeviceMessage({ kind: 'log', level: 2, tMs: 1234, text: 'oct- down' })).toBe(
      '[    1234 ms] INFO  oct- down',
    );
    expect(formatDeviceMessage({ kind: 'event', type: 3, index: 0, value: 8000, tMs: 5 })).toBe(
      '[       5 ms] EVENT analog 0 = 8000',
    );
    expect(formatDeviceMessage({ kind: 'event', type: 4, index: 2, value: 16381, tMs: 1000 })).toBe(
      '[    1000 ms] EVENT encoder 2 = -3',
    );
    expect(formatDeviceMessage({ kind: 'event', type: 9, index: 1, value: 1, tMs: 0 })).toBe(
      '[       0 ms] EVENT type 9 1 = 1',
    );
    expect(formatDeviceMessage({ kind: 'param', id: 3, index: 0, value: -1 })).toBe('              PARAM 3[0] = -1');
  });

  it('formats BOOTED (§4.6.3)', () => {
    const hello = {
      role: Role.App,
      boardId: 1,
      fwVersion: 0x00010005,
      protoVersion: 1,
      maxChunk: 0,
      caps: 64,
      uid: [],
    };
    expect(formatDeviceMessage({ kind: 'booted', hello: { ...hello, nodeName: 'keys' } })).toBe(
      '              BOOTED keys 0.1.5',
    );
    expect(formatDeviceMessage({ kind: 'booted', hello: { ...hello, nodeName: null } })).toBe(
      '              BOOTED device 0.1.5',
    );
  });
});

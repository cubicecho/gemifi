import { describe, expect, it } from 'vitest';
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
});

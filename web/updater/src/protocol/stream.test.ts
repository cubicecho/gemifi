import { describe, expect, it } from 'vitest';
import { encodeFrame } from './frame.ts';
import { SysexStreamParser } from './stream.ts';
import { Command } from './wire.ts';

const BOOT_LOG = Array.from(new TextEncoder().encode('ets Jun  8 2016 00:22:57\r\nrst:0x1 (POWERON_RESET)\r\n'));

describe('SysexStreamParser', () => {
  it('cuts whole messages out of a stream split anywhere', () => {
    const a = encodeFrame(Command.Hello | 0x40, 3, [0, 1, 2]);
    const b = encodeFrame(Command.FwData | 0x40, 4, [0, 5, 6, 7, 8]);
    const stream = [...BOOT_LOG, ...a, 0x90, 0x3c, 0x40, ...b];
    for (const piece of [1, 2, 3, 7, stream.length]) {
      const parser = new SysexStreamParser();
      const got: Uint8Array[] = [];
      for (let i = 0; i < stream.length; i += piece) {
        got.push(...parser.push(stream.slice(i, i + piece)));
      }
      expect(got.map((m) => Array.from(m))).toEqual([Array.from(a), Array.from(b)]);
    }
  });

  it('drops real-time bytes inside a message', () => {
    const parser = new SysexStreamParser();
    expect(parser.push([0xf0, 0x7d, 0xf8, 0x01, 0xfe, 0xf7]).map((m) => Array.from(m))).toEqual([
      [0xf0, 0x7d, 0x01, 0xf7],
    ]);
  });

  it('drops a message cut short by another status byte or a new F0', () => {
    const parser = new SysexStreamParser();
    expect(parser.push([0xf0, 0x01, 0x90, 0x02, 0xf7])).toEqual([]);
    expect(parser.push([0xf0, 0x01, 0xf0, 0x02, 0xf7]).map((m) => Array.from(m))).toEqual([[0xf0, 0x02, 0xf7]]);
  });

  it('ignores an F7 with no F0 and data bytes outside a message', () => {
    const parser = new SysexStreamParser();
    expect(parser.push([0x41, 0x42, 0xf7, 0x43])).toEqual([]);
  });

  it('drops a message longer than the limit and recovers', () => {
    const parser = new SysexStreamParser(8);
    expect(parser.push([0xf0, ...new Array(20).fill(1), 0xf7])).toEqual([]);
    expect(parser.push([0xf0, 1, 0xf7])).toHaveLength(1);
  });

  it('reset drops a partial message', () => {
    const parser = new SysexStreamParser();
    parser.push([0xf0, 0x01]);
    parser.reset();
    expect(parser.push([0x02, 0xf7])).toEqual([]);
  });
});

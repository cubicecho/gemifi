import { describe, expect, it } from 'vitest';
import { encodeU14, encodeU28, encodeU32, pack } from './encoding.ts';
import { checksum, decodeFrame, encodeFrame, FrameProblem } from './frame.ts';
import { capabilityNames, formatVersion, parseDeviceMessage, parseHello } from './messages.ts';

describe('framing (§1)', () => {
  it('builds HELLO with an empty payload', () => {
    // chk = (01 + 01 + 05 + 00 + 00) & 7F = 07
    expect(Array.from(encodeFrame(0x01, 5))).toEqual([
      0xf0, 0x7d, 0x67, 0x6d, 0x01, 0x01, 0x05, 0x00, 0x00, 0x07, 0xf7,
    ]);
  });

  it('builds REBOOT(1) and wraps the checksum at 7 bits', () => {
    // chk = (01 + 10 + 7F + 01 + 00 + 01) & 7F = 0x92 & 7F = 0x12
    expect(Array.from(encodeFrame(0x10, 0x7f, [0x01]))).toEqual([
      0xf0, 0x7d, 0x67, 0x6d, 0x01, 0x10, 0x7f, 0x01, 0x00, 0x01, 0x12, 0xf7,
    ]);
  });

  it('sums ver through the last payload byte', () => {
    expect(checksum([0x01, 0x19, 0x03, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x7f])).toBe(
      (0x01 + 0x19 + 0x03 + 0x05 + 0x04 + 0x7f) & 0x7f,
    );
  });

  it('writes a length above 127 as a 14-bit field', () => {
    const payload = new Array(200).fill(0x11);
    const frame = encodeFrame(0x19, 0, payload);
    expect(frame[7]).toBe(200 & 0x7f);
    expect(frame[8]).toBe(200 >> 7);
    expect(frame.length).toBe(11 + 200);
  });

  it('round-trips', () => {
    const result = decodeFrame(encodeFrame(0x41, 9, [0x00, 0x01, 0x7f]));
    expect(result).toEqual({ ok: true, frame: { version: 1, cmd: 0x41, seq: 9, payload: [0x00, 0x01, 0x7f] } });
  });

  it('rejects a bad checksum, a bad length and other SysEx', () => {
    const frame = Array.from(encodeFrame(0x41, 9, [0x00]));
    const corrupt = [...frame];
    corrupt[corrupt.length - 2] = (corrupt[corrupt.length - 2] ?? 0) ^ 1;
    expect(decodeFrame(corrupt)).toEqual({ ok: false, problem: FrameProblem.BadChecksum });
    const short = [...frame.slice(0, -3), 0xf7];
    expect(decodeFrame(short)).toEqual({ ok: false, problem: FrameProblem.BadLength });
    expect(decodeFrame([0xf0, 0x7e, 0x7f, 0x06, 0x02, 0xf7])).toEqual({ ok: false, problem: FrameProblem.NotGemifi });
    expect(decodeFrame([0x90, 0x40, 0x7f])).toEqual({ ok: false, problem: FrameProblem.NotSysex });
    const v2 = [...frame];
    v2[4] = 2;
    expect(decodeFrame(v2)).toEqual({ ok: false, problem: FrameProblem.BadVersion });
  });

  it('refuses to build a frame with 8-bit data in it', () => {
    expect(() => encodeFrame(0x19, 0, [0x80])).toThrow(RangeError);
    expect(() => encodeFrame(0x19, 128)).toThrow(RangeError);
  });
});

describe('messages (§4)', () => {
  it('parses HELLO', () => {
    const uid = [0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x02, 0x03, 0x80, 0x90, 0xa0, 0xff];
    const body = [
      0x01,
      ...encodeU14(1),
      ...encodeU32(0x0102_0003),
      0x01,
      ...encodeU14(256),
      ...encodeU14(0b0101),
      ...pack(uid),
    ];
    expect(body.length).toBe(27);
    expect(parseHello(body)).toEqual({
      role: 1,
      boardId: 1,
      fwVersion: 0x0102_0003,
      protoVersion: 1,
      maxChunk: 256,
      caps: 0b0101,
      uid,
      nodeName: null,
    });
  });

  it('reads the node name after the uid (§4.1.1)', () => {
    const base = [0x00, ...encodeU14(1), ...encodeU32(0x0001_0005), 0x01, ...encodeU14(0), ...encodeU14(0)];
    const head = [...base, ...pack(new Array(12).fill(0))];
    const text = (s: string) => Array.from(s, (c) => c.charCodeAt(0));
    expect(parseHello([...head, 17, ...text('keystation-mini32')]).nodeName).toBe('keystation-mini32');
    expect(parseHello([...head, 3, ...text('abc'), 0x05, 0x06]).nodeName, 'later fields ignored').toBe('abc');
    for (const bad of [[0], [5, ...text('abc')], [3, ...text('A-b')], [4, ...text('-ab-')]]) {
      expect(parseHello([...head, ...bad]).nodeName, String(bad)).toBeNull();
    }
  });

  it('parses LOG and EVENT', () => {
    const log = decodeFrame(
      encodeFrame(0x60, 3, [2, ...encodeU28(1234), ...Array.from('oct- down', (c) => c.charCodeAt(0))]),
    );
    expect(log.ok && parseDeviceMessage(log.frame)).toEqual({ kind: 'log', level: 2, tMs: 1234, text: 'oct- down' });
    const event = decodeFrame(encodeFrame(0x61, 4, [1, ...encodeU14(2), ...encodeU14(1), ...encodeU28(99)]));
    expect(event.ok && parseDeviceMessage(event.frame)).toEqual({
      kind: 'event',
      type: 1,
      index: 2,
      value: 1,
      tMs: 99,
    });
  });

  it('formats versions and capabilities', () => {
    expect(formatVersion(0x0102_0003)).toBe('1.2.3');
    expect(formatVersion(0xff00_0000)).toBe('255.0.0');
    expect(capabilityNames(0b1101)).toEqual(['firmware upload', 'log', 'events']);
    expect(capabilityNames(0b1_0000)).toEqual(['bit 4']);
  });
});

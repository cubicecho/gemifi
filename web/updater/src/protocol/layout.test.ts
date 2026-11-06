import { describe, expect, it } from 'vitest';
import { SysexClient, type Transport } from './client.ts';
import { encodeU28, PayloadReader, pack } from './encoding.ts';
import { decodeFrame, encodeFrame } from './frame.ts';
import {
  type DeviceLayout,
  getLayout,
  isBlackNote,
  keyRects,
  LayoutError,
  offersLayout,
  parseLayout,
} from './layout.ts';
import type { HelloInfo } from './messages.ts';
import { encodeLayout } from './test-support.ts';
import { Capability, Command, LayoutShape, Role, Status } from './wire.ts';

const EXAMPLE: DeviceLayout = {
  width: 80,
  height: 30,
  items: [
    { kind: 1, index: 0, shape: LayoutShape.Button, x: 1, y: 1, w: 3, h: 2 },
    { kind: 3, index: 0, shape: LayoutShape.Knob, x: 5, y: 1, w: 3, h: 3 },
    { kind: 3, index: 1, shape: LayoutShape.Xy, x: 10, y: 1, w: 8, h: 8, yIndex: 2 },
    { kind: 2, index: 0, shape: LayoutShape.Keys, x: 0, y: 12, w: 76, h: 14, keys: { count: 32, firstNote: 48 } },
  ],
};

describe('layout blob (§4.7)', () => {
  it('round-trips every item', () => {
    expect(parseLayout(encodeLayout(EXAMPLE))).toEqual(EXAMPLE);
  });

  it('reads the header byte by byte', () => {
    const bytes = [1, 20, 10, 1, 0, 7, 4, 3, LayoutShape.Encoder, 2, 3, 4, 5];
    expect(parseLayout(bytes)).toEqual({
      width: 20,
      height: 10,
      items: [{ kind: 4, index: 3, shape: LayoutShape.Encoder, x: 2, y: 3, w: 4, h: 5 }],
    });
  });

  it('skips bytes it does not know by len, and items of unknown kinds', () => {
    const bytes = [1, 20, 10, 2, 0, 9, 1, 0, 0, 1, 1, 2, 2, 0xaa, 0xbb, 7, 9, 0, 0, 0, 0, 1, 1];
    expect(parseLayout(bytes).items).toEqual([{ kind: 1, index: 0, shape: 0, x: 1, y: 1, w: 2, h: 2 }]);
  });

  it('rejects another version, a short blob, a short item and a len under 7', () => {
    expect(() => parseLayout([2, 1, 1, 0, 0])).toThrow(LayoutError);
    expect(() => parseLayout([1, 1, 1])).toThrow(LayoutError);
    expect(() => parseLayout([1, 1, 1, 1, 0, 7, 1, 0])).toThrow(LayoutError);
    expect(() => parseLayout([1, 1, 1, 1, 0, 3, 1, 0, 0])).toThrow(LayoutError);
  });

  it('needs caps bit 8 on the app', () => {
    const hello = { role: Role.App, caps: Capability.Layout } as HelloInfo;
    expect(offersLayout(hello)).toBe(true);
    expect(offersLayout({ ...hello, caps: 0 })).toBe(false);
    expect(offersLayout({ ...hello, role: Role.Boot })).toBe(false);
    expect(offersLayout(null)).toBe(false);
  });
});

describe('key geometry (§4.7)', () => {
  it('shares the width among the white keys and centres black keys on the lines', () => {
    const item = {
      kind: 2,
      index: 0,
      shape: LayoutShape.Keys,
      x: 10,
      y: 5,
      w: 70,
      h: 10,
      keys: { count: 12, firstNote: 48 },
    };
    const keys = keyRects(item);
    const whites = keys.filter((k) => k.isBlack === false);
    const blacks = keys.filter((k) => k.isBlack);
    expect(whites.map((k) => k.x)).toEqual([10, 20, 30, 40, 50, 60, 70]);
    expect(whites.every((k) => k.w === 10 && k.h === 10)).toBe(true);
    expect(blacks.map((k) => k.note)).toEqual([49, 51, 54, 56, 58]);
    expect(blacks.map((k) => k.x + k.w / 2)).toEqual([20, 30, 50, 60, 70]);
    expect(blacks[0]).toMatchObject({ key: 1, w: 6, h: 6, y: 5 });
  });

  it('puts a black key at either end on the edge, and numbers keys from the item index', () => {
    const item = {
      kind: 2,
      index: 4,
      shape: LayoutShape.Keys,
      x: 0,
      y: 0,
      w: 20,
      h: 10,
      keys: { count: 4, firstNote: 49 },
    };
    const keys = keyRects(item);
    const first = keys.find((k) => k.note === 49);
    const last = keys.find((k) => k.note === 52);
    expect(first).toMatchObject({ key: 4, isBlack: true });
    expect((first?.x ?? 0) + (first?.w ?? 0) / 2).toBe(0);
    expect(last).toMatchObject({ key: 7, isBlack: false, x: 10 });
    expect(isBlackNote(61)).toBe(true);
    expect(isBlackNote(60)).toBe(false);
  });
});

/**
 * A device that serves `blob` to GET_LAYOUT in windows.
 *
 * @param blob - The layout.
 * @returns The transport.
 */
function layoutDevice(blob: number[]): Transport {
  const listeners = new Set<(bytes: Uint8Array) => void>();
  return {
    send(bytes) {
      const result = decodeFrame(bytes);
      if (result.ok === false) {
        return;
      }
      const { cmd, seq, payload } = result.frame;
      const reader = new PayloadReader(payload);
      const offset = reader.u28();
      const max = reader.u14();
      const reply =
        cmd === Command.GetLayout
          ? [Status.Ok, ...encodeU28(blob.length), ...encodeU28(offset), ...pack(blob.slice(offset, offset + max))]
          : [Status.BadCmd];
      queueMicrotask(() => {
        for (const listener of listeners) {
          listener(encodeFrame(cmd | 0x40, seq, reply));
        }
      });
    },
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}

describe('GET_LAYOUT', () => {
  it('reads a layout of several windows', async () => {
    const big: DeviceLayout = {
      width: 200,
      height: 40,
      items: Array.from({ length: 30 }, (_, i) => ({ kind: 1, index: i, shape: 0, x: i * 6, y: 1, w: 5, h: 2 })),
    };
    const client = new SysexClient(layoutDevice(encodeLayout(big)));
    expect(await getLayout(client)).toEqual(big);
  });
});

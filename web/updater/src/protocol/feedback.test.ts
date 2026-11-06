import { describe, expect, it } from 'vitest';
import { RequestError, SysexClient, type Transport } from './client.ts';
import { DecodeError, encodeLstring, encodeU14, PayloadReader } from './encoding.ts';
import {
  type Component,
  fbLedPayload,
  fbLedRgbPayload,
  fbLedsPayload,
  fbReleasePayload,
  fbRingPayload,
  fbTextPayload,
  fbValuePayload,
  getComponent,
  getComponentPayload,
  hostDrives,
  listComponents,
  parseComponent,
  takesValue,
} from './feedback.ts';
import { decodeFrame, encodeFrame, type Frame } from './frame.ts';
import { capabilityNames } from './messages.ts';
import { Capability, Command, ComponentKind, NONE_INDEX, Notification, RingMode, Status } from './wire.ts';

/** Text as its character codes. */
const ascii = (text: string) => Array.from(text, (c) => c.charCodeAt(0));

/**
 * Writes a component the way §4.6.1 lays it out (the inverse of `parseComponent`).
 *
 * @param c - The component.
 * @returns The reply body after the status byte.
 */
function encodeComponent(c: Component): number[] {
  const out = [c.kind, ...encodeU14(c.index), ...encodeU14(c.count)];
  out.push(...encodeLstring(c.id), ...encodeLstring(c.name), ...encodeLstring(c.role));
  switch (c.kind) {
    case ComponentKind.Button:
      out.push(...encodeU14(c.led ?? NONE_INDEX));
      break;
    case ComponentKind.KeyMatrix:
      out.push(...encodeU14(c.keys), c.firstNote);
      break;
    case ComponentKind.Analog:
      out.push(...optional(c.takeover), ...optional(c.touch ?? null));
      if (c.adcMin !== undefined && c.adcMax !== undefined) {
        out.push(...encodeU14(c.adcMin), ...encodeU14(c.adcMax));
      }
      break;
    case ComponentKind.Encoder:
      out.push(c.ring, ...encodeU14(c.center ?? NONE_INDEX), ...optional(c.mode));
      break;
    case ComponentKind.Led:
      out.push(c.colours, ...optional(c.owner));
      break;
    case ComponentKind.Display:
      out.push(c.cols, c.rows, c.type);
      break;
    case ComponentKind.Pad:
      out.push(...encodeU14(c.led ?? NONE_INDEX), ...optional(c.layer));
      break;
  }
  return out;
}

/**
 * @param value - An added field (§4.6.1), or null for older firmware.
 * @returns Its byte, or nothing.
 */
function optional(value: number | null): number[] {
  return value === null ? [] : [value];
}

const base = { count: 1, id: 'x', name: 'X', role: '' };
const EXAMPLES: Component[] = [
  { kind: ComponentKind.Button, ...base, index: 0, count: 2, id: 'play', name: 'Play', role: 'play', led: 4 },
  { kind: ComponentKind.Button, ...base, index: 1, count: 2, id: 'stop', name: 'Stop', role: 'stop', led: null },
  { kind: ComponentKind.KeyMatrix, ...base, index: 0, id: 'keys', name: 'Keys', keys: 61, firstNote: 36 },
  { kind: ComponentKind.Analog, ...base, index: 0, id: 'fader_1', name: 'Fader 1', role: 'track_volume', takeover: 1 },
  { kind: ComponentKind.Analog, ...base, index: 1, id: 'knob', name: 'Knob', takeover: 0 },
  { kind: ComponentKind.Analog, ...base, index: 2, id: 'old', name: 'Older firmware', takeover: null },
  { kind: ComponentKind.Analog, ...base, index: 3, id: 'touchy', name: 'Touch', takeover: 0, touch: 2 },
  {
    kind: ComponentKind.Analog,
    ...base,
    index: 4,
    id: 'aftertouch',
    name: 'Aftertouch',
    takeover: 0,
    touch: 0,
    adcMin: 2304,
    adcMax: 1232,
  },
  { kind: ComponentKind.Encoder, ...base, index: 0, id: 'enc_1', name: 'Encoder 1', ring: 11, center: 300, mode: 0 },
  { kind: ComponentKind.Encoder, ...base, index: 0, id: 'enc_2', name: 'Encoder 2', ring: 0, center: null, mode: 3 },
  { kind: ComponentKind.Encoder, ...base, index: 0, id: 'enc_3', name: 'Older', ring: 0, center: null, mode: null },
  { kind: ComponentKind.Led, ...base, index: 0, count: 2, id: 'play_led', name: 'Play LED', colours: 2, owner: 2 },
  { kind: ComponentKind.Led, ...base, index: 1, count: 2, id: 'old_led', name: 'Older', colours: 1, owner: null },
  { kind: ComponentKind.Display, ...base, index: 0, id: 'lcd', name: 'LCD', cols: 16, rows: 2, type: 1 },
  { kind: ComponentKind.Pad, ...base, index: 0, count: 2, id: 'pad_1', name: 'Pad 1', led: 36, layer: 0 },
  { kind: ComponentKind.Pad, ...base, index: 1, count: 2, id: 'pad_1_pads', name: 'Pad 1', led: 52, layer: 3 },
  { kind: ComponentKind.Pad, ...base, index: 0, id: 'old_pad', name: 'Older', led: null, layer: null },
];

describe('GET_COMPONENT (§4.6.1)', () => {
  it('builds the request', () => {
    expect(getComponentPayload(ComponentKind.Encoder, 130)).toEqual([4, 0x02, 0x01]);
    expect(() => getComponentPayload(ComponentKind.Led, 0x4000)).toThrow(RangeError);
  });

  it('reads a button reply byte by byte, with 0x3FFF as no LED', () => {
    const body = [1, 0, 0, 2, 0, 4, ...ascii('play'), 4, ...ascii('Play'), 0, 0x7f, 0x7f];
    expect(parseComponent(body)).toEqual({
      kind: 1,
      index: 0,
      count: 2,
      id: 'play',
      name: 'Play',
      role: '',
      led: null,
    });
  });

  it('round-trips every kind', () => {
    for (const component of EXAMPLES) {
      expect(parseComponent(encodeComponent(component)), component.id).toEqual(component);
    }
  });

  it('reads takeover, mode and owner as the last byte of their kind', () => {
    const strings = [0, 0, 0];
    expect(parseComponent([3, 0, 0, 1, 0, ...strings, 1])).toMatchObject({ takeover: 1 });
    expect(parseComponent([4, 0, 0, 1, 0, ...strings, 5, 0x7f, 0x7f, 2])).toMatchObject({ ring: 5, mode: 2 });
    expect(parseComponent([5, 0, 0, 1, 0, ...strings, 2, 1])).toMatchObject({ colours: 2, owner: 1 });
    expect(parseComponent([5, 0, 0, 1, 0, ...strings, 2])).toMatchObject({ colours: 2, owner: null });
  });

  it("reads an analog's touch and adc range after takeover, and leaves them out when absent", () => {
    const strings = [0, 0, 0];
    const head = [3, 0, 0, 1, 0, ...strings];
    expect(parseComponent([...head, 0, 2, 0x20, 0x00, 0x7f, 0x1f])).toMatchObject({
      touch: 2,
      adcMin: 32,
      adcMax: 4095,
    });
    const old = parseComponent([...head, 1, 0]);
    expect(old).toMatchObject({ takeover: 1, touch: 0 });
    expect('adcMin' in old).toBe(false);
  });

  it('says what FB_VALUE and FB_LED reach', () => {
    const analog = { kind: ComponentKind.Analog, ...base, index: 0 } as const;
    expect(takesValue({ ...analog, takeover: 1 })).toBe(true);
    expect(takesValue({ ...analog, takeover: 0 })).toBe(false);
    expect(takesValue({ ...analog, takeover: null })).toBe(true);
    const encoder = { kind: ComponentKind.Encoder, ...base, index: 0, ring: 0, center: null } as const;
    expect(takesValue({ ...encoder, mode: 0 })).toBe(true);
    expect([1, 2, 3, 9].map((mode) => takesValue({ ...encoder, mode }))).toEqual([false, false, false, false]);
    expect(takesValue({ ...encoder, mode: null })).toBe(true);
    const led = { kind: ComponentKind.Led, ...base, index: 0, colours: 1 } as const;
    expect([0, 1, 2, null].map((owner) => hostDrives({ ...led, owner }))).toEqual([true, true, false, true]);
  });

  it('ignores bytes after the fields it knows', () => {
    const [first] = EXAMPLES;
    expect(parseComponent([...encodeComponent(first as Component), 9, 9])).toEqual(first);
  });

  it('rejects an unknown kind, a short reply and an lstring over 31', () => {
    expect(() => parseComponent([9, 0, 0, 1, 0, 0, 0, 0])).toThrow(/unknown component kind 9/);
    expect(() => parseComponent([5, 0, 0, 1, 0, 0, 0, 0])).toThrow(DecodeError);
    expect(() => parseComponent([3, 0, 0, 1, 0, 32, ...new Array(32).fill(0x41), 0, 0])).toThrow(/over 31/);
    expect(() => parseComponent([3, 0, 0, 1, 0, 5, 0x41])).toThrow(/runs past/);
  });
});

describe('lstring (§2)', () => {
  it('writes and reads 0..31 printable characters', () => {
    expect(encodeLstring('')).toEqual([0]);
    expect(encodeLstring('Ab ~')).toEqual([4, 0x41, 0x62, 0x20, 0x7e]);
    const long = 'a'.repeat(31);
    expect(new PayloadReader(encodeLstring(long)).lstring()).toBe(long);
    expect(() => encodeLstring('a'.repeat(32))).toThrow(RangeError);
    expect(() => encodeLstring('tab\there')).toThrow(RangeError);
  });
});

describe('FB_* payloads (§4.6.2)', () => {
  it('FB_LED', () => {
    expect(fbLedPayload(3, 127)).toEqual([0x03, 0x00, 0x7f]);
    expect(fbLedPayload(200, 64)).toEqual([0x48, 0x01, 0x40]);
    expect(() => fbLedPayload(0, 128)).toThrow(RangeError);
    expect(() => fbLedPayload(0x4000, 0)).toThrow(RangeError);
  });

  it('FB_LED_RGB', () => {
    expect(fbLedRgbPayload(1, { r: 127, g: 0, b: 64 }, true)).toEqual([0x01, 0x00, 0x7f, 0x00, 0x40, 0x01]);
    expect(fbLedRgbPayload(1, { r: 1, g: 2, b: 3 })).toEqual([0x01, 0x00, 0x01, 0x02, 0x03, 0x00]);
    expect(() => fbLedRgbPayload(1, { r: 128, g: 0, b: 0 })).toThrow(RangeError);
  });

  it('FB_LEDS', () => {
    expect(fbLedsPayload(8, [0, 1, 127])).toEqual([0x08, 0x00, 0x03, 0x00, 0x01, 0x7f]);
    expect(fbLedsPayload(0, new Array(64).fill(2))).toHaveLength(67);
    expect(() => fbLedsPayload(0, [])).toThrow(RangeError);
    expect(() => fbLedsPayload(0, new Array(65).fill(0))).toThrow(RangeError);
    expect(() => fbLedsPayload(0x3ffe, [0, 0, 0])).toThrow(RangeError);
    expect(() => fbLedsPayload(0, [200])).toThrow(RangeError);
  });

  it('FB_VALUE', () => {
    expect(fbValuePayload(ComponentKind.Analog, 2, 16383)).toEqual([0x03, 0x02, 0x00, 0x7f, 0x7f]);
    expect(fbValuePayload(ComponentKind.Encoder, 1, 8192)).toEqual([0x04, 0x01, 0x00, 0x00, 0x40]);
    expect(() => fbValuePayload(ComponentKind.Analog, 0, 16384)).toThrow(RangeError);
    expect(() => fbValuePayload(ComponentKind.Led as typeof ComponentKind.Analog, 0, 0)).toThrow(/not 3/);
  });

  it('FB_RING', () => {
    expect(fbRingPayload(0, RingMode.BoostCut, 8192, true)).toEqual([0x00, 0x00, 0x01, 0x00, 0x40, 0x01]);
    expect(fbRingPayload(130, RingMode.Spread, 5)).toEqual([0x02, 0x01, 0x03, 0x05, 0x00, 0x00]);
    expect(() => fbRingPayload(0, 4 as RingMode, 0)).toThrow(RangeError);
    expect(() => fbRingPayload(0, RingMode.Dot, -1)).toThrow(RangeError);
  });

  it('FB_TEXT', () => {
    expect(fbTextPayload(0, 1, 2, 'Hi')).toEqual([0x00, 0x00, 0x01, 0x02, 0x48, 0x69]);
    expect(fbTextPayload(1, 0, 0, '')).toEqual([0x01, 0x00, 0x00, 0x00]);
    expect(fbTextPayload(0, 0, 0, 'x'.repeat(64))).toHaveLength(68);
    expect(() => fbTextPayload(0, 0, 0, 'x'.repeat(65))).toThrow(RangeError);
    expect(() => fbTextPayload(0, 0, 0, 'é')).toThrow(/7-bit/);
    expect(() => fbTextPayload(0, 128, 0, '')).toThrow(RangeError);
  });

  it('FB_RELEASE', () => {
    expect(fbReleasePayload(0)).toEqual([0x00]);
    expect(fbReleasePayload(3)).toEqual([0x03]);
    expect(() => fbReleasePayload(4 as 0)).toThrow(RangeError);
  });
});

/** A device with the given components behind an in-memory transport, answering GET_COMPONENT. */
class FakeFeedbackDevice {
  readonly components: Component[];
  /** Every frame received, decoded. */
  readonly received: Frame[] = [];
  #listeners = new Set<(bytes: Uint8Array) => void>();

  /**
   * @param components - What the device has.
   */
  constructor(components: Component[]) {
    this.components = components;
  }

  /** @returns A transport to this device. */
  transport(): Transport {
    return {
      send: (bytes) => queueMicrotask(() => this.#handle(bytes)),
      subscribe: (listener) => {
        this.#listeners.add(listener);
        return () => this.#listeners.delete(listener);
      },
    };
  }

  #handle(bytes: Uint8Array): void {
    const result = decodeFrame(bytes);
    if (result.ok === false) {
      return;
    }
    const { frame } = result;
    this.received.push(frame);
    if (frame.cmd !== Command.GetComponent) {
      return;
    }
    const reader = new PayloadReader(frame.payload);
    const kind = reader.byte();
    const index = reader.u14();
    const ofKind = this.components.filter((c) => c.kind === kind);
    const found = ofKind[index];
    const payload =
      found === undefined ? [Status.BadParam] : [Status.Ok, ...encodeComponent({ ...found, count: ofKind.length })];
    const reply = encodeFrame(frame.cmd | 0x40, frame.seq, payload);
    for (const listener of this.#listeners) {
      listener(reply);
    }
  }
}

describe('the client side', () => {
  it('lists every component by kind, skipping kinds with none', async () => {
    const buttons = EXAMPLES.filter((c) => c.kind === ComponentKind.Button);
    const leds = EXAMPLES.filter((c) => c.kind === ComponentKind.Led);
    const device = new FakeFeedbackDevice([...leds, ...buttons]);
    const client = new SysexClient(device.transport(), { replyTimeoutMs: 20 });
    const list = await listComponents(client);
    expect(list).toEqual([...buttons, ...leds]);
    // One request at index 0 per kind, then index 1 of the buttons and the LEDs.
    expect(device.received.map((f) => [f.payload[0], f.payload[1]])).toEqual([
      [1, 0],
      [1, 1],
      [2, 0],
      [3, 0],
      [4, 0],
      [5, 0],
      [5, 1],
      [6, 0],
      [7, 0],
    ]);
  });

  it('turns a reply for the wrong component into bad-reply', async () => {
    const listeners = new Set<(bytes: Uint8Array) => void>();
    // A device that answers every GET_COMPONENT with button 0's description.
    const wrong = new SysexClient(
      {
        send: (bytes) => {
          const result = decodeFrame(bytes);
          if (result.ok) {
            const reply = encodeFrame(0x51, result.frame.seq, [
              Status.Ok,
              ...encodeComponent(EXAMPLES[0] as Component),
            ]);
            queueMicrotask(() => {
              for (const listener of listeners) {
                listener(reply);
              }
            });
          }
        },
        subscribe: (listener) => {
          listeners.add(listener);
          return () => listeners.delete(listener);
        },
      },
      { replyTimeoutMs: 20 },
    );
    await expect(getComponent(wrong, ComponentKind.Button, 0)).resolves.toEqual(EXAMPLES[0]);
    await expect(getComponent(wrong, ComponentKind.Button, 1)).rejects.toThrow(/answered kind 1 index 0/);
  });

  it('notify() sends one frame with seq 0 and does not wait for a reply', () => {
    const sent: Uint8Array[] = [];
    const client = new SysexClient({ send: (bytes) => sent.push(bytes), subscribe: () => () => undefined });
    const result = client.notify(Notification.FbLed, fbLedPayload(3, 127));
    expect(result).toBeUndefined();
    expect(sent.map((b) => Array.from(b))).toEqual([
      [0xf0, 0x7d, 0x67, 0x6d, 0x01, 0x20, 0x00, 0x03, 0x00, 0x03, 0x00, 0x7f, 0x26, 0xf7],
    ]);
  });

  it('notify() does not queue behind a waiting request', async () => {
    const sent: number[] = [];
    const client = new SysexClient(
      { send: (bytes) => sent.push(bytes[5] ?? 0), subscribe: () => () => undefined },
      { replyTimeoutMs: 10, maxRetries: 0 },
    );
    const pending = client.request(Command.Hello);
    await Promise.resolve();
    await Promise.resolve();
    expect(sent).toEqual([Command.Hello]);
    client.notify(Notification.FbRelease, fbReleasePayload(0));
    expect(sent).toEqual([Command.Hello, Notification.FbRelease]);
    await expect(pending).rejects.toBeInstanceOf(RequestError);
  });

  it('notify() refuses a request command, and a closed client', () => {
    const client = new SysexClient({ send: () => undefined, subscribe: () => () => undefined });
    expect(() => client.notify(Command.Hello)).toThrow(RangeError);
    expect(() => client.notify(0x40)).toThrow(RangeError);
    client.close();
    expect(() => client.notify(Notification.FbRelease, [0])).toThrow(RequestError);
  });

  it('names caps bit 6', () => {
    expect(capabilityNames(Capability.Feedback | Capability.Config)).toEqual(['config', 'feedback']);
  });
});

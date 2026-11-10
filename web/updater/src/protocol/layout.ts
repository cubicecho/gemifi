/**
 * The physical layout (`protocol/sysex.md` §4.7): GET_LAYOUT's paged read, the v1 blob, and the
 * fixed rule a `keys` item's keys are drawn by. Framework-free.
 */

import type { RequestOptions, SysexClient } from './client.ts';
import { readPagedBytes } from './config.ts';
import { DecodeError } from './encoding.ts';
import type { HelloInfo } from './messages.ts';
import { Capability, Command, ComponentKind, LAYOUT_VERSION, LayoutShape, Role } from './wire.ts';

/** A layout blob that cannot be read. */
export class LayoutError extends DecodeError {
  override name = 'LayoutError';
}

/** One control's place in the drawing (§4.7). */
export interface LayoutItem {
  /** GET_COMPONENT's kind: 1 button, 2 key matrix, 3 analog, 4 encoder, 6 display, 7 pad. */
  kind: number;
  /** Its index in that kind; for a `keys` item, the first key. */
  index: number;
  /** {@link LayoutShape}; unknown shapes are drawn as buttons. */
  shape: number;
  x: number;
  y: number;
  w: number;
  h: number;
  /** `keys`: how many keys, and the note key `index` plays at octave 0. */
  keys?: { count: number; firstNote: number };
  /** `xy`: the analog input for the Y axis (`index` is X). */
  yIndex?: number;
}

/** A device's layout: the drawing's size in units and its items. */
export interface DeviceLayout {
  width: number;
  height: number;
  items: LayoutItem[];
}

/** Where each field of an item is, after its `len` byte (§4.7). */
const Field = { Kind: 0, Index: 1, Shape: 2, X: 3, Y: 4, W: 5, H: 6, Extra: 7 } as const;
/** Bytes of an item after `len` without extras: kind, index, shape, x, y, w, h. */
const ITEM_BASE_BYTES = Field.Extra;
/** Where the header's fields are. */
const Header = { Version: 0, Width: 1, Height: 2, CountLow: 3, CountHigh: 4 } as const;
const BYTE_BITS = 8;
/** Bytes of the header: version, width, height, item_count u16. */
const HEADER_BYTES = 5;
/** The kinds a layout item may be (§4.7). */
const LAYOUT_KINDS: ReadonlySet<number> = new Set([
  ComponentKind.Button,
  ComponentKind.KeyMatrix,
  ComponentKind.Analog,
  ComponentKind.Encoder,
  ComponentKind.Display,
  ComponentKind.Pad,
]);

/**
 * @param info - HELLO from the connected device, or null.
 * @returns Whether it is the app and serves its layout (caps bit 8).
 */
export function offersLayout(info: HelloInfo | null): boolean {
  return info !== null && info.role === Role.App && (info.caps & Capability.Layout) !== 0;
}

/**
 * Parses the v1 layout blob. Items of a kind this module doesn't know are skipped; bytes after
 * the known fields of an item are skipped by its `len`.
 *
 * @param bytes - The unpacked blob.
 * @returns The layout.
 */
export function parseLayout(bytes: ArrayLike<number>): DeviceLayout {
  if (bytes.length < HEADER_BYTES) {
    throw new LayoutError(`a layout of ${bytes.length} bytes is shorter than its header`);
  }
  const at = (i: number): number => {
    const b = bytes[i];
    if (b === undefined) {
      throw new LayoutError(`the layout ends at byte ${i}`);
    }
    return b;
  };
  const version = at(Header.Version);
  if (version !== LAYOUT_VERSION) {
    throw new LayoutError(`layout version ${version} is not supported (this page reads ${LAYOUT_VERSION})`);
  }
  const width = at(Header.Width);
  const height = at(Header.Height);
  const count = at(Header.CountLow) | (at(Header.CountHigh) << BYTE_BITS);
  const items: LayoutItem[] = [];
  let p = HEADER_BYTES;
  for (let n = 0; n < count; n++) {
    const len = at(p);
    if (len < ITEM_BASE_BYTES) {
      throw new LayoutError(`item ${n} has len ${len}, less than ${ITEM_BASE_BYTES}`);
    }
    const start = p + 1;
    at(start + len - 1);
    const item: LayoutItem = {
      kind: at(start + Field.Kind),
      index: at(start + Field.Index),
      shape: at(start + Field.Shape),
      x: at(start + Field.X),
      y: at(start + Field.Y),
      w: at(start + Field.W),
      h: at(start + Field.H),
    };
    const extra = len - ITEM_BASE_BYTES;
    const KEYS_EXTRA = 2;
    if (item.shape === LayoutShape.Keys && extra >= KEYS_EXTRA) {
      item.keys = { count: at(start + Field.Extra), firstNote: at(start + Field.Extra + 1) };
    }
    if (item.shape === LayoutShape.Xy && extra >= 1) {
      item.yIndex = at(start + Field.Extra);
    }
    p = start + len;
    if (LAYOUT_KINDS.has(item.kind)) {
      items.push(item);
    }
  }
  return { width, height, items };
}

/**
 * Reads and parses the device's layout.
 *
 * @param client - The device, which has HELLO caps bit 8.
 * @param options - An abort signal.
 * @returns The layout.
 */
export async function getLayout(client: SysexClient, options: RequestOptions = {}): Promise<DeviceLayout> {
  return parseLayout(await readPagedBytes(client, Command.GetLayout, 'GET_LAYOUT', options));
}

/**
 * The key a control is known by in a drawing's highlight and value maps.
 *
 * @param kind - Its kind (a key matrix's keys: 2, with the key index).
 * @param index - Its index.
 * @returns E.g. `3:0`.
 */
export function controlKey(kind: number, index: number): string {
  return `${kind}:${index}`;
}

/** One key of a `keys` item, as the §4.7 rule places it. */
export interface KeyRect {
  /** The key index (EVENT's). */
  key: number;
  /** The note it plays at octave 0. */
  note: number;
  isBlack: boolean;
  x: number;
  y: number;
  w: number;
  h: number;
}

/** A black key's width and height, as a share of a white key's (§4.7). */
const BLACK_SHARE = 0.6;
const OCTAVE = 12;
/** Which notes of an octave, C first, are black keys. */
const BLACK_IN_OCTAVE = [false, true, false, true, false, false, true, false, true, false, true, false];

/**
 * @param note - A MIDI note.
 * @returns Whether it is a black key.
 */
export function isBlackNote(note: number): boolean {
  return BLACK_IN_OCTAVE[((note % OCTAVE) + OCTAVE) % OCTAVE] === true;
}

/**
 * Places the keys of a `keys` item (§4.7): white keys share the width, full height; a black key
 * is 0.6 of a white key wide and 0.6 of the height tall, centred on the line between its white
 * neighbours (at either end of the range, on the item's edge). White keys come first in the
 * list, so drawing in order puts the black keys on top.
 *
 * @param item - A `keys` item.
 * @returns Every key, white ones first.
 */
export function keyRects(item: LayoutItem): KeyRect[] {
  const count = item.keys?.count ?? 0;
  const firstNote = item.keys?.firstNote ?? 0;
  const notes = Array.from({ length: count }, (_, i) => firstNote + i);
  const whites = notes.filter((note) => isBlackNote(note) === false).length;
  const whiteW = item.w / Math.max(1, whites);
  const whiteKeys: KeyRect[] = [];
  const blackKeys: KeyRect[] = [];
  let whiteIndex = 0;
  notes.forEach((note, i) => {
    const key = item.index + i;
    if (isBlackNote(note)) {
      const w = whiteW * BLACK_SHARE;
      const centre = item.x + whiteIndex * whiteW;
      blackKeys.push({ key, note, isBlack: true, x: centre - w / 2, y: item.y, w, h: item.h * BLACK_SHARE });
    } else {
      whiteKeys.push({ key, note, isBlack: false, x: item.x + whiteIndex * whiteW, y: item.y, w: whiteW, h: item.h });
      whiteIndex++;
    }
  });
  return [...whiteKeys, ...blackKeys];
}

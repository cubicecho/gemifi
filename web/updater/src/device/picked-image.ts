/**
 * An image waiting in the Firmware card: picked from disk there, or handed over by the builder.
 */

import { HEADER_OFFSET, IMAGE_MAGIC } from '../protocol/image.ts';

/** An image the Firmware card checks and uploads. */
export interface PickedImage {
  name: string;
  bytes: Uint8Array;
  /** A `.gfw` (gemifi header at 0xC0) rather than a chip's own image. */
  isGfw: boolean;
  /** Where it came from, when not a file the user picked: "Built by the builder from Keys". */
  source?: string | undefined;
}

/**
 * @param bytes - A file's contents.
 * @returns Whether it carries the `.gfw` magic at 0xC0.
 */
function hasGfwHeader(bytes: Uint8Array): boolean {
  if (bytes.length < HEADER_OFFSET + Uint32Array.BYTES_PER_ELEMENT) {
    return false;
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return view.getUint32(HEADER_OFFSET, true) === IMAGE_MAGIC;
}

/**
 * @param name - The file's name.
 * @param bytes - Its contents.
 * @param source - Where it came from, when not picked by hand.
 * @returns The image, with whether it is a `.gfw` worked out.
 */
export function pickedImageOf(name: string, bytes: Uint8Array, source?: string): PickedImage {
  const isGfw = hasGfwHeader(bytes) || name.toLowerCase().endsWith('.gfw');
  return { name, bytes, isGfw, source };
}

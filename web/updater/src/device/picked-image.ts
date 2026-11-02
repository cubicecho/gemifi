/**
 * An image waiting in the Firmware card: picked from disk there, or handed over by the builder.
 */

import { hasGfwMagic } from '../protocol/image.ts';

/** An image the Firmware card checks and uploads. */
export interface PickedImage {
  name: string;
  bytes: Uint8Array;
  /** A `.gfw` (gemifi header after the vector table) rather than a chip's own image. */
  isGfw: boolean;
  /** Where it came from, when not a file the user picked: "Built by the builder from Keys". */
  source?: string | undefined;
}

/**
 * @param name - The file's name.
 * @param bytes - Its contents.
 * @param source - Where it came from, when not picked by hand.
 * @returns The image, with whether it is a `.gfw` worked out.
 */
export function pickedImageOf(name: string, bytes: Uint8Array, source?: string): PickedImage {
  const isGfw = hasGfwMagic(bytes) || name.toLowerCase().endsWith('.gfw');
  return { name, bytes, isGfw, source };
}

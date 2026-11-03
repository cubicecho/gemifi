/**
 * An image waiting in the Firmware card: picked from disk there, or handed over by the builder.
 */

import { hasGfwMagic } from '../protocol/image.ts';
import { NODE_NAME_PATTERN } from '../protocol/messages.ts';

/** An image the Firmware card checks and uploads. */
export interface PickedImage {
  name: string;
  bytes: Uint8Array;
  /** A `.gfw` (gemifi header after the vector table) rather than a chip's own image. */
  isGfw: boolean;
  /** Where it came from, when not a file the user picked: "Built by the builder from Keys". */
  source?: string | undefined;
  /** The node name it was built for (from the builder, or the file name); null when unknown. */
  nodeName: string | null;
}

/** `<node name>-<major>.<minor>.<patch>`, then anything (e.g. `-factory`) and `.gfw` or `.bin`. */
const IMAGE_NAME = /^(.+?)-\d+\.\d+\.\d+(?:-[a-z]+)?\.(?:gfw|bin)$/;

/**
 * Built images are named `<node name>-<version>` (`tools/builder/gemifi_build.py` `image_stem`), so
 * a file picked from disk still says which device it is for. A name without a version, or one that
 * was renamed, says nothing.
 *
 * @param file - The image's file name.
 * @returns The node name in it, or null.
 */
export function nodeNameOfImage(file: string): string | null {
  const match = IMAGE_NAME.exec(file);
  const name = match?.[1] ?? null;
  return name !== null && NODE_NAME_PATTERN.test(name) ? name : null;
}

/**
 * @param name - The file's name.
 * @param bytes - Its contents.
 * @param source - Where it came from, when not picked by hand.
 * @param nodeName - The node name it was built for, when the builder knows it; else read from `name`.
 * @returns The image, with whether it is a `.gfw` worked out.
 */
export function pickedImageOf(name: string, bytes: Uint8Array, source?: string, nodeName?: string): PickedImage {
  const isGfw = hasGfwMagic(bytes) || name.toLowerCase().endsWith('.gfw');
  const known = nodeName !== undefined && nodeName !== '' ? nodeName : nodeNameOfImage(name);
  return { name, bytes, isGfw, source, nodeName: known };
}

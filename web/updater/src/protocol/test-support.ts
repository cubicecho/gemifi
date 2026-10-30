/** Helpers shared by the protocol tests: a synthetic `.gfw` image built the way mkimage would. */

import { CRC_FIELD_OFFSET, HEADER_OFFSET, IMAGE_MAGIC, imageCrc } from './image.ts';

/** Fields a test may change in the synthetic image. */
export interface TestImageOptions {
  size?: number;
  boardId?: number;
  fwVersion?: number;
  stackPointer?: number;
  resetVector?: number;
  magic?: number;
}

/**
 * Builds a valid image: a vector table, a filled header with a correct CRC, and patterned code.
 *
 * @param options - Fields to change.
 * @returns The file's bytes.
 */
export function makeTestImage(options: TestImageOptions = {}): Uint8Array {
  const size = options.size ?? 1024;
  const bytes = new Uint8Array(size);
  for (let i = 0; i < size; i++) {
    bytes[i] = (i * 37 + 11) & 0xff;
  }
  const view = new DataView(bytes.buffer);
  view.setUint32(0, options.stackPointer ?? 0x2000_0ff0, true);
  view.setUint32(4, options.resetVector ?? 0x0000_0101, true);
  view.setUint32(HEADER_OFFSET, options.magic ?? IMAGE_MAGIC, true);
  view.setUint16(HEADER_OFFSET + 4, 1, true);
  view.setUint16(HEADER_OFFSET + 6, options.boardId ?? 1, true);
  view.setUint32(HEADER_OFFSET + 8, size, true);
  view.setUint32(HEADER_OFFSET + 16, options.fwVersion ?? 0x0102_0003, true);
  view.setUint32(HEADER_OFFSET + 20, 1, true);
  view.setUint32(HEADER_OFFSET + 24, 0xffff_ffff, true);
  view.setUint32(HEADER_OFFSET + 28, 0xffff_ffff, true);
  view.setUint32(CRC_FIELD_OFFSET, imageCrc(bytes, size), true);
  return bytes;
}

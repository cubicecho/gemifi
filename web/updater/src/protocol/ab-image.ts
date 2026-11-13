/**
 * Images for A/B devices (`protocol/sysex.md` §4.1 "Who takes uploads", §4.2 "A/B devices"): ESP32
 * builds whose app takes the upload into its inactive OTA slot. The image is the
 * chip's own app `.bin`, with no `.gfw` header, so the board id and version come from the user
 * and the device. It is padded with 0xFF to a multiple of 4; the size and the CRC-32 are over the
 * padded file; chunks go in plain order from offset 0.
 */

import { formatBoardId } from './boards.ts';
import type { Chunk } from './image.ts';
import { crc32 } from './image.ts';
import type { HelloInfo } from './messages.ts';
import { Capability, CHUNK_ALIGN_BYTES, Role } from './wire.ts';

/** The first byte of an ESP-IDF app image (`ESP_IMAGE_HEADER_MAGIC`). */
export const ESP_IMAGE_MAGIC = 0xe9;
/** What pads the last chunk: erased flash. */
const PAD_BYTE = 0xff;
/** The largest board id: a u14 on the wire. */
const MAX_BOARD_ID = 0x3fff;
/** The largest major and minor version (8 bits each). */
const MAX_MAJOR_MINOR = 0xff;
/** The largest patch version (16 bits). */
const MAX_PATCH = 0xffff;
/** Shift of the major version in `fw_version`. */
const MAJOR_SHIFT = 24;
/** Shift of the minor version in `fw_version`. */
const MINOR_SHIFT = 16;
/** `major.minor.patch`, decimal. */
const VERSION_PATTERN = /^(\d+)\.(\d+)\.(\d+)$/;

/**
 * Whether a device takes uploads in the app itself (§4.1): role 0, caps bit 0 and a non-zero
 * `max_chunk`. Such a device is never rebooted into a bootloader.
 *
 * @param info - Its HELLO.
 * @returns True for an A/B device.
 */
export function isAbDevice(info: HelloInfo): boolean {
  const hasUpload = (info.caps & Capability.FirmwareUpload) !== 0;
  return info.role === Role.App && hasUpload && info.maxChunk > 0;
}

/**
 * Reads `major.minor.patch` as `fw_version` (`major<<24 | minor<<16 | patch`).
 *
 * @param text - E.g. `1.2.3`.
 * @returns The packed version, or null when it is not one.
 */
export function parseVersion(text: string): number | null {
  const match = VERSION_PATTERN.exec(text.trim());
  if (match === null) {
    return null;
  }
  const [major, minor, patch] = match.slice(1).map(Number) as [number, number, number];
  const isInRange = major <= MAX_MAJOR_MINOR && minor <= MAX_MAJOR_MINOR && patch <= MAX_PATCH;
  if (isInRange === false) {
    return null;
  }
  return ((major << MAJOR_SHIFT) >>> 0) + (minor << MINOR_SHIFT) + patch;
}

/**
 * Pads an image with 0xFF to a multiple of 4 bytes.
 *
 * @param bytes - The file.
 * @returns The bytes sent, the same array when no padding is needed.
 */
export function padToWord(bytes: Uint8Array): Uint8Array {
  const extra = (CHUNK_ALIGN_BYTES - (bytes.length % CHUNK_ALIGN_BYTES)) % CHUNK_ALIGN_BYTES;
  if (extra === 0) {
    return bytes;
  }
  const padded = new Uint8Array(bytes.length + extra).fill(PAD_BYTE);
  padded.set(bytes);
  return padded;
}

/** An image ready for an A/B upload. */
export interface AbImage {
  /** The padded bytes: what FW_DATA sends. */
  bytes: Uint8Array;
  /** The file's own length, before padding. */
  fileBytes: number;
  /** FW_BEGIN `size`: the padded length. */
  size: number;
  /** FW_BEGIN `crc32`: zlib CRC-32 over the padded bytes. */
  crc32: number;
  /** FW_BEGIN `board_id`. */
  boardId: number;
  /** FW_BEGIN `fw_version`; HELLO must report it after the reboot. */
  fwVersion: number;
}

/** What checking an A/B image found. */
export type AbImageResult =
  | { ok: true; image: AbImage; warnings: string[] }
  | { ok: false; problems: string[]; warnings: string[] };

/** What {@link prepareAbImage} needs. */
export interface AbImageInput {
  bytes: Uint8Array;
  /** The board to send, normally HELLO's. */
  boardId: number | null;
  /** The version the user typed, `x.y.z`. */
  versionText: string;
  /** The connected device's board, to refuse a mismatch here rather than on the device. */
  deviceBoardId?: number | undefined;
}

/**
 * Checks and pads an A/B image. Nothing is sent until this passes.
 *
 * @param input - The file, the board, the version and the device's board.
 * @returns The image, or the problems.
 */
export function prepareAbImage(input: AbImageInput): AbImageResult {
  const { bytes, boardId, versionText, deviceBoardId } = input;
  const problems: string[] = [];
  const warnings: string[] = [];
  if (bytes.length === 0) {
    problems.push('The file is empty.');
  } else if (bytes[0] !== ESP_IMAGE_MAGIC) {
    warnings.push(
      'The first byte is not 0xE9, so this does not look like an ESP app image. A merged factory image is for the first install, not for an update.',
    );
  }
  const fwVersion = parseVersion(versionText);
  if (fwVersion === null) {
    problems.push('Give the version as major.minor.patch, e.g. 1.2.3.');
  }
  const isBoardValid = boardId !== null && Number.isInteger(boardId) && boardId >= 0 && boardId <= MAX_BOARD_ID;
  if (isBoardValid === false) {
    problems.push('Give the board id (0x0000–0x3FFF).');
  } else if (deviceBoardId !== undefined && boardId !== deviceBoardId) {
    problems.push(
      `The device is board ${formatBoardId(deviceBoardId)}, not ${formatBoardId(boardId ?? 0)}; it would refuse this upload.`,
    );
  }
  if (problems.length > 0 || fwVersion === null || boardId === null) {
    return { ok: false, problems, warnings };
  }
  const padded = padToWord(bytes);
  return {
    ok: true,
    warnings,
    image: { bytes: padded, fileBytes: bytes.length, size: padded.length, crc32: crc32(padded), boardId, fwVersion },
  };
}

/**
 * The A/B upload order: plain order from offset 0, each chunk a multiple of 4 and at most
 * `maxChunk`; no page rule.
 *
 * @param size - The padded size, a multiple of 4.
 * @param maxChunk - FW_BEGIN's `max_chunk`.
 * @returns The chunks, in the order to send them.
 */
export function planLinearChunks(size: number, maxChunk: number): Chunk[] {
  const chunkBytes = maxChunk - (maxChunk % CHUNK_ALIGN_BYTES);
  if (chunkBytes <= 0) {
    throw new RangeError(`max_chunk ${maxChunk} is smaller than ${CHUNK_ALIGN_BYTES} bytes`);
  }
  if (size <= 0 || size % CHUNK_ALIGN_BYTES !== 0) {
    throw new RangeError(`size ${size} is not a non-zero multiple of ${CHUNK_ALIGN_BYTES}`);
  }
  const chunks: Chunk[] = [];
  for (let offset = 0; offset < size; offset += chunkBytes) {
    chunks.push({ offset, length: Math.min(chunkBytes, size - offset) });
  }
  return chunks;
}

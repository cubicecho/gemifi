/**
 * The `.gfw` firmware image (`protocol/sysex.md` §5): header parsing, the checks the page makes
 * before sending anything, and the order its chunks are uploaded in (§4.2).
 */

import { type Board, boardById, formatBoardId, HEADER_OFFSETS } from './boards.ts';
import { CHUNK_ALIGN_BYTES, HEX_RADIX, PAGE_BYTES, PROTOCOL_VERSION } from './wire.ts';

/**
 * Where the header sits on a NUC1xx board, right after the 48-entry vector table. Other boards
 * put it elsewhere (`Board.headerOffset`); {@link findHeader} tries each.
 */
export const HEADER_OFFSET = 0xc0;
/** Header size. */
export const HEADER_SIZE = 32;
/** Byte offsets of the header's fields, from the header's start. */
const Field = {
  Magic: 0,
  HdrVersion: 4,
  BoardId: 6,
  ImageSize: 8,
  Crc32: 12,
  FwVersion: 16,
  ProtoVersion: 20,
} as const;
/** Offset of the header's `crc32` field inside a NUC1xx image. */
export const CRC_FIELD_OFFSET = HEADER_OFFSET + Field.Crc32;
/** `'GMFI'` read as a little-endian u32. */
export const IMAGE_MAGIC = 0x49464d47;
/** The header version this module reads. */
export const HEADER_VERSION = 1;
/** The largest board id: it travels as a u14 (§5). */
const MAX_BOARD_ID = 0x3fff;
/** Bytes in the CRC field. */
const CRC_FIELD_BYTES = 4;
/** Offset of the reset vector in the vector table. */
const RESET_VECTOR_OFFSET = 4;

/** The header fields. */
export interface ImageHeader {
  magic: number;
  hdrVersion: number;
  boardId: number;
  imageSize: number;
  crc32: number;
  fwVersion: number;
  protoVersion: number;
}

/** A `.gfw` file that passed every check. */
export interface FirmwareImage {
  header: ImageHeader;
  /** Where the header was found. */
  headerOffset: number;
  board: Board;
  /** Exactly `imageSize` bytes, as they will be written. */
  bytes: Uint8Array;
}

/** What {@link validateImage} found. */
export type ImageResult =
  | { ok: true; image: FirmwareImage }
  | { ok: false; problems: string[]; header: ImageHeader | null };

/** The reflected CRC-32/ISO-HDLC polynomial. */
const CRC_POLY = 0xedb88320;
/** CRC-32 lookup table, built once. */
const CRC_TABLE = (() => {
  const byteValues = 256;
  const bitsPerByte = 8;
  const table = new Uint32Array(byteValues);
  for (let n = 0; n < table.length; n++) {
    let c = n;
    for (let k = 0; k < bitsPerByte; k++) {
      c = c & 1 ? CRC_POLY ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c >>> 0;
  }
  return table;
})();

/**
 * CRC-32/ISO-HDLC, the same as zlib's `crc32`.
 *
 * @param bytes - The data.
 * @returns The unsigned CRC.
 */
export function crc32(bytes: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc = (CRC_TABLE[(crc ^ byte) & 0xff] ?? 0) ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/**
 * The image CRC as §5 defines it: over `[0, size)` with the header's crc field taken as 0.
 *
 * @param image - At least `size` bytes.
 * @param size - `image_size`.
 * @param headerOffset - Where the header is.
 * @returns The CRC.
 */
export function imageCrc(image: Uint8Array, size: number, headerOffset = HEADER_OFFSET): number {
  const copy = image.slice(0, size);
  const crcField = headerOffset + Field.Crc32;
  copy.fill(0, crcField, crcField + CRC_FIELD_BYTES);
  return crc32(copy);
}

/**
 * Reads the header, if the file is long enough to hold one.
 *
 * @param bytes - The whole file.
 * @param offset - Where the header is.
 * @returns The header, or null.
 */
export function readHeader(bytes: Uint8Array, offset = HEADER_OFFSET): ImageHeader | null {
  if (bytes.length < offset + HEADER_SIZE) {
    return null;
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset + offset, HEADER_SIZE);
  return {
    magic: view.getUint32(Field.Magic, true),
    hdrVersion: view.getUint16(Field.HdrVersion, true),
    boardId: view.getUint16(Field.BoardId, true),
    imageSize: view.getUint32(Field.ImageSize, true),
    crc32: view.getUint32(Field.Crc32, true),
    fwVersion: view.getUint32(Field.FwVersion, true),
    protoVersion: view.getUint32(Field.ProtoVersion, true),
  };
}

/**
 * Finds the header: at the expected board's offset first, then at every offset a known board uses,
 * taking the first that holds the magic and whose board puts its header there.
 *
 * @param bytes - The whole file.
 * @param expectedBoardId - The connected device's board, when known.
 * @returns The header and its offset; the header at the first offset tried when none matches.
 */
export function findHeader(
  bytes: Uint8Array,
  expectedBoardId?: number,
): { header: ImageHeader | null; offset: number } {
  const expected = expectedBoardId === undefined ? undefined : boardById(expectedBoardId)?.headerOffset;
  const offsets = [...new Set([...(expected === undefined ? [] : [expected]), ...HEADER_OFFSETS])];
  for (const offset of offsets) {
    const header = readHeader(bytes, offset);
    const isHere = header?.magic === IMAGE_MAGIC && boardById(header.boardId)?.headerOffset === offset;
    if (isHere) {
      return { header, offset };
    }
  }
  const first = offsets[0] ?? HEADER_OFFSET;
  return { header: readHeader(bytes, first), offset: first };
}

/**
 * @param bytes - A file's contents.
 * @returns Whether it holds the `.gfw` magic where some known board puts its header.
 */
export function hasGfwMagic(bytes: Uint8Array): boolean {
  return HEADER_OFFSETS.some((offset) => readHeader(bytes, offset)?.magic === IMAGE_MAGIC);
}

/**
 * Formats a number as `0x…` hex.
 *
 * @param value - The number.
 * @param digits - Minimum digits.
 * @returns E.g. `0x49464D47`.
 */
export function hex(value: number, digits = 8): string {
  return `0x${value.toString(HEX_RADIX).toUpperCase().padStart(digits, '0')}`;
}

/**
 * Checks a `.gfw` file before anything is sent: the header (magic, version, board, size), the
 * CRC over `[0, size)` with the crc field zeroed, and the vector table the bootloader also checks.
 *
 * @param bytes - The whole file.
 * @param expectedBoardId - The connected device's board, when known.
 * @returns The image, or every problem found.
 */
export function validateImage(bytes: Uint8Array, expectedBoardId?: number): ImageResult {
  const { header, offset } = findHeader(bytes, expectedBoardId);
  if (header === null) {
    return {
      ok: false,
      header: null,
      problems: [`The file is ${bytes.length} bytes, too short to hold a header at ${hex(offset, 0)}.`],
    };
  }
  const problems: string[] = [];
  if (header.magic !== IMAGE_MAGIC) {
    problems.push(`The magic is ${hex(header.magic)}, not 'GMFI' (${hex(IMAGE_MAGIC)}). Is this a .gfw file?`);
  }
  if (header.hdrVersion !== HEADER_VERSION) {
    problems.push(`Header version ${header.hdrVersion} is not supported (expected ${HEADER_VERSION}).`);
  }
  if (header.protoVersion !== PROTOCOL_VERSION) {
    problems.push(`The image speaks protocol ${header.protoVersion}; this page speaks ${PROTOCOL_VERSION}.`);
  }
  const board = boardById(header.boardId);
  if (board === undefined) {
    problems.push(`Board id ${formatBoardId(header.boardId)} is not a known gemifi board.`);
  }
  if (board !== undefined && board.headerOffset !== offset) {
    problems.push(
      `The header is at ${hex(offset, 0)}, but ${board.name} images have it at ${hex(board.headerOffset, 0)}.`,
    );
  }
  if (header.boardId > MAX_BOARD_ID) {
    problems.push(`Board id ${formatBoardId(header.boardId)} is above 0x3FFF and cannot be sent as a u14.`);
  }
  const isWrongBoard = expectedBoardId !== undefined && header.boardId !== expectedBoardId;
  if (isWrongBoard) {
    problems.push(
      `The image is for board ${formatBoardId(header.boardId)} but the device is board ${formatBoardId(expectedBoardId)}.`,
    );
  }
  const size = header.imageSize;
  if (size % CHUNK_ALIGN_BYTES !== 0) {
    problems.push(`The image size ${size} is not a multiple of ${CHUNK_ALIGN_BYTES}.`);
  }
  if (size < offset + HEADER_SIZE) {
    problems.push(`The image size ${size} does not even cover the header.`);
  }
  if (board !== undefined && size > board.apromBytes) {
    problems.push(`The image is ${size} bytes; ${board.name} has ${board.apromBytes} bytes of ${board.areaName}.`);
  }
  if (bytes.length !== size) {
    problems.push(`The file is ${bytes.length} bytes but its header says the image is ${size}.`);
  }
  const canCheckContents = problems.length === 0 && board !== undefined;
  if (canCheckContents) {
    const actualCrc = imageCrc(bytes, size, offset);
    if (actualCrc !== header.crc32) {
      problems.push(`The CRC is ${hex(actualCrc)} but the header says ${hex(header.crc32)}. The file is damaged.`);
    }
    problems.push(...vectorProblems(bytes, size, board));
  }
  if (problems.length > 0 || board === undefined) {
    return { ok: false, header, problems };
  }
  return { ok: true, image: { header, headerOffset: offset, board, bytes: bytes.slice(0, size) } };
}

/**
 * The vector-table checks the bootloader makes before starting the app (§5).
 *
 * @param bytes - The image.
 * @param size - `image_size`.
 * @param board - The board it is for.
 * @returns Problems, if any.
 */
function vectorProblems(bytes: Uint8Array, size: number, board: Board): string[] {
  const view = new DataView(bytes.buffer, bytes.byteOffset, size);
  const stackPointer = view.getUint32(0, true);
  const resetVector = view.getUint32(RESET_VECTOR_OFFSET, true);
  const problems: string[] = [];
  const sramEnd = board.sramStart + board.sramBytes;
  const isStackInSram = stackPointer > board.sramStart && stackPointer <= sramEnd;
  if (isStackInSram === false) {
    problems.push(`The initial stack pointer ${hex(stackPointer)} is not in SRAM; the bootloader would refuse it.`);
  }
  const isThumb = (resetVector & 1) === 1;
  const isInsideImage = resetVector >= board.imageBase && resetVector - board.imageBase < size;
  const isResetValid = isThumb && isInsideImage;
  if (isResetValid === false) {
    problems.push(
      `The reset vector ${hex(resetVector)} is not odd and inside the image; the bootloader would refuse it.`,
    );
  }
  return problems;
}

/** One FW_DATA chunk. */
export interface Chunk {
  /** Where it goes. */
  offset: number;
  /** How many bytes. */
  length: number;
}

/**
 * The upload order of §4.2: from 0x200 upward, then page 0 last. No chunk crosses a 512-byte
 * page, and each is a multiple of 4 bytes and at most `maxChunk`.
 *
 * @param size - Image size, a multiple of 4.
 * @param maxChunk - The device's `max_chunk`.
 * @returns The chunks, in the order to send them.
 */
export function planChunks(size: number, maxChunk: number): Chunk[] {
  const chunkBytes = maxChunk - (maxChunk % CHUNK_ALIGN_BYTES);
  if (chunkBytes <= 0) {
    throw new RangeError(`max_chunk ${maxChunk} is smaller than ${CHUNK_ALIGN_BYTES} bytes`);
  }
  const chunks: Chunk[] = [];
  const addRange = (start: number, end: number) => {
    let offset = start;
    while (offset < end) {
      const pageEnd = (Math.floor(offset / PAGE_BYTES) + 1) * PAGE_BYTES;
      const length = Math.min(chunkBytes, end - offset, pageEnd - offset);
      chunks.push({ offset, length });
      offset += length;
    }
  };
  addRange(PAGE_BYTES, size);
  addRange(0, Math.min(PAGE_BYTES, size));
  return chunks;
}

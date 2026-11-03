/** The boards gemifi knows (`protocol/sysex.md` §7) and the memory facts an image is checked against. */

import { BOARD_ID_DIGITS, Capability, HEX_RADIX, Role } from './wire.ts';

/** One board. */
export interface Board {
  /** `board_id`. */
  id: number;
  /** What the board is called. */
  name: string;
  /** Application flash size; an image may not be larger. */
  apromBytes: number;
  /** Start of SRAM, for the initial stack pointer check. */
  sramStart: number;
  /** SRAM size. */
  sramBytes: number;
  /** Where the `.gfw` header sits in the image: right after the vector table (§5). */
  headerOffset: number;
  /** The flash address the image is linked at, which the reset vector is checked against. */
  imageBase: number;
  /** What the size limit is called in messages. */
  areaName: string;
  /** How an image gets onto the board: the gemifi bootloader, or M-Audio's stock bootloader. */
  update: BoardUpdate;
}

/** How a board takes a new app image. */
export const BoardUpdate = {
  /** gemifi's own bootloader, FW_* over the gemifi protocol (§4.2). */
  Gemifi: 'gemifi',
  /** M-Audio's stock bootloader and its SysEx update protocol (`src/vendor/maudio.ts`). */
  MaudioVendor: 'maudio-vendor',
} as const;
export type BoardUpdate = (typeof BoardUpdate)[keyof typeof BoardUpdate];

/** Where the header sits on a NUC1xx: after 16 core + 32 IRQ vectors. */
const NUC_HEADER_OFFSET = 0xc0;
/** Where the header sits on the Code 61's STM32F1: after 16 core + 60 IRQ vectors. */
const STM32F1_HEADER_OFFSET = 0x130;
/** `board_id` of the M-Audio Code 61. */
export const CODE61_BOARD_ID = 2;
/** The Code 61 app area starts after M-Audio's 9 KB bootloader. */
export const CODE61_APP_BASE = 0x0800_2400;
/** The vendor's image checksum, at the end of the app area. */
const CODE61_IMAGE_CHECKSUM = 0x0801_b3fe;
/** The app may run up to the checksum, rounded down to 4 bytes. */
const CODE61_APP_BYTES = (CODE61_IMAGE_CHECKSUM & ~3) - CODE61_APP_BASE;

/** Every board by id. */
export const BOARDS: Readonly<Record<number, Board>> = Object.freeze({
  1: Object.freeze({
    id: 1,
    name: 'M-Audio Keystation Mini 32 MK3',
    apromBytes: 32_768,
    sramStart: 0x2000_0000,
    sramBytes: 4096,
    headerOffset: NUC_HEADER_OFFSET,
    imageBase: 0,
    areaName: 'APROM',
    update: BoardUpdate.Gemifi,
  }),
  [CODE61_BOARD_ID]: Object.freeze({
    id: CODE61_BOARD_ID,
    name: 'M-Audio Code 61',
    apromBytes: CODE61_APP_BYTES,
    sramStart: 0x2000_0000,
    // An STM32F103-class part, maybe a clone: 64 KB is the family's upper bound.
    sramBytes: 0x1_0000,
    headerOffset: STM32F1_HEADER_OFFSET,
    imageBase: CODE61_APP_BASE,
    areaName: 'the app area',
    update: BoardUpdate.MaudioVendor,
  }),
});

/** Every header offset a known board uses. */
export const HEADER_OFFSETS: readonly number[] = Object.freeze([
  ...new Set(Object.values(BOARDS).map((board) => board.headerOffset)),
]);

/**
 * Looks up a board.
 *
 * @param id - `board_id`.
 * @returns The board, or undefined when it is not one gemifi knows.
 */
export function boardById(id: number): Board | undefined {
  return BOARDS[id];
}

/**
 * Formats a board id as the spec writes it.
 *
 * @param id - `board_id`.
 * @returns E.g. `0x0001`.
 */
export function formatBoardId(id: number): string {
  return `0x${id.toString(HEX_RADIX).toUpperCase().padStart(BOARD_ID_DIGITS, '0')}`;
}

/**
 * Names a board for display.
 *
 * @param id - `board_id`.
 * @returns Its name and id, or the bare id in hex.
 */
export function boardLabel(id: number): string {
  const hex = formatBoardId(id);
  const board = boardById(id);
  return board ? `${board.name} (${hex})` : `unknown board ${hex}`;
}

/**
 * Whether an image for `board` goes through M-Audio's bootloader on this device. A Code 61 can carry
 * either bootloader: a device already in a gemifi bootloader (HELLO role 1, which only a gemifi
 * bootloader answers), or an app that reports caps bit 7, takes the gemifi upload; otherwise the board's
 * default applies, so a Code 61 with its stock bootloader, or one not connected, keeps the vendor path.
 *
 * @param board - The image's board.
 * @param device - The connected device's HELLO, or null.
 * @returns True for the vendor path.
 */
export function usesVendorUpdate(board: Board, device: { role: number; caps: number } | null): boolean {
  if (board.update !== BoardUpdate.MaudioVendor) {
    return false;
  }
  if (device === null) {
    return true;
  }
  const isGemifiBoot = device.role === Role.Boot || (device.caps & Capability.GemifiBoot) !== 0;
  return isGemifiBoot === false;
}

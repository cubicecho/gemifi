/** The boards gemifi knows (`protocol/sysex.md` §7) and the memory facts an image is checked against. */

import { BOARD_ID_DIGITS, HEX_RADIX } from './wire.ts';

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
}

/** Every board by id. */
export const BOARDS: Readonly<Record<number, Board>> = Object.freeze({
  1: Object.freeze({
    id: 1,
    name: 'M-Audio Keystation Mini 32 MK3',
    apromBytes: 32_768,
    sramStart: 0x2000_0000,
    sramBytes: 4096,
  }),
});

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

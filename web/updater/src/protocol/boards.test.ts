import { describe, expect, it } from 'vitest';
import {
  type Board,
  BoardUpdate,
  boardById,
  boardFromLayout,
  CODE61_BOARD_ID,
  isDiyBoardId,
  usesVendorUpdate,
} from './boards.ts';
import { Capability, Role } from './wire.ts';

function board(id: number): Board {
  const found = boardById(id);
  if (found === undefined) {
    throw new Error(`no board ${id}`);
  }
  return found;
}

const code61 = board(CODE61_BOARD_ID);
const keystation = board(1);

describe('usesVendorUpdate', () => {
  it('keeps the vendor path for a Code 61 app without caps bit 7, or no device', () => {
    expect(usesVendorUpdate(code61, { role: Role.App, caps: Capability.Log })).toBe(true);
    expect(usesVendorUpdate(code61, null)).toBe(true);
  });

  it('takes the gemifi path when the app reports a gemifi bootloader', () => {
    expect(usesVendorUpdate(code61, { role: Role.App, caps: Capability.Log | Capability.GemifiBoot })).toBe(false);
  });

  it('takes the gemifi path when the device is already in a gemifi bootloader', () => {
    expect(usesVendorUpdate(code61, { role: Role.Boot, caps: Capability.FirmwareUpload })).toBe(false);
  });

  it('never uses the vendor path for a gemifi board', () => {
    expect(usesVendorUpdate(keystation, null)).toBe(false);
    expect(usesVendorUpdate(keystation, { role: Role.App, caps: 0 })).toBe(false);
  });
});

/** The layout.json a Blue Pill build writes (tools/builder/gemifi_board.py image_layout). */
const BLUEPILL_LAYOUT = {
  gemifi_board_layout: 1,
  board_id: 0x3f03,
  profile: 'generic-stm32f103c8',
  name: 'Generic STM32F103C8 (Blue Pill)',
  platform: 'stm32f1',
  hdr_offset: 0x130,
  base: 0x0800_2400,
  max_size: 0xcc00,
  area: 'the app area',
  sram_start: 0x2000_0000,
  sram_end: 0x2000_5000,
  page: 1024,
};

describe('boardFromLayout (a DIY board, issue #116)', () => {
  it('reads the layout a build writes next to its .gfw', () => {
    const read = boardFromLayout(JSON.stringify(BLUEPILL_LAYOUT));
    expect(read).toMatchObject({
      id: 0x3f03,
      apromBytes: 0xcc00,
      sramBytes: 0x5000,
      headerOffset: 0x130,
      imageBase: 0x0800_2400,
      update: BoardUpdate.Gemifi,
    });
    expect(typeof read !== 'string' && boardById(0x3f03, [read])).toBe(read);
    expect(boardById(0x3f03)).toBeUndefined();
    expect(isDiyBoardId(0x3f03)).toBe(true);
    expect(isDiyBoardId(2)).toBe(false);
  });

  it('refuses what is not a DIY layout', () => {
    expect(boardFromLayout('{')).toMatch(/not JSON/);
    expect(boardFromLayout('{"a": 1}')).toMatch(/not a gemifi board layout/);
    expect(boardFromLayout(JSON.stringify({ ...BLUEPILL_LAYOUT, board_id: 2 }))).toMatch(/not a DIY board/);
    expect(boardFromLayout(JSON.stringify({ ...BLUEPILL_LAYOUT, hdr_offset: 0xc0 }))).toMatch(/platform/);
    expect(boardFromLayout(JSON.stringify({ ...BLUEPILL_LAYOUT, base: 'x' }))).toMatch(/missing/);
  });
});

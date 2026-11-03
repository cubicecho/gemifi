import { describe, expect, it } from 'vitest';
import { type Board, boardById, CODE61_BOARD_ID, usesVendorUpdate } from './boards.ts';
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

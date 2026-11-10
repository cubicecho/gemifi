import { describe, expect, it } from 'vitest';
import { boardFromLayout } from './boards.ts';
import { CRC_FIELD_OFFSET, crc32, imageCrc, planChunks, readHeader, validateImage } from './image.ts';
import { makeTestImage } from './test-support.ts';

describe('crc32', () => {
  it('is CRC-32/ISO-HDLC (zlib)', () => {
    expect(crc32(new TextEncoder().encode('123456789'))).toBe(0xcbf43926);
    expect(crc32(new Uint8Array())).toBe(0);
  });

  it('takes the header crc field as 0', () => {
    const image = makeTestImage();
    const zeroed = image.slice();
    zeroed.fill(0, CRC_FIELD_OFFSET, CRC_FIELD_OFFSET + 4);
    expect(imageCrc(image, image.length)).toBe(crc32(zeroed));
  });
});

describe('validateImage (§5)', () => {
  it('finds a Code 61 header at 0x130, linked at 0x08002400', () => {
    const code61 = { boardId: 2, headerOffset: 0x130, stackPointer: 0x2000_40b0, resetVector: 0x0800_2545 };
    const result = validateImage(makeTestImage(code61), 2);
    expect(result.ok && result.image.headerOffset).toBe(0x130);
    expect(validateImage(makeTestImage(code61)).ok).toBe(true);
    const outside = validateImage(makeTestImage({ ...code61, resetVector: 0x0000_0101 }));
    expect(outside.ok === false && outside.problems.join()).toMatch(/reset vector/);
    const wrongDevice = validateImage(makeTestImage(code61), 1);
    expect(wrongDevice.ok === false && wrongDevice.problems.join()).toMatch(/device is board 0x0001/);
  });

  it('checks a DIY board image against the layout from its build', () => {
    const bluepill = { boardId: 0x3f03, headerOffset: 0x130, stackPointer: 0x2000_4f00, resetVector: 0x0800_2545 };
    const image = makeTestImage(bluepill);
    const alone = validateImage(image, 0x3f03);
    expect(alone.ok === false && alone.problems.join()).toMatch(/DIY board: add the \.board\.json/);
    const board = boardFromLayout(
      JSON.stringify({
        gemifi_board_layout: 1,
        board_id: 0x3f03,
        platform: 'stm32f1',
        hdr_offset: 0x130,
        base: 0x0800_2400,
        max_size: 0xcc00,
        sram_start: 0x2000_0000,
        sram_end: 0x2000_5000,
      }),
    );
    if (typeof board === 'string') {
      throw new Error(board);
    }
    const result = validateImage(image, 0x3f03, [board]);
    expect(result.ok && result.image.board.id).toBe(0x3f03);
    const tooBig = validateImage(makeTestImage(bluepill), undefined, [{ ...board, apromBytes: 512 }]);
    expect(tooBig.ok === false && tooBig.problems.join()).toMatch(/512 bytes of the app area/);
    const wrongStack = validateImage(makeTestImage({ ...bluepill, stackPointer: 0x2000_6000 }), 0x3f03, [board]);
    expect(wrongStack.ok === false && wrongStack.problems.join()).toMatch(/stack pointer/);
  });

  it('accepts a well-formed image', () => {
    const result = validateImage(makeTestImage(), 1);
    expect(result.ok).toBe(true);
    expect(readHeader(makeTestImage())).toMatchObject({ magic: 0x49464d47, boardId: 1, imageSize: 1024 });
  });

  it('finds a damaged byte', () => {
    const image = makeTestImage();
    image[700] = (image[700] ?? 0) ^ 0x01;
    const result = validateImage(image);
    expect(result.ok).toBe(false);
    expect(result.ok === false && result.problems.join()).toMatch(/CRC/);
  });

  it('rejects the wrong magic, an unknown board, the wrong device and a short file', () => {
    expect(validateImage(makeTestImage({ magic: 0x12345678 })).ok).toBe(false);
    expect(validateImage(makeTestImage({ boardId: 0x0003 })).ok).toBe(false);
    const misplaced = validateImage(makeTestImage({ boardId: 0x0002 }));
    expect(misplaced.ok === false && misplaced.problems.join()).toMatch(/have it at 0x130/);
    const wide = validateImage(makeTestImage({ boardId: 0x4001 }));
    expect(wide.ok === false && wide.problems.join()).toMatch(/above 0x3FFF/);
    const wrongDevice = validateImage(makeTestImage(), 0x0003);
    expect(wrongDevice.ok === false && wrongDevice.problems.join()).toMatch(/device is board 0x0003/);
    expect(validateImage(new Uint8Array(100)).ok).toBe(false);
  });

  it('rejects a size that disagrees with the file, is unaligned or too big', () => {
    const image = makeTestImage();
    const padded = new Uint8Array(image.length + 4);
    padded.set(image);
    expect(validateImage(padded).ok).toBe(false);
    expect(validateImage(makeTestImage({ size: 1022 })).ok).toBe(false);
    expect(validateImage(makeTestImage({ size: 32 * 1024 + 512 })).ok).toBe(false);
  });

  it('rejects a vector table the bootloader would refuse', () => {
    expect(validateImage(makeTestImage({ stackPointer: 0x1000_0000 })).ok).toBe(false);
    expect(validateImage(makeTestImage({ resetVector: 0x100 })).ok).toBe(false);
    expect(validateImage(makeTestImage({ resetVector: 0x10001 })).ok).toBe(false);
  });
});

describe('planChunks (§4.2)', () => {
  it('sends 0x200 upward, then page 0 last', () => {
    expect(planChunks(1024, 256)).toEqual([
      { offset: 512, length: 256 },
      { offset: 768, length: 256 },
      { offset: 0, length: 256 },
      { offset: 256, length: 256 },
    ]);
  });

  it('never crosses a page and keeps chunks a multiple of 4', () => {
    const chunks = planChunks(1100, 250);
    expect(chunks).toEqual([
      { offset: 512, length: 248 },
      { offset: 760, length: 248 },
      { offset: 1008, length: 16 },
      { offset: 1024, length: 76 },
      { offset: 0, length: 248 },
      { offset: 248, length: 248 },
      { offset: 496, length: 16 },
    ]);
    for (const { offset, length } of chunks) {
      expect(length % 4).toBe(0);
      expect(Math.floor(offset / 512)).toBe(Math.floor((offset + length - 1) / 512));
    }
  });

  it('covers every byte exactly once', () => {
    const size = 32 * 1024;
    const seen = new Uint8Array(size);
    for (const { offset, length } of planChunks(size, 256)) {
      for (let i = offset; i < offset + length; i++) {
        seen[i] = (seen[i] ?? 0) + 1;
      }
    }
    expect(seen.every((n) => n === 1)).toBe(true);
  });

  it('handles an image no bigger than page 0', () => {
    expect(planChunks(256, 256)).toEqual([{ offset: 0, length: 256 }]);
  });
});

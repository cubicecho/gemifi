import { describe, expect, it } from 'vitest';
import { isAbDevice, parseVersion, planLinearChunks, prepareAbImage } from './ab-image.ts';
import { SysexClient, type Transport } from './client.ts';
import { encodeU14, encodeU28, encodeU32, PayloadReader, pack, unpack } from './encoding.ts';
import { type DeviceLink, FlashError, FlashPhase, flashAbFirmware } from './flash.ts';
import { decodeFrame, encodeFrame } from './frame.ts';
import { crc32 } from './image.ts';
import type { HelloInfo } from './messages.ts';
import { SysexStreamParser } from './stream.ts';
import { Capability, Command, RebootMode, Role, Status } from './wire.ts';

const FAST = { replyTimeoutMs: 15, longReplyTimeoutMs: 60, maxRetries: 5 };
const RESTART = { abRestartTimeoutMs: 1000, abPollMs: 10 };
const BOARD = 0x3f00;
const V_OLD = 0x0001_0000;
const V_NEW = 0x0102_0003;

/** An ESP-style app image: 0xE9 first, then a pattern. */
function espBin(size: number): Uint8Array {
  const bytes = new Uint8Array(size);
  for (let i = 0; i < size; i++) {
    bytes[i] = (i * 31 + 7) & 0xff;
  }
  bytes[0] = 0xe9;
  return bytes;
}

/**
 * An A/B app (ESP32 OTA, §4.2 "A/B devices") behind a byte stream, like a USB-UART: the app takes
 * FW_* itself in plain order from 0; REBOOT(0) after a good FW_END boots the new slot.
 */
class FakeAbDevice {
  boardId = BOARD;
  fwVersion = V_OLD;
  maxChunk = 256;
  /** What the running slot holds. */
  slot: Uint8Array | null = null;
  chunks: Array<{ offset: number; length: number }> = [];
  begin: { size: number; crc: number; boardId: number; fwVersion: number } | null = null;
  reboots: number[] = [];
  messages: number[] = [];
  /** Keep the old slot on reboot, as when the new one does not boot. */
  failNewSlot = false;
  /** Over USB the port goes away during the reboot; over serial it stays. */
  isUsb = false;
  #inactive: number[] = [];
  #pending: { bytes: Uint8Array; fwVersion: number } | null = null;
  #rx = new Set<(bytes: Uint8Array) => void>();
  #booting = false;
  #client: SysexClient | null = null;

  link(): DeviceLink {
    this.#client = this.#newClient();
    return {
      current: () => this.#client,
      waitFor: () => Promise.reject(new Error('A/B uploads never wait for a bootloader port.')),
    };
  }

  /** A client over a byte-stream transport: frames split into 3-byte pieces, boot log noise first. */
  #newClient(): SysexClient {
    const parser = new SysexStreamParser();
    const transport: Transport = {
      send: (bytes) => queueMicrotask(() => this.#handleBytes(bytes)),
      subscribe: (listener) => {
        const onBytes = (bytes: Uint8Array) => {
          for (const message of parser.push(bytes)) {
            listener(message);
          }
        };
        this.#rx.add(onBytes);
        return () => this.#rx.delete(onBytes);
      },
    };
    return new SysexClient(transport, FAST);
  }

  #emit(bytes: ArrayLike<number>): void {
    const all = Array.from(bytes);
    for (let i = 0; i < all.length; i += 3) {
      const piece = Uint8Array.from(all.slice(i, i + 3));
      for (const listener of this.#rx) {
        listener(piece);
      }
    }
  }

  #reply(cmd: number, seq: number, payload: number[]): void {
    this.#emit([0xfe, ...encodeFrame(cmd | 0x40, seq, payload)]);
  }

  #handleBytes(bytes: Uint8Array): void {
    if (this.#booting) {
      return;
    }
    const result = decodeFrame(bytes);
    if (result.ok === false) {
      return;
    }
    const { cmd, seq, payload } = result.frame;
    this.messages.push(cmd);
    const reader = new PayloadReader(payload);
    switch (cmd) {
      case Command.Hello:
        this.#reply(cmd, seq, [
          Status.Ok,
          Role.App,
          ...encodeU14(this.boardId),
          ...encodeU32(this.fwVersion),
          1,
          ...encodeU14(this.maxChunk),
          ...encodeU14(Capability.FirmwareUpload | Capability.Log),
          ...pack(new Array(12).fill(0xab)),
        ]);
        return;
      case Command.Reboot: {
        const mode = reader.byte();
        this.reboots.push(mode);
        this.#reply(cmd, seq, [Status.Ok]);
        this.#restart(mode);
        return;
      }
      case Command.FwBegin: {
        const begin = { size: reader.u32(), crc: reader.u32(), boardId: reader.u14(), fwVersion: reader.u32() };
        if (begin.boardId !== this.boardId) {
          this.#reply(cmd, seq, [Status.BadBoard]);
          return;
        }
        this.begin = begin;
        this.#inactive = [];
        this.chunks = [];
        this.#reply(cmd, seq, [Status.Ok, ...encodeU14(this.maxChunk)]);
        return;
      }
      case Command.FwData: {
        const begin = this.begin;
        const offset = reader.u28();
        const data = unpack(payload.slice(4));
        const isBad =
          begin === null ||
          offset !== this.#inactive.length ||
          data.length % 4 !== 0 ||
          data.length > this.maxChunk ||
          offset + data.length > begin.size;
        if (isBad) {
          this.#reply(cmd, seq, [Status.BadAddr]);
          return;
        }
        this.#inactive.push(...data);
        this.chunks.push({ offset, length: data.length });
        this.#reply(cmd, seq, [Status.Ok, ...encodeU28(offset)]);
        return;
      }
      case Command.FwEnd: {
        const begin = this.begin;
        const bytes = Uint8Array.from(this.#inactive);
        const isGood = begin !== null && bytes.length === begin.size && crc32(bytes) === begin.crc;
        if (isGood && begin !== null) {
          this.#pending = { bytes, fwVersion: begin.fwVersion };
        }
        this.#reply(cmd, seq, [isGood ? Status.Ok : Status.BadImage]);
        return;
      }
      case Command.FwAbort:
        this.begin = null;
        this.#reply(cmd, seq, [Status.Ok]);
        return;
      default:
        this.#reply(cmd, seq, [Status.BadCmd]);
    }
  }

  /** Boots for 30 ms (deaf, printing a boot log), then runs the new slot after a good FW_END. */
  #restart(mode: number): void {
    setTimeout(() => {
      this.#booting = true;
      if (this.isUsb) {
        this.#client?.close();
        this.#client = null;
      }
      this.#emit(new TextEncoder().encode('rst:0xc (SW_CPU_RESET),boot:0x13\r\n'));
      setTimeout(() => {
        if (mode === RebootMode.App && this.#pending !== null && this.failNewSlot === false) {
          this.slot = this.#pending.bytes;
          this.fwVersion = this.#pending.fwVersion;
        }
        this.#pending = null;
        this.#booting = false;
        if (this.isUsb) {
          this.#client = this.#newClient();
        }
      }, 30);
    }, 2);
  }
}

/** Prepares an image or fails the test. */
function ready(bytes: Uint8Array, boardId = BOARD, versionText = '1.2.3') {
  const result = prepareAbImage({ bytes, boardId, versionText });
  if (result.ok === false) {
    throw new Error(result.problems.join());
  }
  return result.image;
}

describe('A/B images', () => {
  it('pads with 0xFF to a multiple of 4 and takes the CRC over the padded file', () => {
    const image = ready(espBin(1001));
    expect(image.fileBytes).toBe(1001);
    expect(image.size).toBe(1004);
    expect(Array.from(image.bytes.subarray(1001))).toEqual([0xff, 0xff, 0xff]);
    expect(image.crc32).toBe(crc32(image.bytes));
    expect(ready(espBin(1000)).size).toBe(1000);
  });

  it('reads versions', () => {
    expect(parseVersion('1.2.3')).toBe(V_NEW);
    expect(parseVersion(' 255.255.65535 ')).toBe(0xffff_ffff);
    expect(parseVersion('1.2')).toBeNull();
    expect(parseVersion('1.256.0')).toBeNull();
    expect(parseVersion('v1.2.3')).toBeNull();
  });

  it('refuses a missing version, a bad board and a board that is not the device', () => {
    const bytes = espBin(64);
    expect(prepareAbImage({ bytes, boardId: BOARD, versionText: '' }).ok).toBe(false);
    expect(prepareAbImage({ bytes, boardId: null, versionText: '1.0.0' }).ok).toBe(false);
    expect(prepareAbImage({ bytes, boardId: 0x4000, versionText: '1.0.0' }).ok).toBe(false);
    const mismatch = prepareAbImage({ bytes, boardId: BOARD, versionText: '1.0.0', deviceBoardId: 1 });
    expect(mismatch.ok).toBe(false);
    expect(mismatch.ok === false && mismatch.problems.join()).toMatch(/0x0001/);
    expect(prepareAbImage({ bytes: new Uint8Array(0), boardId: BOARD, versionText: '1.0.0' }).ok).toBe(false);
  });

  it('warns about a file that is not an ESP app image', () => {
    const result = prepareAbImage({ bytes: new Uint8Array(16), boardId: BOARD, versionText: '1.0.0' });
    expect(result.ok).toBe(true);
    expect(result.warnings).toHaveLength(1);
  });

  it('plans plain order from 0 in multiples of 4', () => {
    expect(planLinearChunks(1004, 256)).toEqual([
      { offset: 0, length: 256 },
      { offset: 256, length: 256 },
      { offset: 512, length: 256 },
      { offset: 768, length: 236 },
    ]);
    expect(planLinearChunks(16, 6)).toHaveLength(4);
    expect(() => planLinearChunks(16, 3)).toThrow(RangeError);
    expect(() => planLinearChunks(10, 8)).toThrow(RangeError);
  });

  it('tells A/B devices apart by role, caps bit 0 and max_chunk', () => {
    const base: HelloInfo = {
      role: Role.App,
      boardId: BOARD,
      fwVersion: 0,
      protoVersion: 1,
      maxChunk: 256,
      caps: Capability.FirmwareUpload,
      uid: [],
    };
    expect(isAbDevice(base)).toBe(true);
    expect(isAbDevice({ ...base, maxChunk: 0 })).toBe(false);
    expect(isAbDevice({ ...base, caps: Capability.Config })).toBe(false);
    expect(isAbDevice({ ...base, role: Role.Boot })).toBe(false);
  });
});

describe('flashAbFirmware', () => {
  for (const isUsb of [false, true]) {
    it(`uploads in plain order from 0, sends REBOOT(0) only and checks the new version (${isUsb ? 'USB' : 'serial'})`, async () => {
      const device = new FakeAbDevice();
      device.isUsb = isUsb;
      device.maxChunk = 200;
      const image = ready(espBin(2001));
      const phases: string[] = [];
      const info = await flashAbFirmware({
        image,
        link: device.link(),
        reboot: RESTART,
        onProgress: (p) => {
          if (phases.at(-1) !== p.phase) {
            phases.push(p.phase);
          }
        },
      });
      expect(info.fwVersion).toBe(V_NEW);
      expect(Array.from(device.slot ?? [])).toEqual(Array.from(image.bytes));
      expect(device.begin).toEqual({ size: 2004, crc: crc32(image.bytes), boardId: BOARD, fwVersion: V_NEW });
      let next = 0;
      for (const chunk of device.chunks) {
        expect(chunk.offset).toBe(next);
        expect(chunk.length % 4).toBe(0);
        expect(chunk.length).toBeLessThanOrEqual(200);
        next += chunk.length;
      }
      expect(next).toBe(2004);
      expect(device.reboots).toEqual([RebootMode.App]);
      expect(device.messages.indexOf(Command.Reboot)).toBe(device.messages.indexOf(Command.FwEnd) + 1);
      expect(device.messages.at(-1)).toBe(Command.Hello);
      expect(phases).toEqual([
        FlashPhase.Checking,
        FlashPhase.Beginning,
        FlashPhase.Writing,
        FlashPhase.Verifying,
        FlashPhase.StartingApp,
        FlashPhase.Done,
      ]);
    });
  }

  it('refuses a board mismatch before FW_BEGIN', async () => {
    const device = new FakeAbDevice();
    const error = await flashAbFirmware({
      image: ready(espBin(64), 0x3f01),
      link: device.link(),
      reboot: RESTART,
    }).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(FlashError);
    expect((error as FlashError).message).toMatch(/0x3F00/);
    expect(device.messages).toEqual([Command.Hello]);
  });

  it('reports a new slot that did not boot', async () => {
    const device = new FakeAbDevice();
    device.failNewSlot = true;
    const error = await flashAbFirmware({ image: ready(espBin(64)), link: device.link(), reboot: RESTART }).catch(
      (e: unknown) => e,
    );
    expect((error as FlashError).phase).toBe(FlashPhase.StartingApp);
    expect((error as FlashError).message).toMatch(/not 1\.2\.3/);
  });

  it('reports BAD_IMAGE from FW_END and aborts without rebooting', async () => {
    const device = new FakeAbDevice();
    const image = ready(espBin(64));
    const error = await flashAbFirmware({
      image: { ...image, crc32: image.crc32 ^ 1 },
      link: device.link(),
      reboot: RESTART,
    }).catch((e: unknown) => e);
    expect((error as FlashError).phase).toBe(FlashPhase.Verifying);
    expect((error as FlashError).message).toMatch(/BAD_IMAGE/);
    expect(device.messages.at(-1)).toBe(Command.FwAbort);
    expect(device.reboots).toEqual([]);
  });
});

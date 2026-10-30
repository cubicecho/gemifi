import { describe, expect, it } from 'vitest';
import { RequestError, RequestFailure, SysexClient, type Transport } from './client.ts';
import { encodeU14, encodeU28, encodeU32, PayloadReader, pack, unpack } from './encoding.ts';
import { type DeviceLink, FlashError, FlashPhase, flashFirmware } from './flash.ts';
import { decodeFrame, encodeFrame } from './frame.ts';
import { imageCrc, validateImage } from './image.ts';
import { PortRole } from './ports.ts';
import { makeTestImage } from './test-support.ts';
import { Command, PAGE_BYTES, Status } from './wire.ts';

const FAST = { replyTimeoutMs: 15, longReplyTimeoutMs: 60, maxRetries: 5 };
const APROM = 32 * 1024;

/** A bootloader and app in one, following the §4.2 rules, behind an in-memory transport. */
class FakeDevice {
  role: 'app' | 'boot' = 'app';
  flash = new Uint8Array(APROM).fill(0xff);
  fwVersion = 0x0001_0000;
  programmed = new Map<number, number>();
  /** Replies to drop, by request command, once each. */
  dropReplies: number[] = [];
  /** A status to answer FW_END with instead of checking. */
  endStatus: number | null = null;
  messages: number[] = [];
  #listeners = new Set<(bytes: Uint8Array) => void>();
  #upload: { size: number; crc: number; fwVersion: number; page0Started: boolean } | null = null;
  #last: { cmd: number; seq: number; reply: Uint8Array } | null = null;
  #generation = 0;
  #waiters: Array<() => void> = [];

  transport(): Transport {
    const generation = this.#generation;
    return {
      send: (bytes) => {
        if (generation === this.#generation) {
          queueMicrotask(() => this.#handle(bytes));
        }
      },
      subscribe: (listener) => {
        this.#listeners.add(listener);
        return () => this.#listeners.delete(listener);
      },
    };
  }

  link(): DeviceLink {
    let client: SysexClient | null = new SysexClient(this.transport(), FAST);
    return {
      current: () => client,
      waitFor: (role) =>
        new Promise((resolve) => {
          const check = () => {
            const wantsBoot = role === PortRole.Boot;
            if ((this.role === 'boot') === wantsBoot) {
              client?.close();
              client = new SysexClient(this.transport(), FAST);
              resolve(client);
              return true;
            }
            return false;
          };
          if (check() === false) {
            this.#waiters.push(() => {
              check();
            });
          }
        }),
    };
  }

  #reply(cmd: number, seq: number, payload: number[]): void {
    const bytes = encodeFrame(cmd | 0x40, seq, payload);
    this.#last = { cmd, seq, reply: bytes };
    const dropAt = this.dropReplies.indexOf(cmd);
    if (dropAt >= 0) {
      this.dropReplies.splice(dropAt, 1);
      return;
    }
    for (const listener of this.#listeners) {
      listener(bytes);
    }
  }

  #reboot(mode: number): void {
    setTimeout(() => {
      this.#generation += 1;
      this.#listeners.clear();
      this.#last = null;
      this.role = mode === 1 ? 'boot' : 'app';
      const waiters = this.#waiters.splice(0);
      for (const waiter of waiters) {
        waiter();
      }
    }, 5);
  }

  #handle(bytes: Uint8Array): void {
    const result = decodeFrame(bytes);
    if (result.ok === false) {
      return;
    }
    const { cmd, seq, payload } = result.frame;
    this.messages.push(cmd);
    if (this.#last && this.#last.cmd === cmd && this.#last.seq === seq) {
      for (const listener of this.#listeners) {
        listener(this.#last.reply);
      }
      return;
    }
    const isBoot = this.role === 'boot';
    const reader = new PayloadReader(payload);
    switch (cmd) {
      case Command.Hello:
        this.#reply(cmd, seq, [
          Status.Ok,
          isBoot ? 1 : 0,
          ...encodeU14(1),
          ...encodeU32(this.fwVersion),
          1,
          ...encodeU14(isBoot ? 256 : 0),
          ...encodeU14(isBoot ? 1 : 0b1101),
          ...pack(new Array(12).fill(0xab)),
        ]);
        return;
      case Command.Reboot:
        this.#reply(cmd, seq, [Status.Ok]);
        this.#reboot(reader.byte());
        return;
      case Command.FwBegin: {
        if (isBoot === false) {
          this.#reply(cmd, seq, [Status.BadState]);
          return;
        }
        const size = reader.u32();
        const crc = reader.u32();
        const board = reader.u14();
        const fwVersion = reader.u32();
        if (board !== 1) {
          this.#reply(cmd, seq, [Status.BadBoard]);
          return;
        }
        this.flash.fill(0xff, 0, PAGE_BYTES);
        this.#upload = { size, crc, fwVersion, page0Started: false };
        this.#reply(cmd, seq, [Status.Ok, ...encodeU14(256)]);
        return;
      }
      case Command.FwData: {
        const upload = this.#upload;
        if (upload === null) {
          this.#reply(cmd, seq, [Status.BadState]);
          return;
        }
        const offset = reader.u28();
        const data = unpack(payload.slice(4));
        const crossesPage = Math.floor(offset / PAGE_BYTES) !== Math.floor((offset + data.length - 1) / PAGE_BYTES);
        const outOfOrder = upload.page0Started && offset >= PAGE_BYTES;
        const bad = data.length % 4 !== 0 || crossesPage || outOfOrder || offset + data.length > upload.size;
        if (bad) {
          this.#reply(cmd, seq, [Status.BadAddr]);
          return;
        }
        if (offset < PAGE_BYTES) {
          upload.page0Started = true;
        }
        if (offset % PAGE_BYTES === 0 && offset !== 0) {
          this.flash.fill(0xff, offset, offset + PAGE_BYTES);
        }
        this.flash.set(data, offset);
        this.programmed.set(offset, (this.programmed.get(offset) ?? 0) + 1);
        this.#reply(cmd, seq, [Status.Ok, ...encodeU28(offset)]);
        return;
      }
      case Command.FwEnd: {
        const upload = this.#upload;
        if (upload === null) {
          this.#reply(cmd, seq, [Status.BadState]);
          return;
        }
        const status =
          this.endStatus ?? (imageCrc(this.flash, upload.size) === upload.crc ? Status.Ok : Status.BadImage);
        if (status === Status.Ok) {
          this.fwVersion = upload.fwVersion;
        }
        this.#reply(cmd, seq, [status]);
        return;
      }
      case Command.FwAbort:
        this.#upload = null;
        this.#reply(cmd, seq, [Status.Ok]);
        return;
      default:
        this.#reply(cmd, seq, [Status.BadCmd]);
    }
  }
}

/**
 * @param fwVersion - The version to put in the image.
 * @returns A checked image.
 */
function checkedImage(fwVersion = 0x0102_0003) {
  const result = validateImage(makeTestImage({ size: 2048, fwVersion }));
  if (result.ok === false) {
    throw new Error(result.problems.join());
  }
  return result.image;
}

describe('SysexClient', () => {
  it('resends a lost request with the same seq', async () => {
    const sent: number[][] = [];
    let drops = 2;
    const listeners = new Set<(b: Uint8Array) => void>();
    const transport: Transport = {
      send: (bytes) => {
        sent.push(Array.from(bytes));
        if (drops > 0) {
          drops -= 1;
          return;
        }
        const seq = bytes[6] ?? 0;
        queueMicrotask(() => {
          for (const l of listeners) {
            l(encodeFrame(0x41, seq, [Status.Ok]));
          }
        });
      },
      subscribe: (l) => {
        listeners.add(l);
        return () => listeners.delete(l);
      },
    };
    const client = new SysexClient(transport, FAST);
    await expect(client.request(Command.Hello)).resolves.toEqual([]);
    expect(sent.length).toBe(3);
    expect(new Set(sent.map((f) => f[6])).size).toBe(1);
    await client.request(Command.Hello);
    expect(sent[3]?.[6]).toBe(1);
  });

  it('routes 0x60–0x7F as LOG/EVENT even when its seq matches the pending request', async () => {
    const listeners = new Set<(b: Uint8Array) => void>();
    const emit = (bytes: Uint8Array) => {
      for (const l of listeners) {
        l(bytes);
      }
    };
    const transport: Transport = {
      send: (bytes) => {
        const seq = bytes[6] ?? 0;
        queueMicrotask(() => {
          emit(encodeFrame(0x60, seq, [2, ...encodeU28(7), 0x68, 0x69]));
          emit(encodeFrame(Command.FwBegin | 0x40, seq, [Status.Ok, ...encodeU14(256)]));
        });
      },
      subscribe: (l) => {
        listeners.add(l);
        return () => listeners.delete(l);
      },
    };
    const client = new SysexClient(transport, FAST);
    const logs: string[] = [];
    client.onMessage((m) => {
      if (m.kind === 'log') {
        logs.push(m.text);
      }
    });
    await expect(client.request(Command.FwBegin, [])).resolves.toEqual(encodeU14(256));
    expect(logs).toEqual(['hi']);
  });

  it('waits the long timeout for FW_BEGIN and FW_END only', async () => {
    const sent: number[] = [];
    const listeners = new Set<(b: Uint8Array) => void>();
    const transport: Transport = {
      send: (bytes) => {
        const cmd = bytes[5] ?? 0;
        const seq = bytes[6] ?? 0;
        sent.push(cmd);
        setTimeout(() => {
          for (const l of listeners) {
            l(encodeFrame(cmd | 0x40, seq, [Status.Ok]));
          }
        }, 30);
      },
      subscribe: (l) => {
        listeners.add(l);
        return () => listeners.delete(l);
      },
    };
    const client = new SysexClient(transport, { ...FAST, maxRetries: 0 });
    await expect(client.request(Command.FwEnd)).resolves.toEqual([]);
    await expect(client.request(Command.FwBegin)).resolves.toEqual([]);
    expect(sent).toEqual([Command.FwEnd, Command.FwBegin]);
    const error = await client.request(Command.Hello).catch((e: unknown) => e);
    expect((error as RequestError).failure).toBe(RequestFailure.Timeout);
  });

  it('resends on BUSY with the same seq', async () => {
    const seqs: number[] = [];
    const listeners = new Set<(b: Uint8Array) => void>();
    const transport: Transport = {
      send: (bytes) => {
        const seq = bytes[6] ?? 0;
        seqs.push(seq);
        const status = seqs.length < 3 ? Status.Busy : Status.Ok;
        queueMicrotask(() => {
          for (const l of listeners) {
            l(encodeFrame(Command.FwEnd | 0x40, seq, [status]));
          }
        });
      },
      subscribe: (l) => {
        listeners.add(l);
        return () => listeners.delete(l);
      },
    };
    await expect(new SysexClient(transport, FAST).request(Command.FwEnd)).resolves.toEqual([]);
    expect(seqs).toEqual([0, 0, 0]);
  });

  it('gives up after the retries and names the status codes', async () => {
    const silent: Transport = { send: () => undefined, subscribe: () => () => undefined };
    const error = await new SysexClient(silent, FAST).request(Command.Hello).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(RequestError);
    expect((error as RequestError).failure).toBe(RequestFailure.Timeout);

    const device = new FakeDevice();
    const failure = await device
      .link()
      .current()
      ?.request(Command.FwBegin, [])
      .catch((e: unknown) => e);
    expect((failure as RequestError).message).toMatch(/BAD_STATE/);
  });
});

describe('flashFirmware', () => {
  it('reboots to the bootloader, uploads in order and starts the new app', async () => {
    const device = new FakeDevice();
    const image = checkedImage();
    const phases: string[] = [];
    let lastSent = 0;
    const info = await flashFirmware({
      image,
      link: device.link(),
      reboot: { reappearTimeoutMs: 500 },
      onProgress: (p) => {
        if (phases.at(-1) !== p.phase) {
          phases.push(p.phase);
        }
        lastSent = p.sentBytes;
      },
    });
    expect(info.role).toBe(0);
    expect(info.fwVersion).toBe(0x0102_0003);
    expect(Array.from(device.flash.subarray(0, image.bytes.length))).toEqual(Array.from(image.bytes));
    expect(phases).toEqual([
      FlashPhase.Checking,
      FlashPhase.EnteringBootloader,
      FlashPhase.Beginning,
      FlashPhase.Writing,
      FlashPhase.Verifying,
      FlashPhase.StartingApp,
      FlashPhase.Done,
    ]);
    expect(lastSent).toBe(2048);
  });

  it('survives a lost FW_DATA reply without programming the chunk twice', async () => {
    const device = new FakeDevice();
    device.dropReplies = [Command.FwData];
    await flashFirmware({ image: checkedImage(), link: device.link() });
    expect([...device.programmed.values()].every((n) => n === 1)).toBe(true);
  });

  it('reports BAD_IMAGE from FW_END and aborts', async () => {
    const device = new FakeDevice();
    device.endStatus = Status.BadImage;
    const error = await flashFirmware({ image: checkedImage(), link: device.link() }).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(FlashError);
    expect((error as FlashError).phase).toBe(FlashPhase.Verifying);
    expect((error as FlashError).message).toMatch(/BAD_IMAGE/);
    expect(device.messages.at(-1)).toBe(Command.FwAbort);
  });
});

import { existsSync, readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import type { Transport } from '../protocol/client.ts';
import {
  APP_BASE,
  appAreaWithChecksum,
  buildUpdateStream,
  CHECK_PAGE,
  IMAGE_CHECKSUM_ADDRESS,
  MAX_APP_BYTES,
  negatedSum16,
  type ProbePort,
  parseStockUpdateFile,
  parseUpdateStream,
  recordChecksum,
  recordMessages,
  START_MESSAGE,
  type VendorProgress,
  vendorUpdate,
} from './maudio.ts';

/** The stock files are vendor-derived and gitignored: present only on a machine that has them. */
const FIRMWARE_DIR = new URL('../../../../research/devices/m-audio-code61/firmware/', import.meta.url);
/** Code 49 v1.2.1 and Code 61 v1.2.2, with each one's whole-image checksum. */
const STOCK_CASES = [
  { name: 'Code 49 v1.2.1', file: 'FirmwareFile_v1.2.1.hex.txt', app: 'app_v1.2.1_0x08002400.bin', checksum: 0x2cf0 },
  {
    name: 'Code 61 v1.2.2',
    file: 'FirmwareFile_code61_v1.2.2.hex.txt',
    app: 'app_code61_v1.2.2_0x08002400.bin',
    checksum: 0xd4c1,
  },
].map((c) => ({ ...c, file: new URL(c.file, FIRMWARE_DIR), app: new URL(c.app, FIRMWARE_DIR) }));

/** @returns The item, or a thrown error when it is missing. */
function must<T>(value: T | undefined): T {
  if (value === undefined) {
    throw new Error('missing');
  }
  return value;
}

/** A patterned app image of `size` bytes. */
function makeApp(size: number): Uint8Array {
  return Uint8Array.from({ length: size }, (_, i) => (i * 37 + 11) & 0xff);
}

/** Concatenates records. */
function join(records: readonly Uint8Array[]): Uint8Array {
  const out = new Uint8Array(records.reduce((n, r) => n + r.length, 0));
  let at = 0;
  for (const r of records) {
    out.set(r, at);
    at += r.length;
  }
  return out;
}

describe('checksums', () => {
  it('negates the 16-bit sum', () => {
    expect(negatedSum16(Uint8Array.of())).toBe(0);
    expect(negatedSum16(Uint8Array.of(1))).toBe(0xffff);
    expect(negatedSum16(Uint8Array.of(0xff, 0xff, 0x02))).toBe(-0x200 & 0xffff);
  });

  it('checks a record over everything after the length, before the checksum', () => {
    // The stock erase-only record for 0x08018800.
    const record = Uint8Array.from([
      0x00, 0x10, 0x6a, 0x04, 0xda, 0x3b, 0x09, 0x01, 0x88, 0, 0, 0, 0, 0, 0, 0, 0xeb, 0xfd,
    ]);
    expect(recordChecksum(record)).toBe(0xfdeb);
  });

  it('makes the app area sum to zero with the image checksum', () => {
    const { area, checksum } = appAreaWithChecksum(makeApp(5000));
    const end = IMAGE_CHECKSUM_ADDRESS - APP_BASE;
    expect(area.length).toBe(CHECK_PAGE + 0x400 - APP_BASE);
    expect(must(area[end]) | (must(area[end + 1]) << 8)).toBe(checksum);
    expect((negatedSum16(area.subarray(0, end)) + 0) & 0xffff).toBe(checksum);
    expect(area[5000]).toBe(0xff);
  });

  it('refuses an image past the checksum', () => {
    expect(() => appAreaWithChecksum(makeApp(MAX_APP_BYTES + 1))).toThrow(RangeError);
  });
});

describe('update stream', () => {
  const app = makeApp(3 * 1024 + 100);
  const records = buildUpdateStream(app);

  it('has the stock layout: data pages, erase-only pages, then the checksum page', () => {
    expect(records.length).toBe((CHECK_PAGE - APP_BASE) / 0x400 + 1);
    expect(records.slice(0, 4).map((r) => r.length)).toEqual([0x41e, 0x41e, 0x41e, 0x41e]);
    expect(must(records[4]).length).toBe(0x12);
    expect(must(records.at(-2)).length).toBe(0x12);
    expect(must(records.at(-1)).length).toBe(0x42a);
  });

  it('lays out a data record byte for byte', () => {
    const second = must(records[1]);
    expect([...second.subarray(0, 18)]).toEqual([
      0x04, 0x1c, 0x6a, 0x04, 0xda, 0x3b, 0x09, 0x00, 0x28, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x04, 0x00,
    ]);
    expect(second.subarray(18, 18 + 1024)).toEqual(app.subarray(1024, 2048));
    expect([...second.subarray(1042, 1052)]).toEqual([0x0b, 0x00, 0x28, 0x00, 0x04, 0x00, 0, 0, 0, 0]);
    // The last data page is padded with 0xFF.
    expect(must(records[3])[18 + 100]).toBe(0xff);
  });

  it('ends with write, verify and finish on the checksum page', () => {
    const last = must(records.at(-1));
    expect([...last.subarray(1042, 1064)]).toEqual([
      0x0b, 0x01, 0xb0, 0x00, 0x04, 0x00, 0x0d, 0x01, 0xb0, 0x00, 0x00, 0x00, 0x0e, 0x01, 0xb0, 0x00, 0x00, 0x00, 0, 0,
      0, 0,
    ]);
    const { checksum } = appAreaWithChecksum(app);
    expect(must(last[18 + 1022]) | (must(last[18 + 1023]) << 8)).toBe(checksum);
  });

  it('parses back with every checksum good', () => {
    const parsed = parseUpdateStream(join(records));
    expect(parsed.ok).toBe(true);
    const bad = join(records);
    bad[100] ^= 1;
    expect(parseUpdateStream(bad)).toMatchObject({ ok: false });
  });

  it('reads the stock hex-text format', () => {
    const text = [...join(records)].map((b) => b.toString(16).toUpperCase().padStart(2, '0')).join('');
    const parsed = parseStockUpdateFile(new TextEncoder().encode(`${text}\n`));
    expect(parsed).toMatchObject({ ok: true });
    expect(parsed?.ok === true ? parsed.records.length : 0).toBe(records.length);
    expect(parseStockUpdateFile(new TextEncoder().encode('not hex'))).toBeNull();
  });

  it('chunks a record into 11-byte ASCII-hex SysEx messages', () => {
    const record = must(records[4]);
    const messages = recordMessages(record);
    expect(messages.length).toBe(2);
    expect(new TextDecoder().decode(must(messages[0]).subarray(1, -1))).toBe('00106A04DA3B0900340000');
    expect(must(messages[0])[0]).toBe(0xf0);
    expect(must(messages[0]).at(-1)).toBe(0xf7);
    expect(must(messages[1]).length).toBe(7 * 2 + 2);
    expect(recordMessages(must(records[0])).length).toBe(Math.ceil(0x41e / 11));
  });

  for (const c of STOCK_CASES) {
    const present = existsSync(c.file) && existsSync(c.app);
    it.skipIf(present === false)(`rebuilds the stock ${c.name} file byte for byte from its app`, () => {
      const stock = parseStockUpdateFile(readFileSync(c.file));
      expect(stock?.ok).toBe(true);
      const rebuilt = buildUpdateStream(new Uint8Array(readFileSync(c.app)));
      expect(join(rebuilt)).toEqual(join(stock?.ok === true ? stock.records : []));
      expect(appAreaWithChecksum(new Uint8Array(readFileSync(c.app))).checksum).toBe(c.checksum);
    });
  }
});

/** A fake MIDI port whose device answers what it receives. */
class FakePort implements Transport {
  readonly #listeners = new Set<(bytes: Uint8Array) => void>();
  readonly received: Uint8Array[] = [];
  answer: (message: Uint8Array) => number | null = () => null;

  send(bytes: Uint8Array): void {
    this.received.push(bytes);
    const code = this.answer(bytes);
    if (code !== null) {
      queueMicrotask(() => {
        for (const listener of this.#listeners) {
          listener(Uint8Array.of(0xf0, code, 0xf7));
        }
      });
    }
  }

  subscribe(listener: (bytes: Uint8Array) => void): () => void {
    this.#listeners.add(listener);
    return () => this.#listeners.delete(listener);
  }
}

/** @returns Whether the message is the start message. */
const isStart = (message: Uint8Array) => message.length === START_MESSAGE.length && message[1] === START_MESSAGE[1];

/** Reassembles the records a fake bootloader received. */
function decodeRecords(messages: readonly Uint8Array[]): Uint8Array {
  const hex = messages
    .filter((m) => isStart(m) === false)
    .map((m) => new TextDecoder().decode(m.subarray(1, -1)))
    .join('');
  return Uint8Array.from(hex.match(/../g) ?? [], (pair) => Number.parseInt(pair, 16));
}

const FAST = {
  probeIntervalMs: 5,
  probeReplyMs: 20,
  portPollMs: 2,
  bootloaderTimeoutMs: 2000,
  ackTimeoutMs: 100,
  messageGapMs: 0,
} as const;

describe('vendor upload', () => {
  const records = buildUpdateStream(makeApp(100)).slice(-3);

  it('waits for the power cycle, then sends every record with an ack each', async () => {
    const keyboard = new FakePort();
    keyboard.answer = (m) => (isStart(m) ? 0x15 : null); // the stock app: request set, power-cycle
    const other = new FakePort();
    let ports: ProbePort[] = [
      { id: 'Code 61', name: 'Code 61', selected: true, transport: keyboard },
      { id: 'Synth', name: 'Synth', selected: false, transport: other },
    ];
    const phases: VendorProgress['phase'][] = [];
    const bootloader = new FakePort();
    let isRecordEnd = 0;
    bootloader.answer = (m) => {
      if (isStart(m)) {
        return 0x45;
      }
      // Ack after the last chunk of each record: a chunk shorter than 11 bytes ends one here.
      isRecordEnd++;
      return m.length < 24 ? 0x45 : null;
    };
    const done = vendorUpdate({
      records,
      ports: () => ports,
      settings: FAST,
      onProgress: (p) => {
        if (phases.at(-1) !== p.phase) {
          phases.push(p.phase);
        }
        if (p.phase === 'power-cycle' && ports.length === 2) {
          // The user turns the keyboard off; it comes back as the bootloader, unselected.
          ports = [must(ports[1])];
          setTimeout(() => {
            ports = [...ports, { id: 'Code 61', name: 'Code 61', selected: false, transport: bootloader }];
          }, 10);
        }
      },
    });
    await expect(done).resolves.toBe('Code 61');
    expect(phases).toEqual(['waiting-for-bootloader', 'power-cycle', 'writing', 'done']);
    expect(other.received.length).toBe(0);
    expect(keyboard.received.length).toBe(1);
    expect(bootloader.received.filter(isStart).length).toBe(2);
    expect(decodeRecords(bootloader.received)).toEqual(join(records));
    expect(isRecordEnd).toBeGreaterThan(0);
  });

  it('fails when a record gets no ack', async () => {
    const bootloader = new FakePort();
    bootloader.answer = (m) => (isStart(m) ? 0x45 : null);
    const ports = [{ id: 'boot', name: 'boot', selected: true, transport: bootloader }];
    await expect(vendorUpdate({ records, ports: () => ports, settings: FAST })).rejects.toThrow(
      /No answer to record 1/,
    );
  });

  it('sends a NAKed record once more, and logs stray replies', async () => {
    const bootloader = new FakePort();
    let naks = 1;
    bootloader.answer = (m) => {
      if (isStart(m)) {
        return 0x45;
      }
      if (m.length >= 24) {
        return null;
      }
      if (naks > 0) {
        naks--;
        return 0x15;
      }
      return 0x1a;
    };
    const log: string[] = [];
    const ports = [{ id: 'boot', name: 'boot', selected: true, transport: bootloader }];
    await vendorUpdate({ records, ports: () => ports, settings: FAST, onLog: (line) => log.push(line) });
    expect(log.some((line) => line.includes('sending it again'))).toBe(true);
  });
});

/**
 * M-Audio's stock update protocol, as the Code 49/61 bootloader speaks it
 * (`research/devices/m-audio-code61/analysis/update-protocol.md`, `custom-firmware-path.md`).
 * The Code 61 keeps its stock bootloader; a gemifi app goes on through it.
 *
 * - The update stream is a list of records. Each record is a big-endian length (record size - 2),
 *   the signature `6A 04 DA 3B`, 6-byte commands (op, page u16 BE in 256-byte units from
 *   0x08000000, count u16 BE in 256-byte units, 0), four zero bytes and a 16-bit checksum
 *   `(-sum(record[2:-2])) & 0xFFFF`, little-endian.
 * - {@link buildUpdateStream} lays an app image out the way the stock file does: a data record per
 *   1 KB page the image covers, an erase-only record for every other page up to 0x0801AFFF, then
 *   the final record for 0x0801B000, whose last halfword is the whole-image checksum.
 * - On the wire each record goes as SysEx messages of up to 11 record bytes in uppercase ASCII
 *   hex; the device answers `F0 cc F7` after the last one ({@link VendorReply}).
 * - {@link vendorUpdate} probes for the bootloader, then sends the stream with an ack per record.
 *   In update mode the keyboard shows "update" and its port is named "Code updater"
 *   ({@link isMaudioBootloaderName}).
 *
 * Framework-free, no DOM types: any {@link Transport} will do.
 */

import type { Transport } from '../protocol/client.ts';
import { toHex } from '../protocol/encoding.ts';
import { HEX_RADIX, SYSEX_END, SYSEX_START } from '../protocol/wire.ts';

/**
 * @param text - Hex digits, two per byte.
 * @returns The bytes.
 */
function hexBytes(text: string): number[] {
  return Array.from(text.match(/../g) ?? [], (pair) => Number.parseInt(pair, HEX_RADIX));
}
const MS_PER_S = 1000;

/** Flash starts here; record pages count from it. */
export const FLASH_BASE = 0x0800_0000;
/** The app area starts after M-Audio's 9 KB bootloader. */
export const APP_BASE = 0x0800_2400;
/** The 1 KB page holding the whole-image checksum, written last. */
export const CHECK_PAGE = 0x0801_b000;
/** Where the whole-image checksum is stored (u16 LE): the checksum page's last halfword. */
export const IMAGE_CHECKSUM_ADDRESS = 0x0801_b3fe;
/** Flash page size on this chip. */
export const PAGE_BYTES = 0x400;
/** The largest app: everything up to the image checksum. */
export const MAX_APP_BYTES = IMAGE_CHECKSUM_ADDRESS - APP_BASE;

/** Every record starts with this after its length. */
export const RECORD_SIGNATURE: readonly number[] = Object.freeze(hexBytes('6A04DA3B'));
/** The start-update message, also used as the probe. No manufacturer id. */
export const START_MESSAGE: readonly number[] = Object.freeze(hexBytes('F05A576E283C4E45F7'));
/** Record bytes per SysEx message. */
export const CHUNK_BYTES = 11;

/** The commands inside a record (inferred meanings). */
export const RecordOp = {
  /** Erase the page. */
  Erase: 0x09,
  /** Load `count` × 256 bytes, which follow the command, into the buffer. */
  Load: 0x0a,
  /** Program the buffer at the page. */
  Program: 0x0b,
  /** Probably verify; only in the final record. */
  Verify: 0x0d,
  /** Probably finish (clear the update request, reset); only in the final record. */
  Finish: 0x0e,
} as const;

/** The middle byte of a device's `F0 cc F7` reply. */
export const VendorReply = {
  /** In update mode. */
  UpdateMode: 0x20,
  /** OK / next (the bootloader's answer to the probe and to each record). */
  Ok: 0x45,
  /** Also OK / next; how it differs from 0x45 is not known. */
  OkAlt: 0x1a,
  /**
   * NAK. The stock app also answers the start message with it, after writing the update request
   * flag: the keyboard enters update mode at the next power-on.
   */
  Nak: 0x15,
} as const;

const BYTE_MASK = 0xff;
const U16_MASK = 0xffff;
const BITS_PER_BYTE = 8;
/** Record pages are in units of 256 bytes. */
const PAGE_UNIT_SHIFT = 8;
/** 1 KB in 256-byte units, the `count` of a load or program command. */
const PAGE_UNITS = PAGE_BYTES >> PAGE_UNIT_SHIFT;
/** Bytes before the commands: length and signature. */
const RECORD_PREFIX_BYTES = 6;
/** Zero bytes between the commands and the checksum. */
const RECORD_PAD_BYTES = 4;
/** The checksum. */
const CHECKSUM_BYTES = 2;
/** Bytes of a record that are not commands (length, signature, pad, checksum). */
const RECORD_OVERHEAD = RECORD_PREFIX_BYTES + RECORD_PAD_BYTES + CHECKSUM_BYTES;

/** One command in a record. */
interface Command {
  op: number;
  address: number;
  /** In 256-byte units. */
  count: number;
  /** The bytes a Load carries. */
  data?: Uint8Array;
}

/**
 * The negated 16-bit sum, as both the records and the image checksum use it.
 *
 * @param bytes - The bytes to sum.
 * @returns `(-sum(bytes)) & 0xFFFF`.
 */
export function negatedSum16(bytes: Uint8Array): number {
  let sum = 0;
  for (const byte of bytes) {
    sum = (sum + byte) & U16_MASK;
  }
  return -sum & U16_MASK;
}

/**
 * @param record - A whole record.
 * @returns The checksum it should carry: over everything after the length, before the checksum.
 */
export function recordChecksum(record: Uint8Array): number {
  return negatedSum16(record.subarray(2, record.length - CHECKSUM_BYTES));
}

/**
 * Builds a record from its commands, with its length and checksum.
 *
 * @param commands - The commands in order.
 * @returns The record bytes.
 */
function makeRecord(commands: readonly Command[]): Uint8Array {
  const commandBytes = 6;
  const body = commands.reduce((n, command) => n + commandBytes + (command.data?.length ?? 0), 0);
  const record = new Uint8Array(RECORD_OVERHEAD + body);
  const length = record.length - 2;
  record[0] = length >> BITS_PER_BYTE;
  record[1] = length & BYTE_MASK;
  record.set(RECORD_SIGNATURE, 2);
  let at = RECORD_PREFIX_BYTES;
  for (const command of commands) {
    const page = (command.address - FLASH_BASE) >> PAGE_UNIT_SHIFT;
    record.set(
      [
        command.op,
        page >> BITS_PER_BYTE,
        page & BYTE_MASK,
        command.count >> BITS_PER_BYTE,
        command.count & BYTE_MASK,
        0,
      ],
      at,
    );
    at += commandBytes;
    if (command.data !== undefined) {
      record.set(command.data, at);
      at += command.data.length;
    }
  }
  const checksum = recordChecksum(record);
  record[record.length - 2] = checksum & BYTE_MASK;
  record[record.length - 1] = checksum >> BITS_PER_BYTE;
  return record;
}

/**
 * The flash from {@link APP_BASE} to the end of the checksum page as the stream leaves it:
 * the app, then 0xFF, with the image checksum in the last halfword.
 *
 * @param app - The app image, linked at {@link APP_BASE}.
 * @returns The bytes, and the checksum on its own.
 */
export function appAreaWithChecksum(app: Uint8Array): { area: Uint8Array; checksum: number } {
  if (app.length > MAX_APP_BYTES) {
    throw new RangeError(`The image is ${app.length} bytes; the Code 61 app area holds ${MAX_APP_BYTES}.`);
  }
  const area = new Uint8Array(CHECK_PAGE + PAGE_BYTES - APP_BASE).fill(BYTE_MASK);
  area.set(app);
  const checkAt = IMAGE_CHECKSUM_ADDRESS - APP_BASE;
  const checksum = negatedSum16(area.subarray(0, checkAt));
  area[checkAt] = checksum & BYTE_MASK;
  area[checkAt + 1] = checksum >> BITS_PER_BYTE;
  return { area, checksum };
}

/**
 * Lays an app image out as an update stream, the way the stock file does.
 *
 * @param app - The app image, linked at {@link APP_BASE}, at most {@link MAX_APP_BYTES}.
 * @returns The records in wire order.
 */
export function buildUpdateStream(app: Uint8Array): Uint8Array[] {
  const { area } = appAreaWithChecksum(app);
  const records: Uint8Array[] = [];
  const pageData = (address: number) => area.subarray(address - APP_BASE, address - APP_BASE + PAGE_BYTES);
  for (let address = APP_BASE; address < CHECK_PAGE; address += PAGE_BYTES) {
    const hasData = address - APP_BASE < app.length;
    records.push(
      makeRecord(
        hasData
          ? [
              { op: RecordOp.Erase, address, count: 0 },
              { op: RecordOp.Load, address: FLASH_BASE, count: PAGE_UNITS, data: pageData(address) },
              { op: RecordOp.Program, address, count: PAGE_UNITS },
            ]
          : [{ op: RecordOp.Erase, address, count: 0 }],
      ),
    );
  }
  records.push(
    makeRecord([
      { op: RecordOp.Erase, address: CHECK_PAGE, count: 0 },
      { op: RecordOp.Load, address: FLASH_BASE, count: PAGE_UNITS, data: pageData(CHECK_PAGE) },
      { op: RecordOp.Program, address: CHECK_PAGE, count: PAGE_UNITS },
      { op: RecordOp.Verify, address: CHECK_PAGE, count: 0 },
      { op: RecordOp.Finish, address: CHECK_PAGE, count: 0 },
    ]),
  );
  return records;
}

/** What {@link parseUpdateStream} found. */
export type ParsedStream = { ok: true; records: Uint8Array[] } | { ok: false; problem: string };

/**
 * Splits a decoded update stream into records and checks each one's signature and checksum.
 *
 * @param blob - The stream's bytes.
 * @returns The records, or the first problem.
 */
export function parseUpdateStream(blob: Uint8Array): ParsedStream {
  const records: Uint8Array[] = [];
  let at = 0;
  while (at < blob.length) {
    if (at + RECORD_OVERHEAD > blob.length) {
      return { ok: false, problem: `The stream ends inside a record header at byte ${at}.` };
    }
    const size = (((blob[at] ?? 0) << BITS_PER_BYTE) | (blob[at + 1] ?? 0)) + 2;
    const record = blob.subarray(at, at + size);
    if (record.length !== size || size < RECORD_OVERHEAD) {
      return { ok: false, problem: `The record at byte ${at} is cut short or too small.` };
    }
    const isSigned = RECORD_SIGNATURE.every((byte, i) => record[2 + i] === byte);
    if (isSigned === false) {
      return { ok: false, problem: `The record at byte ${at} does not start with the M-Audio signature 6A 04 DA 3B.` };
    }
    const stored = (record[size - 2] ?? 0) | ((record[size - 1] ?? 0) << BITS_PER_BYTE);
    if (stored !== recordChecksum(record)) {
      return { ok: false, problem: `Record ${records.length + 1} (byte ${at}) has a bad checksum.` };
    }
    records.push(record);
    at += size;
  }
  if (records.length === 0) {
    return { ok: false, problem: 'The stream is empty.' };
  }
  return { ok: true, records };
}

/** Hex text: the stock `FirmwareFile` is the whole stream as one line of it. */
const HEX_TEXT = /^[0-9a-f\s]+$/i;

/**
 * Reads a stock M-Audio update file (`FirmwareFile…hex.txt`: the stream as hex text).
 *
 * @param bytes - The file's contents.
 * @returns The checked records, or why the file is not one; null when it is not hex text at all.
 */
export function parseStockUpdateFile(bytes: Uint8Array): ParsedStream | null {
  const text = new TextDecoder().decode(bytes);
  if (HEX_TEXT.test(text) === false) {
    return null;
  }
  const digits = text.replace(/\s+/g, '');
  if (digits.length % 2 !== 0) {
    return { ok: false, problem: 'The file has an odd number of hex digits.' };
  }
  const blob = new Uint8Array(digits.length / 2);
  for (let i = 0; i < blob.length; i++) {
    blob[i] = Number.parseInt(digits.slice(i * 2, i * 2 + 2), 16);
  }
  return parseUpdateStream(blob);
}

const UPPER_HEX = '0123456789ABCDEF';
const NIBBLE_BITS = 4;
const NIBBLE_MASK = 0x0f;

/**
 * The SysEx messages one record goes out as: up to 11 bytes each, as uppercase ASCII hex.
 *
 * @param record - The record.
 * @returns `F0 <hex> F7` messages in order.
 */
export function recordMessages(record: Uint8Array): Uint8Array[] {
  const messages: Uint8Array[] = [];
  for (let at = 0; at < record.length; at += CHUNK_BYTES) {
    const chunk = record.subarray(at, at + CHUNK_BYTES);
    const message = new Uint8Array(chunk.length * 2 + 2);
    message[0] = SYSEX_START;
    chunk.forEach((byte, i) => {
      message[1 + i * 2] = UPPER_HEX.charCodeAt(byte >> NIBBLE_BITS);
      message[2 + i * 2] = UPPER_HEX.charCodeAt(byte & NIBBLE_MASK);
    });
    message[message.length - 1] = SYSEX_END;
    messages.push(message);
  }
  return messages;
}

/**
 * @param message - A received SysEx message.
 * @returns The reply code of an `F0 cc F7`, or null for anything else.
 */
export function replyCode(message: Uint8Array): number | null {
  const replyLength = 3;
  const isReply = message.length === replyLength && message[0] === SYSEX_START && message[2] === SYSEX_END;
  return isReply ? (message[1] ?? null) : null;
}

/** The port name of M-Audio's bootloader: "Code updater" (the OS may add a suffix such as " MIDI 1"). */
const BOOTLOADER_NAME = /code updater/i;

/**
 * @param name - A MIDI port's name.
 * @returns Whether it looks like M-Audio's bootloader (the keyboard in update mode).
 */
export function isMaudioBootloaderName(name: string): boolean {
  return BOOTLOADER_NAME.test(name);
}

/** A MIDI port the uploader may probe. */
export interface ProbePort {
  /** Stable while it stays connected. */
  id: string;
  name: string;
  /** The port the user has selected: probed even when it was there before the wait. */
  selected: boolean;
  transport: Transport;
  /**
   * Closes the port, so it isn't open when the keyboard is switched off and it vanishes (Chrome's
   * Web MIDI on Windows can hang on an open port that disappears). Optional: tests leave it out.
   */
  release?: () => void;
}

/** Tunable timings. */
export interface VendorSettings {
  /** How often the probe is sent while waiting for the bootloader (the stock updater: 2 s). */
  probeIntervalMs: number;
  /** How long a probe waits for its reply. */
  probeReplyMs: number;
  /** How often the port list is re-read while waiting. */
  portPollMs: number;
  /** How long to wait for the bootloader in all, power cycle included. */
  bootloaderTimeoutMs: number;
  /** How long to wait for a record's ack (the stock updater: 10 s). */
  ackTimeoutMs: number;
  /** The pause between messages (the stock updater: 3 ms). */
  messageGapMs: number;
  /** How many times a record that was NAKed is sent again. */
  nakRetries: number;
}

export const VENDOR_DEFAULTS: Readonly<VendorSettings> = Object.freeze({
  probeIntervalMs: 2000,
  probeReplyMs: 1500,
  portPollMs: 250,
  bootloaderTimeoutMs: 300_000,
  ackTimeoutMs: 10_000,
  messageGapMs: 3,
  nakRetries: 1,
});

/** Where a vendor update is. */
export const VendorPhase = {
  /** Asking the gemifi app to set the update request (REBOOT(1)). */
  Requesting: 'requesting',
  /** Probing for the bootloader; the user may have to power-cycle the keyboard. */
  WaitingForBootloader: 'waiting-for-bootloader',
  /** An app accepted the update request; the keyboard needs turning off and on. */
  PowerCycle: 'power-cycle',
  /** Sending records. */
  Writing: 'writing',
  Done: 'done',
} as const;
export type VendorPhase = (typeof VendorPhase)[keyof typeof VendorPhase];

/** What the reader sees for each phase. */
export const VENDOR_PHASE_LABELS: Readonly<Record<VendorPhase, string>> = Object.freeze({
  requesting: 'Asking the keyboard for update mode',
  'waiting-for-bootloader': 'Looking for the M-Audio bootloader',
  'power-cycle': 'Turn the keyboard off and on',
  writing: 'Writing firmware',
  done: 'Done',
});

/** A progress report. */
export interface VendorProgress {
  phase: VendorPhase;
  sentBytes: number;
  totalBytes: number;
  /** The port being talked to, once known. */
  portName?: string;
}

/** What {@link vendorUpdate} needs. */
export interface VendorUpdateOptions {
  /** The records to send, in order. */
  records: readonly Uint8Array[];
  /** The MIDI ports connected now. */
  ports: () => readonly ProbePort[];
  /** Runs first, e.g. REBOOT(1) to a gemifi app. Errors in it are logged, not fatal. */
  request?: () => Promise<void>;
  onProgress?: (progress: VendorProgress) => void;
  /** Unexpected replies and other things worth a line in the log. */
  onLog?: (line: string) => void;
  signal?: AbortSignal;
  settings?: Partial<VendorSettings>;
  /**
   * Runs, and is awaited, right before the keyboard is expected to vanish from USB: before
   * `request`, and before the power-cycle phase is reported. The page stores the image here, so it
   * survives a browser that has to be closed. Errors in it are logged, not fatal.
   */
  beforeVanish?: () => Promise<void>;
  /**
   * End the run once the stock app has accepted the update request, instead of waiting for the
   * bootloader under the open page. Set on Windows, where Chrome can stop responding when a MIDI
   * device is switched off while a page has Web MIDI access.
   */
  stopForPowerCycle?: boolean;
}

/** How {@link vendorUpdate} ended without an error. */
export type VendorOutcome =
  /** The bootloader took every record. */
  | { kind: 'done'; portName: string }
  /** The app accepted the update request and {@link VendorUpdateOptions.stopForPowerCycle} is set. */
  | { kind: 'power-cycle'; portName: string };

/** Thrown when a vendor update stops. */
export class VendorError extends Error {
  override name = 'VendorError';
  readonly phase: VendorPhase;

  /**
   * @param phase - Where it stopped.
   * @param message - The reader's sentence.
   */
  constructor(phase: VendorPhase, message: string) {
    super(message);
    this.phase = phase;
  }
}

/**
 * @param ms - How long.
 * @param signal - Ends the wait early with an error.
 * @returns After `ms`.
 */
function sleep(ms: number, signal: AbortSignal | undefined): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted === true) {
      reject(new Error('Stopped.'));
      return;
    }
    const onAbort = () => {
      clearTimeout(timer);
      reject(new Error('Stopped.'));
    };
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

/**
 * @param code - A reply code.
 * @returns The reply as it was on the wire, `F0 cc F7`.
 */
function formatReply(code: number): string {
  return toHex([SYSEX_START, code, SYSEX_END]);
}

/** Reply codes collected from one transport, so an ack that comes early is not missed. */
class ReplyQueue {
  readonly #codes: number[] = [];
  readonly #unsubscribe: () => void;
  #wake: (() => void) | null = null;

  /**
   * @param transport - Where replies come from.
   * @param onOther - Called with any message that is not an `F0 cc F7`.
   */
  constructor(transport: Transport, onOther: (message: Uint8Array) => void) {
    this.#unsubscribe = transport.subscribe((message) => {
      const code = replyCode(message);
      if (code === null) {
        onOther(message);
        return;
      }
      this.#codes.push(code);
      this.#wake?.();
    });
  }

  /** Forgets replies received so far. */
  clear(): void {
    this.#codes.length = 0;
  }

  /**
   * @param timeoutMs - How long to wait.
   * @param signal - Stops waiting.
   * @returns The next reply code, or null on timeout.
   */
  async next(timeoutMs: number, signal: AbortSignal | undefined): Promise<number | null> {
    const deadline = Date.now() + timeoutMs;
    while (this.#codes.length === 0) {
      const left = deadline - Date.now();
      if (left <= 0) {
        return null;
      }
      await new Promise<void>((resolve, reject) => {
        const done = () => {
          clearTimeout(timer);
          signal?.removeEventListener('abort', onAbort);
          this.#wake = null;
        };
        const onAbort = () => {
          done();
          reject(new Error('Stopped.'));
        };
        const timer = setTimeout(() => {
          done();
          resolve();
        }, left);
        this.#wake = () => {
          done();
          resolve();
        };
        if (signal?.aborted === true) {
          onAbort();
        }
        signal?.addEventListener('abort', onAbort, { once: true });
      });
    }
    return this.#codes.shift() ?? null;
  }

  close(): void {
    this.#unsubscribe();
  }
}

/**
 * Probes ports until the bootloader answers: the selected port, and any port that appears (or
 * comes back) after the wait starts, so a power-cycled keyboard is found under whatever name its
 * bootloader has, and any port named like the bootloader ("Code updater"), so a keyboard that was
 * in update mode before the page loaded is found too. Other ports that were there already are left
 * alone.
 *
 * @param options - The update's options.
 * @param settings - Timings.
 * @param report - Progress.
 * @param beforeVanish - {@link VendorUpdateOptions.beforeVanish}, with its errors logged.
 * @returns The bootloader's port, or the app's port name when the run stops for the power cycle.
 */
async function findBootloader(
  options: VendorUpdateOptions,
  settings: VendorSettings,
  report: (phase: VendorPhase, portName?: string) => void,
  beforeVanish: () => Promise<void>,
): Promise<{ port: ProbePort } | { powerCycle: string }> {
  const { signal, onLog } = options;
  const deadline = Date.now() + settings.bootloaderTimeoutMs;
  /** Ports present when the wait started and not gone since: not probed unless selected. */
  const old = new Set(options.ports().map((port) => port.id));
  /**
   * Ports whose app answered F0 15 F7: it wrote the request to flash, so it is not asked again
   * (each ask is a page write) until it goes away with the power cycle.
   */
  const requested = new Set<string>();
  const lastProbe = new Map<string, number>();
  while (Date.now() < deadline) {
    const ports = options.ports();
    const present = new Set(ports.map((port) => port.id));
    for (const set of [old, requested]) {
      for (const id of set) {
        if (present.has(id) === false) {
          set.delete(id);
        }
      }
    }
    const now = Date.now();
    const due = ports.filter(
      (port) =>
        (port.selected || old.has(port.id) === false || isMaudioBootloaderName(port.name)) &&
        requested.has(port.id) === false &&
        now - (lastProbe.get(port.id) ?? -Infinity) >= settings.probeIntervalMs,
    );
    for (const port of due) {
      lastProbe.set(port.id, Date.now());
      const queue = new ReplyQueue(port.transport, () => undefined);
      try {
        port.transport.send(Uint8Array.from(START_MESSAGE));
        let code = await queue.next(settings.probeReplyMs, signal);
        while (code === VendorReply.UpdateMode) {
          onLog?.(`${port.name}: F0 20 F7 (in update mode)`);
          code = await queue.next(settings.probeReplyMs, signal);
        }
        if (code === VendorReply.Ok || code === VendorReply.OkAlt) {
          onLog?.(`${port.name}: the M-Audio bootloader is ready`);
          return { port };
        }
        if (code === VendorReply.Nak) {
          onLog?.(`${port.name}: F0 15 F7, the app set the update request; the keyboard needs a power cycle`);
          requested.add(port.id);
          queue.close();
          port.release?.(); // closed before the user switches the keyboard off
          await beforeVanish();
          if (options.stopForPowerCycle === true) {
            return { powerCycle: port.name };
          }
          report(VendorPhase.PowerCycle, port.name);
        } else if (code !== null) {
          onLog?.(`${port.name}: unexpected reply ${formatReply(code)} to the probe`);
        }
      } catch (error) {
        if (signal?.aborted === true) {
          throw error;
        }
        onLog?.(`${port.name}: probe failed: ${error instanceof Error ? error.message : String(error)}`);
      } finally {
        queue.close();
      }
    }
    await sleep(settings.portPollMs, signal);
  }
  throw new VendorError(
    VendorPhase.WaitingForBootloader,
    `The M-Audio bootloader did not answer within ${Math.round(settings.bootloaderTimeoutMs / MS_PER_S)} s. Turn the keyboard off and on, and try again.`,
  );
}

/**
 * Puts the keyboard into update mode and sends the stream: `request` (REBOOT(1) to a gemifi app),
 * then the probe every 2 s until the bootloader answers `F0 45 F7` (an app's `F0 15 F7` means
 * "power-cycle now"), then the start message and every record, waiting for an ack after each.
 * With {@link VendorUpdateOptions.stopForPowerCycle} it ends at that `F0 15 F7` instead.
 *
 * @param options - The records, the ports, callbacks and a signal.
 * @returns How it ended, with the port's name.
 */
export async function vendorUpdate(options: VendorUpdateOptions): Promise<VendorOutcome> {
  const settings = { ...VENDOR_DEFAULTS, ...options.settings };
  const { records, signal, onLog } = options;
  const totalBytes = records.reduce((n, record) => n + record.length, 0);
  let phase: VendorPhase = VendorPhase.Requesting;
  let portName: string | undefined;
  const report = (next: VendorPhase, name?: string, sentBytes = 0) => {
    phase = next;
    portName = name ?? portName;
    options.onProgress?.({ phase, sentBytes, totalBytes, ...(portName === undefined ? {} : { portName }) });
  };

  const beforeVanish = async () => {
    try {
      await options.beforeVanish?.();
    } catch (error) {
      onLog?.(`Saving the upload for later: ${error instanceof Error ? error.message : String(error)}`);
    }
  };

  if (options.request !== undefined) {
    report(VendorPhase.Requesting);
    await beforeVanish(); // REBOOT(1) restarts the keyboard into the bootloader: it vanishes
    try {
      await options.request();
    } catch (error) {
      if (signal?.aborted === true) {
        throw error;
      }
      onLog?.(`Update request: ${error instanceof Error ? error.message : String(error)}`);
    }
    // The app restarts into the bootloader: close its port before it vanishes. The bootloader
    // comes back as a new port, which the probe finds without a selection.
    for (const port of options.ports()) {
      if (port.selected) {
        port.release?.();
      }
    }
  }

  report(VendorPhase.WaitingForBootloader);
  const found = await findBootloader(options, settings, report, beforeVanish);
  if ('powerCycle' in found) {
    return { kind: 'power-cycle', portName: found.powerCycle };
  }
  const { port } = found;
  report(VendorPhase.Writing, port.name);

  const queue = new ReplyQueue(port.transport, (message) =>
    onLog?.(`${port.name}: unexpected message ${toHex(message)}`),
  );
  const fail = (message: string): never => {
    throw new VendorError(phase, message);
  };
  /** Waits for the ack of what was just sent. */
  const ack = async (what: string): Promise<'ok' | 'nak'> => {
    for (;;) {
      const code = await queue.next(settings.ackTimeoutMs, signal);
      if (code === null) {
        return fail(`No answer to ${what} within ${settings.ackTimeoutMs / MS_PER_S} s.`);
      }
      if (code === VendorReply.Ok || code === VendorReply.OkAlt) {
        return 'ok';
      }
      if (code === VendorReply.Nak) {
        return 'nak';
      }
      onLog?.(`${port.name}: ${formatReply(code)} while waiting for ${what}`);
    }
  };
  try {
    // The stock updater sends the start message once more before the records, and waits for its ack.
    queue.clear();
    port.transport.send(Uint8Array.from(START_MESSAGE));
    if ((await ack('the start message')) === 'nak') {
      fail('The bootloader refused the start message (F0 15 F7).');
    }
    let sentBytes = 0;
    for (const [index, record] of records.entries()) {
      const what = `record ${index + 1} of ${records.length}`;
      for (let attempt = 0; ; attempt++) {
        queue.clear();
        for (const message of recordMessages(record)) {
          port.transport.send(message);
          await sleep(settings.messageGapMs, signal);
        }
        if ((await ack(what)) === 'ok') {
          break;
        }
        if (attempt >= settings.nakRetries) {
          fail(`The bootloader refused ${what} (F0 15 F7).`);
        }
        onLog?.(`${port.name}: ${what} was refused (F0 15 F7); sending it again`);
      }
      sentBytes += record.length;
      report(VendorPhase.Writing, port.name, sentBytes);
    }
  } finally {
    queue.close();
  }
  report(VendorPhase.Done, port.name, totalBytes);
  return { kind: 'done', portName: port.name };
}

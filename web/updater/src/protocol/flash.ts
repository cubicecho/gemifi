/**
 * The firmware upload sequences (`protocol/sysex.md` §4.1–4.2, `research/core/bootloader-design.md` §9).
 *
 * - {@link flashFirmware}, a `.gfw` through the gemifi bootloader: reboot into the bootloader if
 *   needed, FW_BEGIN, FW_DATA in the spec's order (0x200 up, page 0 last), FW_END, reboot into the
 *   app and check it answers HELLO.
 * - {@link flashAbFirmware}, a chip image to an A/B device (ESP32 / ESP8266 OTA): the app takes
 *   FW_BEGIN, FW_DATA in plain order from 0 and FW_END itself, then REBOOT(0) starts the new slot.
 *   Never REBOOT(1).
 *
 * Framework-free: the caller supplies the device link.
 */

import { type AbImage, isAbDevice, planLinearChunks } from './ab-image.ts';
import { formatBoardId } from './boards.ts';
import { RequestError, RequestFailure, type RequestOptions, type SysexClient } from './client.ts';
import { REBOOT_DEFAULTS, type RebootSettings } from './defaults.ts';
import { type Chunk, type FirmwareImage, hex, planChunks } from './image.ts';
import {
  formatVersion,
  fwBeginPayload,
  fwDataPayload,
  type HelloInfo,
  parseFwBegin,
  parseFwData,
  parseHello,
  rebootPayload,
} from './messages.ts';
import { PortRole } from './ports.ts';
import { CHUNK_ALIGN_BYTES, Command, RebootMode, Role } from './wire.ts';

/** Where an upload is. */
export const FlashPhase = {
  Checking: 'checking',
  EnteringBootloader: 'entering-bootloader',
  Beginning: 'beginning',
  Writing: 'writing',
  Verifying: 'verifying',
  StartingApp: 'starting-app',
  Done: 'done',
} as const;
export type FlashPhase = (typeof FlashPhase)[keyof typeof FlashPhase];

/** What the reader sees for each phase. */
export const FLASH_PHASE_LABELS: Readonly<Record<FlashPhase, string>> = Object.freeze({
  checking: 'Asking the device who it is',
  'entering-bootloader': 'Rebooting into the bootloader',
  beginning: 'Erasing and starting the upload',
  writing: 'Writing firmware',
  verifying: 'Checking the written image',
  'starting-app': 'Starting the new firmware',
  done: 'Done',
});

/** A progress report. */
export interface FlashProgress {
  phase: FlashPhase;
  /** Bytes acknowledged so far. */
  sentBytes: number;
  /** Bytes to send. */
  totalBytes: number;
}

/** The device link the upload drives. The web page backs it with Web MIDI; a Node tool could too. */
export interface DeviceLink {
  /** @returns The client for the device connected now, or null. */
  current(): SysexClient | null;
  /**
   * Waits until a device whose port is in `role` is connected and settled.
   *
   * @param role - `app` or `boot`.
   * @param timeoutMs - How long to wait.
   * @param signal - Stops waiting.
   * @returns Its client.
   */
  waitFor(role: PortRole, timeoutMs: number, signal?: AbortSignal): Promise<SysexClient>;
}

/** Thrown when an upload stops, with the phase it stopped in. */
export class FlashError extends Error {
  override name = 'FlashError';
  readonly phase: FlashPhase;

  /**
   * @param phase - Where it stopped.
   * @param message - The reader's sentence.
   * @param cause - The underlying error.
   */
  constructor(phase: FlashPhase, message: string, cause?: unknown) {
    super(message, { cause });
    this.phase = phase;
  }
}

/** What {@link flashFirmware} needs. */
export interface FlashOptions {
  image: FirmwareImage;
  link: DeviceLink;
  /** Called as the upload moves. */
  onProgress?: (progress: FlashProgress) => void;
  /** Cancels the upload. An upload cancelled after FW_BEGIN leaves the device in the bootloader. */
  signal?: AbortSignal;
  /** Timing overrides. */
  reboot?: Partial<RebootSettings>;
}

/**
 * Asks a device who it is.
 *
 * @param client - The device.
 * @param options - An abort signal.
 * @returns Its HELLO reply.
 */
export async function hello(client: SysexClient, options: RequestOptions = {}): Promise<HelloInfo> {
  return parseHello(await client.request(Command.Hello, [], options));
}

/**
 * Sends REBOOT. A reply lost to the reset itself is not an error: the caller waits for the port.
 *
 * @param client - The device.
 * @param mode - Where to reboot to.
 * @param options - An abort signal.
 * @returns Whether the device acknowledged it.
 */
export async function reboot(client: SysexClient, mode: RebootMode, options: RequestOptions = {}): Promise<boolean> {
  try {
    await client.request(Command.Reboot, rebootPayload(mode), options);
    return true;
  } catch (error) {
    const isLostReply = error instanceof RequestError && error.failure === RequestFailure.Timeout;
    if (isLostReply) {
      return false;
    }
    throw error;
  }
}

/**
 * Uploads a checked image and starts it.
 *
 * @param options - The image, the link, a progress callback and a signal.
 * @returns HELLO from the new firmware.
 */
export async function flashFirmware(options: FlashOptions): Promise<HelloInfo> {
  const { image, link, signal } = options;
  const { reappearTimeoutMs } = { ...REBOOT_DEFAULTS, ...options.reboot };
  const { header } = image;
  const totalBytes = header.imageSize;
  const run = makeRun(totalBytes, options.onProgress);
  const { report, fail, stage } = run;

  report(FlashPhase.Checking);
  let client = link.current() ?? fail('No device is connected.');
  let info = await stage(() => hello(client, { signal }));
  if (info.boardId !== header.boardId) {
    fail(`The device is board ${formatBoardId(info.boardId)} but the image is for ${formatBoardId(header.boardId)}.`);
  }

  if (info.role === Role.App) {
    report(FlashPhase.EnteringBootloader);
    await stage(() => reboot(client, RebootMode.Boot, { signal }));
    client = await stage(() => link.waitFor(PortRole.Boot, reappearTimeoutMs, signal));
    info = await stage(() => hello(client, { signal }));
    if (info.role !== Role.Boot) {
      fail('The device came back, but not in the bootloader.');
    }
  }

  report(FlashPhase.Beginning);
  const begin = fwBeginPayload({
    size: header.imageSize,
    crc32: header.crc32,
    boardId: header.boardId,
    fwVersion: header.fwVersion,
  });
  const beginChunk = await stage(async () => parseFwBegin(await client.request(Command.FwBegin, begin, { signal })));
  // FW_BEGIN's max_chunk is authoritative (§4); HELLO's is only advisory.
  const maxChunk = beginChunk;
  const isChunkUsable = maxChunk > 0;
  if (isChunkUsable === false) {
    fail('FW_BEGIN reported a max_chunk of 0.');
  }

  const bootClient = client;
  await writeImage(bootClient, image.bytes, planChunks(header.imageSize, maxChunk), run, signal);

  report(FlashPhase.StartingApp, totalBytes);
  await stage(() => reboot(bootClient, RebootMode.App, { signal }));
  const appClient = await stage(() => link.waitFor(PortRole.App, reappearTimeoutMs, signal));
  const appInfo = await stage(() => hello(appClient, { signal }));
  if (appInfo.role !== Role.App) {
    fail('The device came back in the bootloader, so the new firmware did not start.');
  }
  if (appInfo.fwVersion !== header.fwVersion) {
    fail(`The device restarted with firmware ${hex(appInfo.fwVersion)}, not the uploaded ${hex(header.fwVersion)}.`);
  }
  report(FlashPhase.Done, totalBytes);
  return appInfo;
}

/** The phase bookkeeping both sequences share. */
interface Run {
  report: (next: FlashPhase, sentBytes?: number) => void;
  fail: (message: string, cause?: unknown) => never;
  /** Runs a step, turning any error into a {@link FlashError} in the current phase. */
  stage: <T>(work: () => Promise<T>) => Promise<T>;
}

/**
 * @param totalBytes - What the progress bar counts to.
 * @param onProgress - The caller's callback.
 * @returns The helpers.
 */
function makeRun(totalBytes: number, onProgress: ((progress: FlashProgress) => void) | undefined): Run {
  let phase: FlashPhase = FlashPhase.Checking;
  const report = (next: FlashPhase, sentBytes = 0) => {
    phase = next;
    onProgress?.({ phase: next, sentBytes, totalBytes });
  };
  const fail = (message: string, cause?: unknown): never => {
    throw new FlashError(phase, message, cause);
  };
  const stage = async <T>(work: () => Promise<T>): Promise<T> => {
    try {
      return await work();
    } catch (error) {
      if (error instanceof FlashError) {
        throw error;
      }
      return fail(error instanceof Error ? error.message : String(error), error);
    }
  };
  return { report, fail, stage };
}

/**
 * FW_DATA for every chunk, in the order given, then FW_END. Sends FW_ABORT if anything fails.
 *
 * @param client - The device taking the upload.
 * @param bytes - The image as sent.
 * @param chunks - The upload order.
 * @param run - Progress and errors.
 * @param signal - Cancels it.
 */
async function writeImage(
  client: SysexClient,
  bytes: Uint8Array,
  chunks: readonly Chunk[],
  run: Run,
  signal: AbortSignal | undefined,
): Promise<void> {
  const { report, fail, stage } = run;
  const totalBytes = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  try {
    let sentBytes = 0;
    report(FlashPhase.Writing, sentBytes);
    for (const chunk of chunks) {
      const data = bytes.subarray(chunk.offset, chunk.offset + chunk.length);
      const echo = await stage(async () =>
        parseFwData(await client.request(Command.FwData, fwDataPayload(chunk.offset, data), { signal })),
      );
      if (echo !== chunk.offset) {
        fail(`The device acknowledged offset ${hex(echo)} for a chunk sent to ${hex(chunk.offset)}.`);
      }
      sentBytes += chunk.length;
      report(FlashPhase.Writing, sentBytes);
    }
    report(FlashPhase.Verifying, totalBytes);
    await stage(() => client.request(Command.FwEnd, [], { signal }));
  } catch (error) {
    await client.request(Command.FwAbort).catch(() => undefined);
    throw error;
  }
}

/** What {@link flashAbFirmware} needs. */
export interface AbFlashOptions {
  image: AbImage;
  link: DeviceLink;
  /** Called as the upload moves. */
  onProgress?: (progress: FlashProgress) => void;
  /** Cancels the upload. The old slot stays bootable until FW_END has checked the new one. */
  signal?: AbortSignal;
  /** Timing overrides. */
  reboot?: Partial<RebootSettings>;
}

/**
 * Uploads a chip image to an A/B device and starts it (§4.1, §4.2 "A/B devices"): FW_BEGIN,
 * FW_DATA in plain order from 0, FW_END, REBOOT(0), then HELLO must report the new version.
 *
 * @param options - The prepared image, the link, a progress callback and a signal.
 * @returns HELLO from the new firmware.
 */
export async function flashAbFirmware(options: AbFlashOptions): Promise<HelloInfo> {
  const { image, link, signal } = options;
  const settings = { ...REBOOT_DEFAULTS, ...options.reboot };
  const run = makeRun(image.size, options.onProgress);
  const { report, fail, stage } = run;

  report(FlashPhase.Checking);
  const client = link.current() ?? fail('No device is connected.');
  const info = await stage(() => hello(client, { signal }));
  if (isAbDevice(info) === false) {
    fail('This device does not take uploads in the app. Flash it with a .gfw image, through its bootloader.');
  }
  if (info.boardId !== image.boardId) {
    fail(`The device is board ${formatBoardId(info.boardId)} but the image is for ${formatBoardId(image.boardId)}.`);
  }

  report(FlashPhase.Beginning);
  const begin = fwBeginPayload({
    size: image.size,
    crc32: image.crc32,
    boardId: image.boardId,
    fwVersion: image.fwVersion,
  });
  const maxChunk = await stage(async () => parseFwBegin(await client.request(Command.FwBegin, begin, { signal })));
  if (maxChunk < CHUNK_ALIGN_BYTES) {
    fail(`FW_BEGIN reported a max_chunk of ${maxChunk}.`);
  }
  await writeImage(client, image.bytes, planLinearChunks(image.size, maxChunk), run, signal);

  report(FlashPhase.StartingApp, image.size);
  await stage(() => reboot(client, RebootMode.App, { signal }));
  const appInfo = await stage(() => waitForApp(link, settings.abRestartTimeoutMs, settings.abPollMs, signal));
  if (appInfo.fwVersion !== image.fwVersion) {
    fail(
      `The device restarted with firmware ${formatVersion(appInfo.fwVersion)}, not ${formatVersion(image.fwVersion)}: the version typed was wrong, or the new slot did not boot and it went back to the old one.`,
    );
  }
  report(FlashPhase.Done, image.size);
  return appInfo;
}

/**
 * Waits for the app to answer HELLO after a reboot. Polls rather than waiting for a new port:
 * over USB the port goes and comes back with the same name, and over serial it never goes at all.
 *
 * @param link - The device link.
 * @param timeoutMs - How long to keep trying.
 * @param pollMs - The pause before each try.
 * @param signal - Stops waiting.
 * @returns The app's HELLO.
 */
async function waitForApp(
  link: DeviceLink,
  timeoutMs: number,
  pollMs: number,
  signal: AbortSignal | undefined,
): Promise<HelloInfo> {
  const deadline = Date.now() + timeoutMs;
  let lastError = '';
  while (Date.now() < deadline) {
    await sleep(pollMs, signal);
    const client = link.current();
    if (client === null) {
      continue;
    }
    try {
      const info = await hello(client, { signal });
      if (info.role === Role.App) {
        return info;
      }
      lastError = 'it answered as a bootloader';
    } catch (error) {
      if (signal?.aborted === true) {
        throw error;
      }
      lastError = error instanceof Error ? error.message : String(error);
    }
  }
  const why = lastError === '' ? '' : ` (${lastError})`;
  throw new Error(`The device did not answer HELLO within ${timeoutMs} ms of the reboot${why}.`);
}

/**
 * @param ms - How long.
 * @param signal - Ends the wait early with an error.
 * @returns After `ms`.
 */
function sleep(ms: number, signal: AbortSignal | undefined): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted === true) {
      reject(new RequestError(RequestFailure.Aborted, 'Stopped.'));
      return;
    }
    const onAbort = () => {
      clearTimeout(timer);
      reject(new RequestError(RequestFailure.Aborted, 'Stopped.'));
    };
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

/**
 * The firmware upload sequence (`protocol/sysex.md` §4.2, `research/core/bootloader-design.md` §9):
 * reboot into the bootloader if needed, FW_BEGIN, FW_DATA in the spec's order, FW_END, reboot
 * into the app and check it answers HELLO. Framework-free: the caller supplies the device link.
 */

import { formatBoardId } from './boards.ts';
import { RequestError, RequestFailure, type RequestOptions, type SysexClient } from './client.ts';
import { REBOOT_DEFAULTS, type RebootSettings } from './defaults.ts';
import { type FirmwareImage, hex, planChunks } from './image.ts';
import {
  fwBeginPayload,
  fwDataPayload,
  type HelloInfo,
  parseFwBegin,
  parseFwData,
  parseHello,
  rebootPayload,
} from './messages.ts';
import { PortRole } from './ports.ts';
import { Command, RebootMode, Role } from './wire.ts';

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
  let phase: FlashPhase = FlashPhase.Checking;
  const report = (next: FlashPhase, sentBytes = 0) => {
    phase = next;
    options.onProgress?.({ phase: next, sentBytes, totalBytes });
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
  try {
    let sentBytes = 0;
    report(FlashPhase.Writing, sentBytes);
    for (const chunk of planChunks(header.imageSize, maxChunk)) {
      const data = image.bytes.subarray(chunk.offset, chunk.offset + chunk.length);
      const echo = await stage(async () =>
        parseFwData(await bootClient.request(Command.FwData, fwDataPayload(chunk.offset, data), { signal })),
      );
      if (echo !== chunk.offset) {
        fail(`The device acknowledged offset ${hex(echo)} for a chunk sent to ${hex(chunk.offset)}.`);
      }
      sentBytes += chunk.length;
      report(FlashPhase.Writing, sentBytes);
    }
    report(FlashPhase.Verifying, totalBytes);
    await stage(() => bootClient.request(Command.FwEnd, [], { signal }));
  } catch (error) {
    await bootClient.request(Command.FwAbort).catch(() => undefined);
    throw error;
  }

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

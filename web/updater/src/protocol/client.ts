/**
 * A stop-and-wait client for one device link (`protocol/sysex.md` §1): one request outstanding,
 * a reply matched by `cmd | 0x40` and `seq`, and the same request (same `seq`) resent on timeout.
 * A LOCKED reply (§4.9) is not resent at once: the client asks UNLOCK what to tell the user,
 * offers it as an {@link UnlockPrompt}, waits for UNLOCKED and then sends the request again.
 * Knows nothing about Web MIDI: anything that can send and receive whole SysEx messages will do.
 */

import { REQUEST_DEFAULTS, type RequestSettings } from './defaults.ts';
import { toHex } from './encoding.ts';
import { decodeFrame, encodeFrame, type Frame } from './frame.ts';
import { type DeviceMessage, parseDeviceMessage, parseUnlock, type UnlockInfo, unlockPayload } from './messages.ts';
import {
  Command,
  MAX_SEQ,
  NOTIFICATION_FIRST,
  NOTIFICATION_LAST,
  REPLY_FLAG,
  STATUS_MEANINGS,
  STATUS_NAMES,
  Status,
  UNLOCK_WINDOW_SECONDS,
  UNSOLICITED_FIRST,
} from './wire.ts';

/** A link that carries whole SysEx messages, F0 to F7. */
export interface Transport {
  /**
   * Sends one message.
   *
   * @param bytes - F0 … F7.
   */
  send(bytes: Uint8Array): void;
  /**
   * Listens for messages.
   *
   * @param listener - Called with each whole message received.
   * @returns A function that stops listening.
   */
  subscribe(listener: (bytes: Uint8Array) => void): () => void;
}

/** Why a request failed. */
export const RequestFailure = {
  /** No reply after every retry. */
  Timeout: 'timeout',
  /** The device replied with a status other than OK. */
  Status: 'status',
  /** The reply could not be parsed. */
  BadReply: 'bad-reply',
  /** The caller gave up, or the client was closed. */
  Aborted: 'aborted',
  /** The device answered LOCKED and was not unlocked on the device (§4.9). */
  Locked: 'locked',
} as const;
export type RequestFailure = (typeof RequestFailure)[keyof typeof RequestFailure];

/** A request that did not succeed. */
export class RequestError extends Error {
  override name = 'RequestError';
  readonly failure: RequestFailure;
  /** The status code, for a `status` failure. */
  readonly status: number | undefined;

  /**
   * @param failure - What went wrong.
   * @param message - The reader's sentence.
   * @param status - The device's status code, if it sent one.
   */
  constructor(failure: RequestFailure, message: string, status?: number) {
    super(message);
    this.failure = failure;
    this.status = status;
  }
}

/**
 * Names a status code as the spec does, with what it means.
 *
 * @param status - The code.
 * @returns E.g. `BAD_ADDR (the offset is out of range, misaligned or out of order)`.
 */
export function describeStatus(status: number): string {
  const name = STATUS_NAMES[status] ?? `status ${status}`;
  const meaning = STATUS_MEANINGS[status];
  return meaning ? `${name} (${meaning})` : name;
}

/** What the client reports about traffic, for a raw view. */
export interface TrafficEntry {
  direction: 'in' | 'out';
  bytes: Uint8Array;
}

/** Requests that get the long reply timeout (§1): erases, read-back and CRC. */
const SLOW_COMMANDS: ReadonlySet<number> = new Set([
  Command.FwBegin,
  Command.FwEnd,
  Command.Commit,
  Command.FactoryReset,
  Command.PresetSave,
  Command.PresetReset,
  Command.PresetName,
  Command.PresetDelete,
  Command.PresetMove,
]);

/** Statuses after which the same request is worth sending again. */
const RETRYABLE: ReadonlySet<number> = new Set([Status.BadChecksum, Status.Busy]);

/** How many times one request is sent again after an unlock before the client gives up on it. */
const MAX_UNLOCKS = 3;

/** Milliseconds in a second. */
const MS_PER_SECOND = 1000;

/** What the user is asked while a request waits for the device to be unlocked (§4.9). */
export interface UnlockPrompt {
  /** The request that answered LOCKED: FW_BEGIN, REBOOT or FACTORY_RESET. */
  cmd: number;
  /** The unlock combo's buttons, by their display names; empty when the device did not say. */
  buttons: string[];
  /** How long to hold them, in ms; 0 for a press; null when the device did not say. */
  holdMs: number | null;
  /** When the device's unlock window closes, as a `Date.now()` time. */
  windowEndsAt: number;
  /** Stops waiting: the request fails with `aborted`. */
  cancel: () => void;
}

/** How an unlock wait ended: UNLOCKED 1, UNLOCKED 0, or nothing in time. */
type UnlockOutcome = 'unlocked' | 'closed' | 'timeout';

/** The `seq` notifications carry: the device ignores it (§4.6.2). */
const NOTIFICATION_SEQ = 0;

/** Options for one request. */
export interface RequestOptions {
  /** Stops waiting and fails the request with `aborted`. */
  signal?: AbortSignal;
}

/** Talks to one device over one transport. */
export class SysexClient {
  readonly #transport: Transport;
  readonly #settings: RequestSettings;
  readonly #unsubscribe: () => void;
  readonly #messageListeners = new Set<(message: DeviceMessage) => void>();
  readonly #trafficListeners = new Set<(entry: TrafficEntry) => void>();
  readonly #replyListeners = new Set<(frame: Frame) => void>();
  readonly #promptListeners = new Set<(prompt: UnlockPrompt | null) => void>();
  /** Unlock waits in progress, so close() can end them. */
  readonly #unlockWaits = new Set<AbortController>();
  /** Prompts on show, oldest first; the newest is the one offered. */
  #prompts: UnlockPrompt[] = [];
  #nextSeq = 0;
  #queue: Promise<unknown> = Promise.resolve();
  #isClosed = false;

  /**
   * @param transport - The link to the device.
   * @param overrides - Timing overrides.
   */
  constructor(transport: Transport, overrides: Partial<RequestSettings> = {}) {
    this.#transport = transport;
    this.#settings = { ...REQUEST_DEFAULTS, ...overrides };
    this.#unsubscribe = transport.subscribe((bytes) => this.#receive(bytes));
  }

  /**
   * Listens for LOG, EVENT, PARAM_CHANGED and BOOTED messages.
   *
   * @param listener - Called with each one.
   * @returns A function that stops listening.
   */
  onMessage(listener: (message: DeviceMessage) => void): () => void {
    this.#messageListeners.add(listener);
    return () => this.#messageListeners.delete(listener);
  }

  /**
   * Listens for every message sent and received, gemifi or not.
   *
   * @param listener - Called with each one.
   * @returns A function that stops listening.
   */
  onTraffic(listener: (entry: TrafficEntry) => void): () => void {
    this.#trafficListeners.add(listener);
    return () => this.#trafficListeners.delete(listener);
  }

  /**
   * Listens for the unlock prompt (§4.9): set while a request waits for the user to unlock the
   * device, null once none does.
   *
   * @param listener - Called with each change.
   * @returns A function that stops listening.
   */
  onUnlockPrompt(listener: (prompt: UnlockPrompt | null) => void): () => void {
    this.#promptListeners.add(listener);
    return () => this.#promptListeners.delete(listener);
  }

  /** @returns The unlock prompt on show now, or null. */
  unlockPrompt(): UnlockPrompt | null {
    return this.#prompts.at(-1) ?? null;
  }

  /** Stops listening to the transport and fails anything still waiting. */
  close(): void {
    this.#isClosed = true;
    this.#unsubscribe();
    for (const wait of this.#unlockWaits) {
      wait.abort();
    }
    this.#messageListeners.clear();
    this.#trafficListeners.clear();
    this.#replyListeners.clear();
  }

  /**
   * Sends a request and waits for its reply. Requests queue: only one is ever outstanding. A
   * LOCKED reply waits for the user to unlock the device (§4.9), then sends the request again.
   *
   * @param cmd - The request command.
   * @param payload - Its 7-bit payload.
   * @param options - An abort signal.
   * @returns The reply payload after the status byte, once the status is OK.
   */
  async request(cmd: number, payload: number[] = [], options: RequestOptions = {}): Promise<number[]> {
    for (let unlocks = 0; ; unlocks++) {
      try {
        return await this.#enqueue(cmd, payload, options);
      } catch (error) {
        const isLocked = error instanceof RequestError && error.status === Status.Locked;
        if (isLocked === false) {
          throw error;
        }
        if (unlocks >= MAX_UNLOCKS) {
          throw new RequestError(
            RequestFailure.Locked,
            `The device still answered LOCKED after it was unlocked ${MAX_UNLOCKS} times.`,
            Status.Locked,
          );
        }
        await this.#waitForUnlock(cmd, options.signal);
      }
    }
  }

  /**
   * @param cmd - The request command.
   * @param payload - Its payload.
   * @param options - An abort signal.
   * @returns The reply body; the request waits its turn in the queue.
   */
  #enqueue(cmd: number, payload: number[], options: RequestOptions): Promise<number[]> {
    const run = this.#queue.then(() => this.#exchange(cmd, payload, options));
    this.#queue = run.catch(() => undefined);
    return run;
  }

  /**
   * After a LOCKED reply: asks UNLOCK what to tell the user, offers the prompt and waits for
   * UNLOCKED (§4.9). Resolves once the device is unlocked; throws `locked` when it was not, and
   * `aborted` when the caller, the prompt's cancel or close() stopped it.
   *
   * @param cmd - The request that answered LOCKED.
   * @param signal - The caller's signal.
   */
  async #waitForUnlock(cmd: number, signal: AbortSignal | undefined): Promise<void> {
    this.#throwIfStopped(signal);
    const wait = new AbortController();
    const onAbort = () => wait.abort();
    signal?.addEventListener('abort', onAbort, { once: true });
    this.#unlockWaits.add(wait);
    let isCancelled = false;
    let settle: (outcome: UnlockOutcome | 'aborted') => void = () => undefined;
    const outcome = new Promise<UnlockOutcome | 'aborted'>((resolve) => {
      settle = resolve;
    });
    wait.signal.addEventListener('abort', () => settle('aborted'), { once: true });
    // Listen before asking, so an UNLOCKED sent straight after the reply is not missed.
    const unsubscribe = this.onMessage((message) => {
      if (message.kind === 'unlocked') {
        settle(message.isUnlocked ? 'unlocked' : 'closed');
      }
    });
    let prompt: UnlockPrompt | null = null;
    let timer: ReturnType<typeof setTimeout> | undefined;
    try {
      const info = await this.#askUnlock(wait.signal);
      if (info?.isUnlocked === true) {
        return;
      }
      const windowSeconds = info === null || info.windowSeconds === 0 ? UNLOCK_WINDOW_SECONDS : info.windowSeconds;
      const windowMs = windowSeconds * MS_PER_SECOND;
      prompt = {
        cmd,
        buttons: info?.buttons ?? [],
        holdMs: info?.holdMs ?? null,
        windowEndsAt: Date.now() + windowMs,
        cancel: () => {
          isCancelled = true;
          wait.abort();
        },
      };
      this.#showPrompt(prompt);
      timer = setTimeout(() => settle('timeout'), windowMs + this.#settings.unlockGraceMs);
      const result = await outcome;
      if (result === 'aborted') {
        throw new RequestError(
          RequestFailure.Aborted,
          isCancelled ? "Cancelled: the device wasn't unlocked." : 'Stopped.',
        );
      }
      if (result === 'closed') {
        throw new RequestError(
          RequestFailure.Locked,
          "The device wasn't unlocked: its unlock window closed first.",
          Status.Locked,
        );
      }
      if (result === 'timeout') {
        throw new RequestError(
          RequestFailure.Locked,
          `The device wasn't unlocked: it did not report an unlock within ${windowSeconds} s.`,
          Status.Locked,
        );
      }
    } finally {
      clearTimeout(timer);
      unsubscribe();
      signal?.removeEventListener('abort', onAbort);
      this.#unlockWaits.delete(wait);
      if (prompt !== null) {
        this.#hidePrompt(prompt);
      }
    }
  }

  /**
   * UNLOCK for the prompt: `open` 0, then `open` 1 if the window has closed already.
   *
   * @param signal - Stops it.
   * @returns What the device says, or null when it did not say (the prompt then names no buttons).
   */
  async #askUnlock(signal: AbortSignal): Promise<UnlockInfo | null> {
    try {
      const info = parseUnlock(await this.#enqueue(Command.Unlock, unlockPayload(false), { signal }));
      const isClosed = info.isUnlocked === false && info.windowSeconds === 0;
      return isClosed ? parseUnlock(await this.#enqueue(Command.Unlock, unlockPayload(true), { signal })) : info;
    } catch (error) {
      if (signal.aborted || this.#isClosed) {
        throw error;
      }
      return null;
    }
  }

  /**
   * @param prompt - A prompt to offer.
   */
  #showPrompt(prompt: UnlockPrompt): void {
    this.#prompts = [...this.#prompts, prompt];
    this.#notifyPrompt();
  }

  /**
   * @param prompt - A prompt no longer waited on.
   */
  #hidePrompt(prompt: UnlockPrompt): void {
    this.#prompts = this.#prompts.filter((shown) => shown !== prompt);
    this.#notifyPrompt();
  }

  /** Tells the prompt listeners what is on show now. */
  #notifyPrompt(): void {
    const current = this.unlockPrompt();
    for (const listener of this.#promptListeners) {
      listener(current);
    }
  }

  /**
   * Sends a host → device notification (0x20–0x3F, §4.6.2) at once: no reply is awaited, nothing
   * is retried, and it does not queue behind a request that is waiting for its reply. The device
   * ignores `seq`, so notifications send 0 and leave the requests' sequence alone. Send them only
   * to a device with HELLO caps bit 6.
   *
   * @param cmd - The notification command, 0x20–0x3F.
   * @param payload - Its 7-bit payload.
   */
  notify(cmd: number, payload: number[] = []): void {
    const isNotification = cmd >= NOTIFICATION_FIRST && cmd <= NOTIFICATION_LAST;
    if (isNotification === false) {
      throw new RangeError(`${toHex([cmd])} is not a notification command (0x20–0x3F)`);
    }
    this.#throwIfStopped(undefined);
    this.#write(encodeFrame(cmd, NOTIFICATION_SEQ, payload));
  }

  /**
   * @param cmd - The request command.
   * @returns How long to wait for its reply: longer for FW_BEGIN, FW_END, COMMIT and FACTORY_RESET (§1).
   */
  #timeoutFor(cmd: number): number {
    return SLOW_COMMANDS.has(cmd) ? this.#settings.longReplyTimeoutMs : this.#settings.replyTimeoutMs;
  }

  /** @returns The next sequence number, wrapping at 127. */
  #takeSeq(): number {
    const seq = this.#nextSeq;
    this.#nextSeq = (seq + 1) % (MAX_SEQ + 1);
    return seq;
  }

  /**
   * One request's stop-and-wait loop.
   *
   * @param cmd - The request command.
   * @param payload - Its payload.
   * @param options - An abort signal.
   * @returns The reply body after the status byte.
   */
  async #exchange(cmd: number, payload: number[], options: RequestOptions): Promise<number[]> {
    const seq = this.#takeSeq();
    const frame = encodeFrame(cmd, seq, payload);
    const { maxRetries } = this.#settings;
    const replyTimeoutMs = this.#timeoutFor(cmd);
    let lastStatus: number | undefined;
    for (let attempt = 0; attempt <= maxRetries; attempt++) {
      this.#throwIfStopped(options.signal);
      this.#write(frame);
      const reply = await this.#awaitReply(cmd | REPLY_FLAG, seq, replyTimeoutMs, options.signal);
      if (reply === null) {
        continue;
      }
      const [status, ...body] = reply.payload;
      if (status === undefined) {
        throw new RequestError(RequestFailure.BadReply, `The reply to command ${toHex([cmd])} had no status.`);
      }
      if (status === Status.Ok) {
        return body;
      }
      if (RETRYABLE.has(status)) {
        lastStatus = status;
        continue;
      }
      throw new RequestError(RequestFailure.Status, `The device answered ${describeStatus(status)}.`, status);
    }
    if (lastStatus !== undefined) {
      throw new RequestError(
        RequestFailure.Status,
        `The device kept answering ${describeStatus(lastStatus)} after ${maxRetries} retries.`,
        lastStatus,
      );
    }
    throw new RequestError(
      RequestFailure.Timeout,
      `No reply to command ${toHex([cmd])} after ${maxRetries + 1} tries ${replyTimeoutMs} ms apart.`,
    );
  }

  /**
   * @param signal - The caller's signal.
   * @returns Nothing; throws `aborted` when the request should stop.
   */
  #throwIfStopped(signal: AbortSignal | undefined): void {
    const isStopped = this.#isClosed || signal?.aborted === true;
    if (isStopped) {
      throw new RequestError(RequestFailure.Aborted, 'Stopped.');
    }
  }

  /**
   * Waits for one reply frame.
   *
   * @param replyCmd - `cmd | 0x40`.
   * @param seq - The request's `seq`.
   * @param timeoutMs - How long to wait.
   * @param signal - The caller's signal.
   * @returns The frame, or null on timeout.
   */
  #awaitReply(
    replyCmd: number,
    seq: number,
    timeoutMs: number,
    signal: AbortSignal | undefined,
  ): Promise<Frame | null> {
    return new Promise((resolve, reject) => {
      const finish = (result: Frame | null) => {
        clearTimeout(timer);
        this.#replyListeners.delete(listener);
        signal?.removeEventListener('abort', onAbort);
        resolve(result);
      };
      const listener = (frame: Frame) => {
        const isOurs = frame.cmd === replyCmd && frame.seq === seq;
        if (isOurs) {
          finish(frame);
        }
      };
      const onAbort = () => {
        clearTimeout(timer);
        this.#replyListeners.delete(listener);
        reject(new RequestError(RequestFailure.Aborted, 'Stopped.'));
      };
      const timer = setTimeout(() => finish(null), timeoutMs);
      this.#replyListeners.add(listener);
      signal?.addEventListener('abort', onAbort, { once: true });
    });
  }

  /**
   * @param bytes - A whole message to send.
   */
  #write(bytes: Uint8Array): void {
    this.#transport.send(bytes);
    for (const listener of this.#trafficListeners) {
      listener({ direction: 'out', bytes });
    }
  }

  /**
   * Routes one received message: 0x60–0x7F to the message listeners, anything else to the reply waiter.
   *
   * @param bytes - A whole message.
   */
  #receive(bytes: Uint8Array): void {
    for (const listener of this.#trafficListeners) {
      listener({ direction: 'in', bytes });
    }
    const result = decodeFrame(bytes);
    if (result.ok === false) {
      return;
    }
    const { frame } = result;
    if (frame.cmd >= UNSOLICITED_FIRST) {
      this.#dispatchMessage(frame);
      return;
    }
    for (const listener of [...this.#replyListeners]) {
      listener(frame);
    }
  }

  /**
   * @param frame - An unsolicited frame.
   */
  #dispatchMessage(frame: Frame): void {
    let message: DeviceMessage | null;
    try {
      message = parseDeviceMessage(frame);
    } catch {
      return;
    }
    if (message === null) {
      return;
    }
    for (const listener of this.#messageListeners) {
      listener(message);
    }
  }
}

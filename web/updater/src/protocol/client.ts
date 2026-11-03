/**
 * A stop-and-wait client for one device link (`protocol/sysex.md` §1): one request outstanding,
 * a reply matched by `cmd | 0x40` and `seq`, and the same request (same `seq`) resent on timeout.
 * Knows nothing about Web MIDI: anything that can send and receive whole SysEx messages will do.
 */

import { REQUEST_DEFAULTS, type RequestSettings } from './defaults.ts';
import { toHex } from './encoding.ts';
import { decodeFrame, encodeFrame, type Frame } from './frame.ts';
import { type DeviceMessage, parseDeviceMessage } from './messages.ts';
import {
  Command,
  MAX_SEQ,
  NOTIFICATION_FIRST,
  NOTIFICATION_LAST,
  REPLY_FLAG,
  STATUS_MEANINGS,
  STATUS_NAMES,
  Status,
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
]);

/** Statuses after which the same request is worth sending again. */
const RETRYABLE: ReadonlySet<number> = new Set([Status.BadChecksum, Status.Busy]);

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

  /** Stops listening to the transport and fails anything still waiting. */
  close(): void {
    this.#isClosed = true;
    this.#unsubscribe();
    this.#messageListeners.clear();
    this.#trafficListeners.clear();
    this.#replyListeners.clear();
  }

  /**
   * Sends a request and waits for its reply. Requests queue: only one is ever outstanding.
   *
   * @param cmd - The request command.
   * @param payload - Its 7-bit payload.
   * @param options - An abort signal.
   * @returns The reply payload after the status byte, once the status is OK.
   */
  request(cmd: number, payload: number[] = [], options: RequestOptions = {}): Promise<number[]> {
    const run = this.#queue.then(() => this.#exchange(cmd, payload, options));
    this.#queue = run.catch(() => undefined);
    return run;
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

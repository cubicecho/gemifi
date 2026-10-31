/**
 * Cutting whole SysEx messages out of a raw MIDI byte stream, for links that carry bytes rather
 * than messages: a serial port (classic ESP32 and ESP8266 speak MIDI over their USB-UART,
 * `GM_MIDI_SERIAL`), a TCP socket, a DIN MIDI adapter. The frames inside are the same as over
 * USB-MIDI (`protocol/sysex.md` §1). Same rules as the host tool's `SysexAssembler`:
 *
 * - real-time bytes (0xF8–0xFF) may sit inside a SysEx and are dropped;
 * - 0xF0 starts a message, even in the middle of another (the partial one is dropped);
 * - 0xF7 ends it;
 * - any other status byte aborts a partial message;
 * - data bytes outside a message (an ESP boot log, running-status MIDI) are ignored.
 */

import { SYSEX_END, SYSEX_START } from './wire.ts';

/** First real-time status byte: these can appear anywhere and never interrupt a SysEx. */
const REALTIME_FIRST = 0xf8;
/** The status bit. */
const STATUS_BIT = 0x80;
/** A message longer than this is dropped, so noise without an F7 cannot grow the buffer forever. */
export const MAX_STREAM_MESSAGE_BYTES = 65_536;

/** Collects SysEx messages from chunks of a byte stream, split anywhere. */
export class SysexStreamParser {
  readonly #maxBytes: number;
  #buffer: number[] | null = null;

  /**
   * @param maxBytes - The longest message kept; a longer one is dropped.
   */
  constructor(maxBytes: number = MAX_STREAM_MESSAGE_BYTES) {
    this.#maxBytes = maxBytes;
  }

  /**
   * Feeds bytes as they arrive.
   *
   * @param chunk - Any number of bytes, cut anywhere.
   * @returns The messages completed by this chunk, F0 … F7, in order.
   */
  push(chunk: ArrayLike<number>): Uint8Array[] {
    const done: Uint8Array[] = [];
    for (let i = 0; i < chunk.length; i++) {
      const byte = chunk[i] ?? 0;
      if (byte >= REALTIME_FIRST) {
        continue;
      }
      if (byte === SYSEX_START) {
        this.#buffer = [byte];
        continue;
      }
      const buffer = this.#buffer;
      if (buffer === null) {
        continue;
      }
      if (byte === SYSEX_END) {
        buffer.push(byte);
        done.push(Uint8Array.from(buffer));
        this.#buffer = null;
      } else if ((byte & STATUS_BIT) !== 0) {
        this.#buffer = null;
      } else if (buffer.length >= this.#maxBytes) {
        this.#buffer = null;
      } else {
        buffer.push(byte);
      }
    }
    return done;
  }

  /** Drops a partial message, e.g. after the link was reopened. */
  reset(): void {
    this.#buffer = null;
  }
}

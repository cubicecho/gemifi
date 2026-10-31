/**
 * Web Serial side of the updater: the same protocol as over USB-MIDI, carried as raw MIDI bytes
 * on a serial port. Classic ESP32 and ESP8266 have no native USB; a gemifi build for them
 * (`GM_MIDI_SERIAL`) speaks MIDI over the board's USB-UART at 115200 baud. The SysEx frames are cut
 * out of the byte stream by {@link SysexStreamParser}; the rest (boot log, other MIDI) is dropped.
 *
 * Unlike a USB-MIDI port, the serial port stays open while the chip reboots (the USB-UART bridge
 * is not reset), so the client is kept across a REBOOT and HELLO simply goes unanswered for a
 * moment. Framework-free; the React layer subscribes to it.
 */

import { SysexClient, type Transport } from '../protocol/client.ts';
import { REBOOT_DEFAULTS } from '../protocol/defaults.ts';
import type { DeviceLink } from '../protocol/flash.ts';
import type { PortRole } from '../protocol/ports.ts';
import { SysexStreamParser } from '../protocol/stream.ts';

/** The baud rate gemifi's serial MIDI uses (`GM_MIDI_SERIAL`). */
export const DEFAULT_SERIAL_BAUD = 115_200;

/** Why Web Serial is not available here. */
export const SerialUnavailable = {
  InsecureContext: 'insecure-context',
  /** Firefox and Safari have no Web Serial. */
  NoWebSerial: 'no-web-serial',
} as const;
export type SerialUnavailable = (typeof SerialUnavailable)[keyof typeof SerialUnavailable];

/**
 * Says whether this browser can open serial ports at all.
 *
 * @returns Null when it can, or the reason it cannot.
 */
export function serialUnavailableReason(): SerialUnavailable | null {
  if (globalThis.isSecureContext === false) {
    return SerialUnavailable.InsecureContext;
  }
  const hasWebSerial = typeof navigator !== 'undefined' && 'serial' in navigator;
  return hasWebSerial ? null : SerialUnavailable.NoWebSerial;
}

/** Hex digits in a USB vendor or product id. */
const USB_ID_DIGITS = 4;
/** Hex. */
const HEX_RADIX = 16;

/** USB-UART bridges found on ESP dev boards, by `vendor:product`. */
const BRIDGES: Readonly<Record<string, string>> = Object.freeze({
  '1a86:7523': 'CH340',
  '1a86:55d4': 'CH9102',
  '10c4:ea60': 'CP210x',
  '0403:6001': 'FTDI FT232',
  '0403:6015': 'FTDI FT231X',
  '303a:1001': 'ESP USB-JTAG/serial',
});

/**
 * Names a serial port for display.
 *
 * @param info - From `port.getInfo()`.
 * @returns E.g. `CH340 (1a86:7523)`, or `Serial port` when the browser gives no USB ids.
 */
export function describeSerialPort(info: {
  usbVendorId?: number | undefined;
  usbProductId?: number | undefined;
}): string {
  if (info.usbVendorId === undefined || info.usbProductId === undefined) {
    return 'Serial port';
  }
  const hex = (n: number) => n.toString(HEX_RADIX).padStart(USB_ID_DIGITS, '0');
  const ids = `${hex(info.usbVendorId)}:${hex(info.usbProductId)}`;
  const bridge = BRIDGES[ids];
  return bridge === undefined ? `USB serial (${ids})` : `${bridge} (${ids})`;
}

/** What the page draws from. Replaced, never mutated, on every change. */
export interface SerialSnapshot {
  /** The open port, or null. */
  port: SerialPort | null;
  /** What it is called. */
  label: string;
  baudRate: number;
  client: SysexClient | null;
  /** Why the port closed by itself, if it did. */
  error: string | null;
}

/** The empty snapshot. */
const CLOSED: SerialSnapshot = Object.freeze({ port: null, label: '', baudRate: 0, client: null, error: null });

/** One serial port carrying gemifi SysEx, and the client for it. */
export class SerialLink implements DeviceLink {
  readonly #listeners = new Set<() => void>();
  readonly #settleMs: number;
  #snapshot: SerialSnapshot = CLOSED;
  #reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  #writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  #rx = new Set<(bytes: Uint8Array) => void>();
  #isOpen = false;

  /**
   * @param settleMs - How long {@link waitFor} lets the link settle before handing it out.
   */
  constructor(settleMs: number = REBOOT_DEFAULTS.portSettleMs) {
    this.#settleMs = settleMs;
  }

  /**
   * Listens for any change to the snapshot.
   *
   * @param listener - Called after each change.
   * @returns A function that stops listening.
   */
  subscribe = (listener: () => void): (() => void) => {
    this.#listeners.add(listener);
    return () => this.#listeners.delete(listener);
  };

  /** @returns The current snapshot. */
  getSnapshot = (): SerialSnapshot => this.#snapshot;

  /** @returns The client for the open port. */
  current(): SysexClient | null {
    return this.#snapshot.client;
  }

  /**
   * Waits for an open port. A serial port does not change with the device's role, so the caller
   * asks HELLO to find out which it is.
   *
   * @param _role - Unused: one port carries the app and a bootloader alike.
   * @param timeoutMs - How long to wait.
   * @param signal - Stops waiting.
   * @returns The client.
   */
  waitFor(_role: PortRole, timeoutMs: number, signal?: AbortSignal): Promise<SysexClient> {
    return new Promise((resolve, reject) => {
      const started = Date.now();
      const tick = () => {
        if (signal?.aborted === true) {
          reject(new Error('Stopped.'));
          return;
        }
        const { client } = this.#snapshot;
        if (client !== null) {
          setTimeout(() => resolve(client), this.#settleMs);
          return;
        }
        if (Date.now() - started > timeoutMs) {
          reject(new Error(`The serial port did not come back within ${timeoutMs} ms.`));
          return;
        }
        setTimeout(tick, this.#settleMs);
      };
      tick();
    });
  }

  /**
   * Asks the browser for a port (it shows its own chooser) and opens it.
   *
   * @param baudRate - The serial speed; gemifi uses 115200.
   */
  async request(baudRate: number = DEFAULT_SERIAL_BAUD): Promise<void> {
    const port = await navigator.serial.requestPort();
    await this.open(port, baudRate);
  }

  /**
   * Opens a port, replacing any open one.
   *
   * @param port - From `navigator.serial`.
   * @param baudRate - The serial speed.
   */
  async open(port: SerialPort, baudRate: number): Promise<void> {
    await this.close();
    await port.open({ baudRate });
    // The browser asserts DTR and RTS on open; the usual auto-reset circuit ignores both at once.
    // Release RTS (EN) first so the chip is not reset: DTR alone only pulls IO0.
    await port.setSignals({ requestToSend: false }).catch(() => undefined);
    await port.setSignals({ dataTerminalReady: false }).catch(() => undefined);
    const writable = port.writable;
    if (writable === null) {
      await port.close();
      throw new Error('The serial port opened without a way to write to it.');
    }
    this.#writer = writable.getWriter();
    this.#isOpen = true;
    port.addEventListener('disconnect', this.#onDisconnect);
    const client = new SysexClient(this.#transport());
    this.#publish({ port, label: describeSerialPort(port.getInfo()), baudRate, client, error: null });
    void this.#readLoop(port);
  }

  /**
   * Closes the port, if one is open. Waits for the read loop to let go, so the same port can be
   * handed to another tool (esptool) straight after.
   */
  async close(): Promise<void> {
    const { port, client } = this.#snapshot;
    this.#isOpen = false;
    client?.close();
    if (port === null) {
      return;
    }
    port.removeEventListener('disconnect', this.#onDisconnect);
    await this.#reader?.cancel().catch(() => undefined);
    this.#writer?.releaseLock();
    this.#writer = null;
    await port.close().catch(() => undefined);
    this.#publish(CLOSED);
  }

  /** @returns A transport over the open port: writes are queued on its writer in order. */
  #transport(): Transport {
    return {
      send: (bytes) => {
        const writer = this.#writer;
        if (writer === null) {
          return;
        }
        // A write that fails (port gone) shows up as a request timeout, like a lost MIDI message.
        writer.write(bytes).catch(() => undefined);
      },
      subscribe: (listener) => {
        const parser = new SysexStreamParser();
        const onBytes = (bytes: Uint8Array) => {
          for (const message of parser.push(bytes)) {
            listener(message);
          }
        };
        this.#rx.add(onBytes);
        return () => this.#rx.delete(onBytes);
      },
    };
  }

  /**
   * Reads until the port closes. A framing or parity error ends one reader; a new one is taken
   * while the port is still readable.
   *
   * @param port - The open port.
   */
  async #readLoop(port: SerialPort): Promise<void> {
    while (this.#isOpen && port.readable !== null) {
      const reader = port.readable.getReader();
      this.#reader = reader;
      try {
        for (;;) {
          const { value, done } = await reader.read();
          if (done) {
            break;
          }
          for (const listener of this.#rx) {
            listener(value);
          }
        }
      } catch {
        // A break or framing error; the loop takes a new reader if the port is still there.
      } finally {
        reader.releaseLock();
        this.#reader = null;
      }
    }
    if (this.#isOpen) {
      await this.close();
      this.#publish({ ...CLOSED, error: 'The serial port closed. Was the board unplugged?' });
    }
  }

  #onDisconnect = (): void => {
    void this.close().then(() => this.#publish({ ...CLOSED, error: 'The serial device was unplugged.' }));
  };

  /**
   * @param snapshot - The new state.
   */
  #publish(snapshot: SerialSnapshot): void {
    this.#snapshot = snapshot;
    for (const listener of this.#listeners) {
      listener();
    }
  }
}

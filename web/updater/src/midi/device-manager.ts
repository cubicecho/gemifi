/**
 * Web MIDI side of the updater: port discovery, input/output pairing, hot-plug and the
 * {@link DeviceLink} the flash sequence drives. Framework-free; the React layer subscribes to it.
 *
 * A gemifi device disappears and comes back as a different port on every reboot
 * ("gemifi boot (…)" in the bootloader, "gemifi (…)" in the app), so the manager follows it:
 * when the selected port goes away it selects the next gemifi port that appears.
 */

import { SysexClient, type Transport } from '../protocol/client.ts';
import { REBOOT_DEFAULTS } from '../protocol/defaults.ts';
import type { DeviceLink } from '../protocol/flash.ts';
import { classifyPortName, PortRole } from '../protocol/ports.ts';
import type { ProbePort } from '../vendor/maudio.ts';

/** An input and an output with the same name: one device's MIDI port. */
export interface PortPair {
  /** Stable while the pair is connected: its name, plus `#n` for a second port of that name. */
  id: string;
  name: string;
  role: PortRole;
  input: MIDIInput;
  output: MIDIOutput;
}

/** What the page draws from. Replaced, never mutated, on every change. */
export interface ConnectionSnapshot {
  pairs: readonly PortPair[];
  selected: PortPair | null;
  client: SysexClient | null;
}

/** Why Web MIDI with SysEx is not available here. */
export const MidiUnavailable = {
  /** The page is not served over HTTPS or localhost. */
  InsecureContext: 'insecure-context',
  /** The browser has no Web MIDI (Safari). */
  NoWebMidi: 'no-web-midi',
} as const;
export type MidiUnavailable = (typeof MidiUnavailable)[keyof typeof MidiUnavailable];

/**
 * Says whether this browser can be asked for SysEx access at all.
 *
 * @returns Null when it can, or the reason it cannot.
 */
export function midiUnavailableReason(): MidiUnavailable | null {
  if (globalThis.isSecureContext === false) {
    return MidiUnavailable.InsecureContext;
  }
  const hasWebMidi = typeof navigator !== 'undefined' && 'requestMIDIAccess' in navigator;
  return hasWebMidi ? null : MidiUnavailable.NoWebMidi;
}

/**
 * A transport over one input/output pair. The input is opened the first time the transport is
 * used, not when it's made: the M-Audio updater makes one for every port on the computer, and on
 * Windows opening a port another program holds can stall the browser.
 *
 * @param pair - The ports.
 * @returns The transport, and `release` to close both ports.
 */
function pairTransport(pair: PortPair): Transport & { release: () => void } {
  let isOpen = false;
  const open = () => {
    if (isOpen === false) {
      isOpen = true;
      void pair.input.open().catch(() => undefined);
    }
  };
  return {
    release() {
      isOpen = false;
      void pair.input.close().catch(() => undefined);
      void pair.output.close().catch(() => undefined);
    },
    send(bytes) {
      open();
      try {
        pair.output.send(bytes);
      } catch (error) {
        // A port that vanished mid-request (a REBOOT) throws; the request times out instead.
        const isGone = pair.output.state === 'disconnected';
        if (isGone === false) {
          throw error;
        }
      }
    },
    subscribe(listener) {
      open();
      const onMessage = (event: MIDIMessageEvent) => {
        if (event.data) {
          listener(event.data);
        }
      };
      pair.input.addEventListener('midimessage', onMessage);
      return () => pair.input.removeEventListener('midimessage', onMessage);
    },
  };
}

/**
 * Pairs connected inputs with connected outputs of the same name.
 *
 * @param access - The MIDI access object.
 * @returns The pairs, in input order.
 */
export function pairPorts(access: MIDIAccess): PortPair[] {
  const outputs = [...access.outputs.values()].filter((port) => port.state === 'connected');
  const used = new Set<MIDIOutput>();
  const seen = new Map<string, number>();
  const pairs: PortPair[] = [];
  for (const input of access.inputs.values()) {
    const name = input.name ?? '';
    const output = outputs.find((candidate) => candidate.name === name && used.has(candidate) === false);
    const isConnected = input.state === 'connected';
    if (output === undefined || isConnected === false) {
      continue;
    }
    used.add(output);
    const count = seen.get(name) ?? 0;
    seen.set(name, count + 1);
    pairs.push({ id: count === 0 ? name : `${name}#${count + 1}`, name, role: classifyPortName(name), input, output });
  }
  return pairs;
}

/**
 * Asks for Web MIDI with SysEx. Chromium shows a permission prompt; Firefox installs a
 * site-permission add-on after its own prompt.
 *
 * @returns The access object.
 */
export function requestSysexAccess(): Promise<MIDIAccess> {
  return navigator.requestMIDIAccess({ sysex: true });
}

/** Follows gemifi ports as they come and go, and owns the client for the selected one. */
export class DeviceManager implements DeviceLink {
  readonly #access: MIDIAccess;
  readonly #listeners = new Set<() => void>();
  readonly #settleMs: number;
  #snapshot: ConnectionSnapshot = { pairs: [], selected: null, client: null };
  /** Set once the user picks a port by hand, so a manual choice of a non-gemifi port is kept. */
  #isManual = false;
  /** Raw transports handed to the vendor uploader, one per input port. */
  readonly #rawTransports = new WeakMap<
    MIDIInput,
    { output: MIDIOutput; transport: ReturnType<typeof pairTransport> }
  >();

  /**
   * @param access - From {@link requestSysexAccess}.
   * @param settleMs - How long a port that just appeared is left before it is handed to a waiter.
   */
  constructor(access: MIDIAccess, settleMs: number = REBOOT_DEFAULTS.portSettleMs) {
    this.#access = access;
    this.#settleMs = settleMs;
    access.addEventListener('statechange', this.#onStateChange);
    this.#refresh();
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
  getSnapshot = (): ConnectionSnapshot => this.#snapshot;

  /** @returns The client for the selected port. */
  current(): SysexClient | null {
    return this.#snapshot.client;
  }

  /**
   * Selects a port by hand.
   *
   * @param id - A pair's id, or null to disconnect.
   */
  select(id: string | null): void {
    this.#isManual = id !== null;
    const pair = this.#snapshot.pairs.find((candidate) => candidate.id === id) ?? null;
    this.#setSelected(pair, this.#snapshot.pairs);
  }

  /**
   * Waits until the selected port is a gemifi port in `role`, then lets it settle.
   *
   * @param role - `app` or `boot`.
   * @param timeoutMs - How long to wait.
   * @param signal - Stops waiting.
   * @returns The client for it.
   */
  waitFor(role: PortRole, timeoutMs: number, signal?: AbortSignal): Promise<SysexClient> {
    return new Promise((resolve, reject) => {
      let settleTimer: ReturnType<typeof setTimeout> | undefined;
      const cleanup = () => {
        clearTimeout(deadline);
        clearTimeout(settleTimer);
        unsubscribe();
        signal?.removeEventListener('abort', onAbort);
      };
      const check = () => {
        clearTimeout(settleTimer);
        const { selected, client } = this.#snapshot;
        if (selected?.role !== role || client === null) {
          return;
        }
        settleTimer = setTimeout(() => {
          const isStillThere = this.#snapshot.client === client;
          if (isStillThere) {
            cleanup();
            resolve(client);
          }
        }, this.#settleMs);
      };
      const onAbort = () => {
        cleanup();
        reject(new Error('Stopped.'));
      };
      const deadline = setTimeout(() => {
        cleanup();
        const where = role === PortRole.Boot ? 'the bootloader' : 'the app';
        reject(new Error(`The device did not come back as ${where} within ${timeoutMs} ms.`));
      }, timeoutMs);
      const unsubscribe = this.subscribe(check);
      signal?.addEventListener('abort', onAbort, { once: true });
      check();
    });
  }

  /**
   * Every connected port as a raw transport, for the M-Audio updater's probe
   * (`src/vendor/maudio.ts`): its bootloader is not a gemifi port and has a name of its own.
   *
   * @returns The ports, with the selected one marked.
   */
  probePorts = (): ProbePort[] => {
    const { pairs, selected } = this.#snapshot;
    return pairs.map((pair) => {
      let raw = this.#rawTransports.get(pair.input);
      if (raw?.output !== pair.output) {
        raw = { output: pair.output, transport: pairTransport(pair) };
        this.#rawTransports.set(pair.input, raw);
      }
      const { transport } = raw;
      const release = () => {
        // The selected port's client holds the same ports open: let go of it too.
        if (this.#snapshot.selected?.input === pair.input) {
          this.select(null);
        }
        transport.release();
        this.#rawTransports.delete(pair.input);
      };
      return { id: pair.id, name: pair.name, selected: pair.id === selected?.id, transport, release };
    });
  };

  /**
   * Every connected MIDI input, the device's own and any other (a synth, a DAW), for the MIDI
   * monitor and MIDI learn. Listen with {@link subscribe}: it fires when ports come and go.
   *
   * @returns The inputs, in the browser's order.
   */
  inputs = (): MIDIInput[] => [...this.#access.inputs.values()].filter((port) => port.state === 'connected');

  /** Stops following ports and closes the client. */
  close(): void {
    this.#access.removeEventListener('statechange', this.#onStateChange);
    this.#snapshot.client?.close();
    this.#listeners.clear();
  }

  #onStateChange = (): void => {
    this.#refresh();
  };

  /** Re-reads the ports and keeps or moves the selection. */
  #refresh(): void {
    const pairs = pairPorts(this.#access);
    const { selected } = this.#snapshot;
    const kept = selected === null ? null : (pairs.find((pair) => pair.id === selected.id) ?? null);
    if (kept === null) {
      this.#isManual = false;
    }
    const isAutomatic = this.#isManual === false;
    const gemifi = pairs.find((pair) => pair.role !== PortRole.Other) ?? null;
    const next = kept ?? (isAutomatic ? gemifi : null);
    this.#setSelected(next, pairs);
  }

  /**
   * Publishes a new snapshot, replacing the client when the selected port changed.
   *
   * @param pair - The port to select.
   * @param pairs - Every pair.
   */
  #setSelected(pair: PortPair | null, pairs: readonly PortPair[]): void {
    const previous = this.#snapshot;
    const isSamePort = previous.selected?.input === pair?.input && previous.selected?.output === pair?.output;
    let client = previous.client;
    if (isSamePort === false) {
      previous.client?.close();
      client = pair === null ? null : new SysexClient(pairTransport(pair));
    }
    this.#snapshot = { pairs, selected: pair, client };
    for (const listener of this.#listeners) {
      listener();
    }
  }
}

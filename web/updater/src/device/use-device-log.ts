import { useCallback, useEffect, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';
import { toHex } from '../protocol/encoding.ts';
import type { DeviceMessage } from '../protocol/messages.ts';

/** How many lines each view keeps. */
const MAX_LINES = 1000;
/** Width the device timestamp is padded to. */
const TIME_WIDTH = 8;

/** LOG level names (§4). */
const LEVEL_NAMES: Readonly<Record<number, string>> = Object.freeze({ 0: 'ERROR', 1: 'WARN', 2: 'INFO', 3: 'DEBUG' });
/** EVENT type names (§4). */
const EVENT_NAMES: Readonly<Record<number, string>> = Object.freeze({ 1: 'button', 2: 'key', 3: 'pot' });

/**
 * One LOG or EVENT as a line of text.
 *
 * @param message - The message.
 * @returns E.g. `[    1234 ms] INFO  oct- down`.
 */
export function formatDeviceMessage(message: DeviceMessage): string {
  const time = `[${String(message.tMs).padStart(TIME_WIDTH)} ms]`;
  if (message.kind === 'log') {
    const level = (LEVEL_NAMES[message.level] ?? `L${message.level}`).padEnd('DEBUG'.length);
    return `${time} ${level} ${message.text}`;
  }
  const type = EVENT_NAMES[message.type] ?? `type ${message.type}`;
  return `${time} EVENT ${type} ${message.index} = ${message.value}`;
}

/**
 * Appends a line, keeping the newest {@link MAX_LINES}.
 *
 * @param lines - The current lines.
 * @param line - The new one.
 * @returns The new lines.
 */
function append(lines: readonly string[], line: string): string[] {
  const next = [...lines, line];
  return next.length > MAX_LINES ? next.slice(next.length - MAX_LINES) : next;
}

/**
 * Collects the device's LOG and EVENT messages, and every SysEx message both ways.
 * The lines outlive a reboot: the client changes, the log carries on.
 *
 * @param client - The connected device, or null.
 * @returns The decoded lines, the raw hex lines, and `clear`.
 */
export function useDeviceLog(client: SysexClient | null): {
  lines: readonly string[];
  rawLines: readonly string[];
  clear: () => void;
} {
  const [lines, setLines] = useState<readonly string[]>([]);
  const [rawLines, setRawLines] = useState<readonly string[]>([]);

  useEffect(() => {
    if (client === null) {
      return;
    }
    const stopMessages = client.onMessage((message) => setLines((prev) => append(prev, formatDeviceMessage(message))));
    const stopTraffic = client.onTraffic(({ direction, bytes }) => {
      const arrow = direction === 'in' ? '←' : '→';
      setRawLines((prev) => append(prev, `${arrow} ${toHex(bytes)}`));
    });
    return () => {
      stopMessages();
      stopTraffic();
    };
  }, [client]);

  const clear = useCallback(() => {
    setLines([]);
    setRawLines([]);
  }, []);

  return { lines, rawLines, clear };
}

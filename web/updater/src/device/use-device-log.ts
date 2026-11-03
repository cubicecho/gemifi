import { useCallback, useEffect, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';
import { toHex } from '../protocol/encoding.ts';
import { type DeviceMessage, encoderDelta, formatVersion } from '../protocol/messages.ts';
import { EventType } from '../protocol/wire.ts';

/** How many lines each view keeps. */
const MAX_LINES = 1000;
/** Width the device timestamp is padded to. */
const TIME_WIDTH = 8;

/** LOG level names (§4). */
const LEVEL_NAMES: Readonly<Record<number, string>> = Object.freeze({ 0: 'ERROR', 1: 'WARN', 2: 'INFO', 3: 'DEBUG' });
/** EVENT type names (§4). */
const EVENT_NAMES: Readonly<Record<number, string>> = Object.freeze({
  1: 'button',
  2: 'key',
  3: 'analog',
  4: 'encoder',
});

/**
 * @param tMs - Device time.
 * @returns E.g. `[    1234 ms]`.
 */
function timeColumn(tMs: number): string {
  return `[${String(tMs).padStart(TIME_WIDTH)} ms]`;
}

/**
 * One LOG, EVENT or PARAM_CHANGED as a line of text.
 *
 * @param message - The message.
 * @returns E.g. `[    1234 ms] INFO  oct- down`.
 */
export function formatDeviceMessage(message: DeviceMessage): string {
  if (message.kind === 'param') {
    // PARAM_CHANGED carries no device time, so its time column is blank.
    const blank = ' '.repeat(timeColumn(0).length);
    return `${blank} PARAM ${message.id}[${message.index}] = ${message.value}`;
  }
  if (message.kind === 'booted') {
    const blank = ' '.repeat(timeColumn(0).length);
    return `${blank} BOOTED ${message.hello.nodeName ?? 'device'} ${formatVersion(message.hello.fwVersion)}`;
  }
  const time = timeColumn(message.tMs);
  if (message.kind === 'log') {
    const level = (LEVEL_NAMES[message.level] ?? `L${message.level}`).padEnd('DEBUG'.length);
    return `${time} ${level} ${message.text}`;
  }
  const type = EVENT_NAMES[message.type] ?? `type ${message.type}`;
  const value = message.type === EventType.Encoder ? encoderDelta(message.value) : message.value;
  return `${time} EVENT ${type} ${message.index} = ${value}`;
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
 * Collects the device's LOG, EVENT and PARAM_CHANGED messages, and every SysEx message both ways.
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

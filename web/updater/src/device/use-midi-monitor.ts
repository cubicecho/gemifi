import { useCallback, useEffect, useRef, useState } from 'react';
import { portTag } from '../midi/device-ports.ts';
import { MidiLog, type MidiLogEntry } from '../midi/midi-log.ts';
import type { SysexClient } from '../protocol/client.ts';

/** How often the list is redrawn at most while messages pour in. */
const REDRAW_MS = 100;
/** The tag of the port the page talks to. */
export const MAIN_PORT_TAG = 'Device';

/** What {@link useMidiMonitor} hands the card. */
export interface MidiMonitor {
  /** What is shown: the newest messages, oldest first; frozen while paused. */
  entries: readonly MidiLogEntry[];
  /** Messages that came in while paused, not shown yet. */
  waiting: number;
  isPaused: boolean;
  setPaused: (isPaused: boolean) => void;
  clear: () => void;
}

/**
 * Collects the channel and system messages the device sends: on the port the page talks to (from
 * the client's traffic, so the ?demo device and serial links work too), and on the device's other
 * ports (a DAW port, a DIN port). The list is redrawn at most every {@link REDRAW_MS}, so a fader
 * sweep doesn't redraw it hundreds of times a second.
 *
 * @param client - The connected device, or null.
 * @param otherInputs - The device's other input ports.
 * @returns The list and its controls.
 */
export function useMidiMonitor(client: SysexClient | null, otherInputs: readonly MIDIInput[]): MidiMonitor {
  const log = useRef(new MidiLog());
  const started = useRef(performance.now());
  const [entries, setEntries] = useState<readonly MidiLogEntry[]>([]);
  const [isPaused, setIsPaused] = useState(false);
  const [shownTotal, setShownTotal] = useState(0);
  const pausedRef = useRef(false);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [, setTick] = useState(0);

  const redraw = useCallback(() => {
    timer.current = null;
    if (pausedRef.current) {
      setTick((n) => n + 1); // the waiting count moves on
      return;
    }
    setEntries(log.current.entries());
    setShownTotal(log.current.total);
  }, []);

  const add = useCallback(
    (bytes: ArrayLike<number>, port: string) => {
      const isKept = log.current.add(bytes, port, performance.now() - started.current);
      if (isKept && timer.current === null) {
        timer.current = setTimeout(redraw, REDRAW_MS);
      }
    },
    [redraw],
  );

  useEffect(() => {
    if (client === null) {
      return;
    }
    return client.onTraffic(({ direction, bytes }) => {
      if (direction === 'in') {
        add(bytes, MAIN_PORT_TAG);
      }
    });
  }, [client, add]);

  useEffect(() => {
    const stops = otherInputs.map((input) => {
      const tag = portTag(input.name ?? 'MIDI');
      const onMessage = (event: MIDIMessageEvent) => {
        if (event.data) {
          add(event.data, tag);
        }
      };
      input.addEventListener('midimessage', onMessage);
      return () => input.removeEventListener('midimessage', onMessage);
    });
    return () => {
      for (const stop of stops) {
        stop();
      }
    };
  }, [otherInputs, add]);

  useEffect(
    () => () => {
      if (timer.current !== null) {
        clearTimeout(timer.current);
      }
    },
    [],
  );

  const setPaused = useCallback(
    (next: boolean) => {
      pausedRef.current = next;
      setIsPaused(next);
      if (next === false) {
        redraw();
      }
    },
    [redraw],
  );

  const clear = useCallback(() => {
    log.current.clear();
    started.current = performance.now();
    setEntries([]);
    setShownTotal(0);
  }, []);

  return { entries, waiting: Math.max(0, log.current.total - shownTotal), isPaused, setPaused, clear };
}

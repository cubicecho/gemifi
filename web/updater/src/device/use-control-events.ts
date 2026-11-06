import { useEffect, useRef, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';
import { controlKey } from '../protocol/layout.ts';
import type { EventMessage } from '../protocol/messages.ts';
import { ComponentKind, EventType } from '../protocol/wire.ts';
import { holdEvents } from './events-lease.ts';

/** How long a moved knob, fader, encoder or pad stays lit after its last EVENT. */
export const LIT_MS = 700;
/** The largest EVENT value (§4). */
const EVENT_VALUE_MAX = 0x3fff;
/** A pad EVENT's value: the peak reading × 4, so 4095 × 4 at most. */
const PAD_EVENT_MAX = 16380;

/** What {@link useControlEvents} gives. */
export interface ControlEvents {
  /** The controls moved or held just now, by {@link controlKey}. */
  lit: ReadonlySet<string>;
  /** Analog positions 0..1 from their EVENTs, by {@link controlKey}. */
  values: ReadonlyMap<string, number>;
  /** The last EVENT, with a running number so the same control twice is still news. */
  last: { event: EventMessage; n: number } | null;
}

/**
 * @param type - An EVENT type.
 * @returns The component kind a drawing knows it by, or null for a type it doesn't draw.
 */
export function kindOfEvent(type: number): number | null {
  switch (type) {
    case EventType.Button:
      return ComponentKind.Button;
    case EventType.Key:
      return ComponentKind.KeyMatrix;
    case EventType.Analog:
      return ComponentKind.Analog;
    case EventType.Encoder:
      return ComponentKind.Encoder;
    case EventType.Pad:
      return ComponentKind.Pad;
    default:
      return null;
  }
}

/**
 * While `isOn`, turns EVENTs on (`debug.events`, put back after) and follows them: keys and
 * buttons are lit while held, the others for {@link LIT_MS} after they last moved.
 *
 * @param client - The device, with config, or null.
 * @param isOn - Whether anything wants the EVENTs now.
 * @returns The lit controls, the analog values and the last EVENT.
 */
export function useControlEvents(client: SysexClient | null, isOn: boolean): ControlEvents {
  const [lit, setLit] = useState<ReadonlySet<string>>(new Set());
  const [values, setValues] = useState<ReadonlyMap<string, number>>(new Map());
  const [last, setLast] = useState<ControlEvents['last']>(null);
  const counter = useRef(0);
  useEffect(() => {
    if (client === null || isOn === false) {
      setLit(new Set());
      return;
    }
    const release = holdEvents(client);
    const held = new Set<string>();
    const until = new Map<string, number>();
    let shown = '';
    const show = () => {
      const now = Date.now();
      for (const [key, end] of until) {
        if (end <= now) {
          until.delete(key);
        }
      }
      const next = new Set([...held, ...until.keys()]);
      const signature = [...next].sort().join(' ');
      if (signature !== shown) {
        shown = signature;
        setLit(next);
      }
    };
    const stop = client.onMessage((message) => {
      if (message.kind !== 'event') {
        return;
      }
      const kind = kindOfEvent(message.type);
      if (kind === null) {
        return;
      }
      const key = controlKey(kind, message.index);
      const isHeldKind = kind === ComponentKind.Button || kind === ComponentKind.KeyMatrix;
      if (isHeldKind) {
        if (message.value === 0) {
          held.delete(key);
          until.set(key, Date.now() + LIT_MS / 2);
        } else {
          held.add(key);
        }
      } else {
        until.set(key, Date.now() + LIT_MS);
      }
      if (kind === ComponentKind.Analog) {
        setValues((prev) => new Map(prev).set(key, message.value / EVENT_VALUE_MAX));
      }
      if (kind === ComponentKind.Pad) {
        setValues((prev) => new Map(prev).set(key, Math.min(1, message.value / PAD_EVENT_MAX)));
      }
      counter.current += 1;
      setLast({ event: message, n: counter.current });
      show();
    });
    const TICK_MS = 100;
    const timer = setInterval(show, TICK_MS);
    return () => {
      clearInterval(timer);
      stop();
      release();
    };
  }, [client, isOn]);
  return { lit, values, last };
}

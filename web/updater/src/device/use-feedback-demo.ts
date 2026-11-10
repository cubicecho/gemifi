import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  colourToRgb,
  DEMO_EFFECTS,
  type DemoEffect,
  type DemoFrame,
  type EffectRun,
  frameChanges,
  type LedColour,
} from '../demo/effects.ts';
import type { SysexClient } from '../protocol/client.ts';
import {
  type DisplayComponent,
  fbLedRgbPayload,
  fbTextPayload,
  hostDrives,
  type LedComponent,
} from '../protocol/feedback.ts';
import { ComponentKind, Notification } from '../protocol/wire.ts';
import type { FeedbackController } from './use-feedback.ts';

/** How often a frame is worked out and its changes sent: 20 a second. */
export const DEMO_TICK_MS = 50;

/** Half the effect's own pace. */
const HALF_SPEED = 0.5;
/** The speeds offered, as multiples of the effect's own pace. */
export const DEMO_SPEEDS = [HALF_SPEED, 1, 2] as const;
export type DemoSpeed = (typeof DEMO_SPEEDS)[number];

/** The colour picked before the reader picks one. */
export const DEMO_FIRST_COLOUR: LedColour = 0x0040ff;

/** What {@link useFeedbackDemo} hands the card. */
export interface DemoController {
  /** Every LED the device lists, by index. */
  leds: LedComponent[];
  /** Every display the device lists, by index. */
  displays: DisplayComponent[];
  /** The effect running, or null. */
  running: DemoEffect | null;
  /** What was last sent (the preview), or null when stopped. */
  frame: DemoFrame | null;
  speed: DemoSpeed;
  setSpeed: (speed: DemoSpeed) => void;
  /** The colour picked, for an effect that picks one (Breathe). */
  colour: LedColour;
  /** Picks it; a running effect shows it from the next frame. */
  setColour: (colour: LedColour) => void;
  /** Starts an effect (stopping the one running). */
  start: (id: string) => void;
  /** Stops and hands every LED and display back to the device (FB_RELEASE 0). */
  stop: () => void;
}

/**
 * Runs the demo effects on every LED and display the device lists, with the same FB_LED_RGB and
 * FB_TEXT notifications a DAW script sends (`protocol/sysex.md` §4.6.2). Only what changed since
 * the last frame is sent. LEDs with `owner: local` ignore host feedback, so nothing is sent to
 * them. Stopping, leaving the page or losing the device releases everything.
 *
 * @param client - The connected device, which has HELLO caps bit 6.
 * @param feedback - The component list and the sender, shared with the Host feedback card.
 * @returns The state and the actions.
 */
export function useFeedbackDemo(client: SysexClient, feedback: FeedbackController): DemoController {
  const { state, send, releaseAll } = feedback;
  const components = state.kind === 'ready' ? state.components : null;
  const leds = useMemo(
    () => (components ?? []).filter((c): c is LedComponent => c.kind === ComponentKind.Led),
    [components],
  );
  const displays = useMemo(
    () => (components ?? []).filter((c): c is DisplayComponent => c.kind === ComponentKind.Display),
    [components],
  );
  const [running, setRunning] = useState<DemoEffect | null>(null);
  const [frame, setFrame] = useState<DemoFrame | null>(null);
  const [speed, setSpeedState] = useState<DemoSpeed>(1);
  const speedRef = useRef<DemoSpeed>(1);
  const [colour, setColourState] = useState<LedColour>(DEMO_FIRST_COLOUR);
  const colourRef = useRef<LedColour>(DEMO_FIRST_COLOUR);
  /** What the device was last sent; null makes the next frame send everything. */
  const sentRef = useRef<DemoFrame | null>(null);

  // BOOTED: the device forgot every state set here, so send the next frame whole.
  useEffect(
    () =>
      client.onMessage((message) => {
        if (message.kind === 'booted') {
          sentRef.current = null;
        }
      }),
    [client],
  );

  // The running effect's timer. It restarts (from the start) when the component list changes,
  // e.g. after a reload, so the canvas always matches the device.
  useEffect(() => {
    if (running === null) {
      return;
    }
    const canvas = { leds: leds.length, displays: displays.map((d) => d.cols) };
    const step: EffectRun = running.start(canvas, Math.random, { colour: () => colourRef.current });
    let t = 0;
    sentRef.current = null;
    const tick = () => {
      const next = step(t);
      t += DEMO_TICK_MS * speedRef.current;
      const changes = frameChanges(sentRef.current, next);
      for (const i of changes.leds) {
        const led = leds[i];
        if (led !== undefined && hostDrives(led)) {
          send(Notification.FbLedRgb, () => fbLedRgbPayload(led.index, colourToRgb(next.leds[i] ?? 0)));
        }
      }
      for (const d of changes.displays) {
        const display = displays[d];
        if (display !== undefined) {
          send(Notification.FbText, () => fbTextPayload(display.index, 0, 0, next.texts[d] ?? ''));
        }
      }
      sentRef.current = next;
      setFrame(next);
    };
    tick();
    const timer = setInterval(tick, DEMO_TICK_MS);
    return () => clearInterval(timer);
  }, [running, leds, displays, send]);

  // Leaving the card or losing the device: hand everything back.
  const isRunning = running !== null;
  useEffect(() => {
    if (isRunning === false) {
      return;
    }
    return () => releaseAll();
  }, [isRunning, releaseAll]);

  const start = useCallback((id: string) => {
    const effect = DEMO_EFFECTS.find((e) => e.id === id) ?? null;
    setRunning(effect);
  }, []);

  const stop = useCallback(() => {
    setRunning(null);
    setFrame(null);
    sentRef.current = null;
  }, []);

  const setSpeed = useCallback((next: DemoSpeed) => {
    speedRef.current = next;
    setSpeedState(next);
  }, []);

  const setColour = useCallback((next: LedColour) => {
    colourRef.current = next;
    setColourState(next);
  }, []);

  return { leds, displays, running, frame, speed, setSpeed, colour, setColour, start, stop };
}

/**
 * The ?demo device's notes and CCs, for the MIDI monitor: now and then a short phrase on the keys,
 * a mod-wheel sweep, a pitch bend and a pad hit, as a player would. Never in a production build.
 */

import type { Transport } from '../protocol/client.ts';

/** Time between two things played. */
const GAP_MS = 2500;
/** Time between the messages inside one thing. */
const STEP_MS = 60;

/** A few things to play, in turn: each is a list of messages, sent STEP_MS apart. */
const PHRASES: readonly (readonly number[][])[] = [
  // C major arpeggio, channel 1.
  [
    [0x90, 60, 96],
    [0x80, 60, 0],
    [0x90, 64, 88],
    [0x80, 64, 0],
    [0x90, 67, 100],
    [0x80, 67, 0],
    [0x90, 72, 110],
    [0x90, 72, 0],
  ],
  // Mod wheel up and back (CC 1).
  [0, 16, 32, 48, 64, 80, 96, 112, 127, 96, 64, 32, 0].map((v) => [0xb0, 1, v]),
  // Pitch wheel up and back to the centre.
  [
    [0xe0, 0x00, 0x50],
    [0xe0, 0x00, 0x60],
    [0xe0, 0x7f, 0x7f],
    [0xe0, 0x00, 0x60],
    [0xe0, 0x00, 0x40],
  ],
  // A pad hit on channel 10 with aftertouch, then a fader (CC 20).
  [
    [0x99, 36, 120],
    [0xd9, 40],
    [0xd9, 0],
    [0x89, 36, 0],
    [0xb0, 20, 40],
    [0xb0, 20, 45],
    [0xb0, 20, 50],
  ],
];

/**
 * Wraps the demo device's transport so its listeners also get the demo's notes and CCs, while
 * anyone listens.
 *
 * @param transport - The demo device's transport.
 * @returns The same transport, playing now and then.
 */
export function withDemoMidi(transport: Transport): Transport {
  const listeners = new Set<(bytes: Uint8Array) => void>();
  let timer: ReturnType<typeof setTimeout> | null = null;
  let phrase = 0;
  const play = () => {
    const messages = PHRASES[phrase % PHRASES.length] ?? [];
    phrase += 1;
    messages.forEach((message, i) => {
      setTimeout(() => {
        for (const listener of listeners) {
          listener(new Uint8Array(message));
        }
      }, i * STEP_MS);
    });
    timer = setTimeout(play, GAP_MS);
  };
  return {
    send: (bytes) => transport.send(bytes),
    subscribe(listener) {
      const stop = transport.subscribe(listener);
      listeners.add(listener);
      if (timer === null) {
        timer = setTimeout(play, GAP_MS);
      }
      return () => {
        stop();
        listeners.delete(listener);
        if (listeners.size === 0 && timer !== null) {
          clearTimeout(timer);
          timer = null;
        }
      };
    },
  };
}

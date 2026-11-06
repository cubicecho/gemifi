/**
 * A pretend MIDI input for `npm run dev` with ?demo: a synth that sends a Control Change or a Note
 * On every couple of seconds while something listens, so MIDI learn can be tried without one.
 * Never part of a production build (see `useLearnSources`).
 */

import type { LearnSource } from '../device/learn-sources.ts';

/** How often it plays something. */
const EVERY_MS = 2500;

/** @returns The pretend input. */
export function demoLearnSource(): LearnSource {
  return {
    id: 'demo-synth',
    name: 'Demo synth (pretend)',
    subscribe(listener) {
      let n = 0;
      const timer = setInterval(() => {
        n += 1;
        const NOTE_ON = 0x90;
        const CONTROL_CHANGE = 0xb0;
        const message = n % 2 === 0 ? [NOTE_ON, 36 + ((n * 5) % 24), 100] : [CONTROL_CHANGE, 20 + ((n * 7) % 40), 64];
        listener(new Uint8Array(message));
      }, EVERY_MS);
      return () => clearInterval(timer);
    },
  };
}

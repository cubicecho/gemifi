import { useEffect, useMemo, useState } from 'react';

/** Somewhere MIDI learn listens: another Web MIDI input (a synth, a DAW), or the ?demo's pretend one. */
export interface LearnSource {
  id: string;
  name: string;
  /**
   * Listens for MIDI messages.
   *
   * @param listener - Called with each message's bytes.
   * @returns A function that stops listening.
   */
  subscribe(listener: (bytes: Uint8Array) => void): () => void;
}

/**
 * @param input - A Web MIDI input.
 * @returns It as a {@link LearnSource}; listening opens it.
 */
export function inputSource(input: MIDIInput): LearnSource {
  return {
    id: input.id,
    name: input.name ?? input.id,
    subscribe(listener) {
      const onMessage = (event: MIDIMessageEvent) => {
        if (event.data) {
          listener(event.data);
        }
      };
      void input.open().catch(() => undefined);
      input.addEventListener('midimessage', onMessage);
      return () => input.removeEventListener('midimessage', onMessage);
    },
  };
}

/**
 * The inputs MIDI learn can listen on: every Web MIDI input, and under `npm run dev` with ?demo a
 * pretend synth that sends a CC or a note now and then.
 *
 * @param inputs - From `useMidiInputs`.
 * @returns The sources.
 */
export function useLearnSources(inputs: readonly MIDIInput[]): LearnSource[] {
  const [demo, setDemo] = useState<LearnSource | null>(null);
  useEffect(() => {
    if (import.meta.env.DEV && new URLSearchParams(window.location.search).has('demo')) {
      void import('../dev/demo-learn-source.ts').then((module) => setDemo(module.demoLearnSource()));
    }
  }, []);
  return useMemo(() => [...(demo === null ? [] : [demo]), ...inputs.map(inputSource)], [demo, inputs]);
}

/** What a learnt MIDI message is. */
export type LearntNumber = { kind: 'cc'; number: number } | { kind: 'note'; number: number };

/** Status bytes' high nibbles (MIDI 1.0). */
const NOTE_ON = 0x90;
const CONTROL_CHANGE = 0xb0;
const STATUS_MASK = 0xf0;

/**
 * @param bytes - A MIDI message.
 * @returns The CC or note number it carries, for a Control Change or a Note On (velocity above 0).
 */
export function learntNumber(bytes: ArrayLike<number>): LearntNumber | null {
  const [status = 0, data1 = 0, data2 = 0] = Array.from(bytes);
  const type = status & STATUS_MASK;
  if (type === CONTROL_CHANGE) {
    return { kind: 'cc', number: data1 };
  }
  if (type === NOTE_ON && data2 > 0) {
    return { kind: 'note', number: data1 };
  }
  return null;
}

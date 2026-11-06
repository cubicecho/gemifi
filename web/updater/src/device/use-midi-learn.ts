import { createContext, useCallback, useContext, useEffect, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';
import type { ParamSpec, Schema } from '../protocol/schema.ts';
import { type ParamRef, rowOfEvent } from './controls.ts';
import { type LearnSource, type LearntNumber, learntNumber } from './learn-sources.ts';
import { type ControlEvents, useControlEvents } from './use-control-events.ts';

/** The params "learn a number" can set, and what it listens for on each (§4.4 names). */
const LEARNABLE: Readonly<Record<string, 'cc' | 'note' | 'either'>> = {
  'analog.cc': 'cc',
  'encoder.cc': 'cc',
  'layer.encoder.cc': 'cc',
  'pad.note': 'note',
  'button.number': 'either',
  'layer.button.number': 'either',
};

/**
 * @param param - A param.
 * @returns What learning it listens for, or null when it can't be learnt.
 */
export function learnableAs(param: ParamSpec): 'cc' | 'note' | 'either' | null {
  return LEARNABLE[param.name] ?? null;
}

/**
 * @param want - What the field takes.
 * @param got - What arrived.
 * @returns Whether it fits.
 */
export function fits(want: 'cc' | 'note' | 'either', got: LearntNumber): boolean {
  return want === 'either' || want === got.kind;
}

/** What the settings tables read from MIDI learn. */
export interface LearnRows {
  /** The row "find a control" found last, with a running number so the same row again is news. */
  found: (ParamRef & { n: number }) | null;
  /** The field "learn a number" sets. */
  target: ParamRef | null;
  /** Called when a learnable field takes the focus. */
  onFocusField: (param: ParamSpec, index: number) => void;
}

/** The rows' view of MIDI learn; outside a provider nothing is found or targeted. */
export const LearnRowsContext = createContext<LearnRows>({ found: null, target: null, onFocusField: () => undefined });

/** @returns MIDI learn as the settings tables see it. */
export function useLearnRows(): LearnRows {
  return useContext(LearnRowsContext);
}

/** What {@link useMidiLearn} gives the learn card and the tables. */
export interface MidiLearn {
  rows: LearnRows;
  isFinding: boolean;
  setIsFinding: (on: boolean) => void;
  /** The EVENTs while finding: lit controls and values for the drawing. */
  events: ControlEvents;
  sourceId: string | null;
  setSourceId: (id: string | null) => void;
  /** Listening for the next CC or note on the source. */
  isListening: boolean;
  /** Starts listening, or stops when already listening. */
  toggleListening: () => void;
  /** What the last learn did, for the card. */
  note: string | null;
}

/**
 * "Find a control" and "learn a number" for the settings editor. Finding turns EVENTs on and points
 * at the row of each control moved; learning sets the focused CC or note field to the next CC or
 * Note On that arrives on another MIDI input.
 *
 * @param client - The device, or null.
 * @param schema - Its schema, or null before it is read.
 * @param sources - The inputs learning can listen on.
 * @param setValue - The editor's live SET_PARAM.
 * @param isShown - Whether the settings view is on screen; finding pauses while it is not.
 * @returns The state and actions.
 */
export function useMidiLearn(
  client: SysexClient | null,
  schema: Schema | null,
  sources: readonly LearnSource[],
  setValue: (param: ParamSpec, index: number, value: number) => void,
  isShown: boolean,
): MidiLearn {
  const [isFinding, setIsFinding] = useState(false);
  const [found, setFound] = useState<LearnRows['found']>(null);
  const [target, setTarget] = useState<ParamRef | null>(null);
  const [sourceId, setSourceId] = useState<string | null>(null);
  const [isListening, setIsListening] = useState(false);
  const [note, setNote] = useState<string | null>(null);
  const events = useControlEvents(client, isFinding && isShown);
  const { last } = events;

  useEffect(() => {
    if (last === null || schema === null) {
      return;
    }
    const row = rowOfEvent(schema, last.event.type, last.event.index);
    if (row !== null) {
      setFound({ ...row, n: last.n });
    }
  }, [last, schema]);

  useEffect(() => {
    if (found === null) {
      return;
    }
    const element = document.querySelector(`[data-param-row~="${found.id}:${found.index}"]`);
    element?.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  }, [found]);

  useEffect(() => {
    if (isFinding === false) {
      setFound(null);
    }
  }, [isFinding]);

  const source = sources.find((candidate) => candidate.id === sourceId) ?? null;
  const targetParam = target === null ? undefined : schema?.params.find((param) => param.id === target.id);
  useEffect(() => {
    if (isListening === false || source === null || target === null || targetParam === undefined) {
      return;
    }
    const want = learnableAs(targetParam);
    if (want === null) {
      return;
    }
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        setIsListening(false);
        setNote('Learning stopped.');
      }
    };
    window.addEventListener('keydown', onKey);
    const stop = source.subscribe((bytes) => {
      const got = learntNumber(bytes);
      if (got === null || fits(want, got) === false) {
        return;
      }
      setValue(targetParam, target.index, got.number);
      setIsListening(false);
      const label = targetParam.indexLabels?.[target.index] ?? String(target.index + 1);
      setNote(
        `${label}: ${targetParam.label} set to ${got.number} (${got.kind === 'cc' ? 'CC' : 'note'} from ${source.name}).`,
      );
    });
    return () => {
      window.removeEventListener('keydown', onKey);
      stop();
    };
  }, [isListening, source, target, targetParam, setValue]);

  const onFocusField = useCallback((param: ParamSpec, index: number) => {
    if (learnableAs(param) !== null) {
      setTarget((prev) => (prev?.id === param.id && prev.index === index ? prev : { id: param.id, index }));
    }
  }, []);

  const toggleListening = useCallback(() => {
    setNote(null);
    setIsListening((on) => on === false);
  }, []);

  return {
    rows: { found, target, onFocusField },
    isFinding,
    setIsFinding,
    events,
    sourceId,
    setSourceId,
    isListening,
    toggleListening,
    note,
  };
}

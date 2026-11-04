import { useCallback, useEffect, useRef, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';
import {
  deletePreset,
  isPresetDirty,
  type PresetInfo,
  readPresets,
  renamePreset,
  resetPreset,
  savePreset,
  selectPreset,
} from '../protocol/presets.ts';

/** Where reading the preset list is. */
export type PresetsState =
  | { kind: 'off' }
  | { kind: 'loading' }
  | { kind: 'error'; message: string }
  | { kind: 'ready'; presets: PresetInfo[]; active: number; dirty: boolean };

/** A preset action that is running. */
export type PresetAction = 'select' | 'create' | 'reset' | 'rename' | 'delete';

/** What a new preset starts from. */
export type PresetStart = 'current' | 'defaults';

/** A new preset, as the New preset dialog asks for it. */
export interface NewPreset {
  /** The slot it goes into; what that slot held before is replaced. */
  slot: number;
  name: string;
  /** The live values (a copy of the active preset), or the firmware's for that slot. */
  from: PresetStart;
}

/** What {@link usePresets} hands the card. */
export interface PresetsController {
  state: PresetsState;
  action: PresetAction | null;
  actionError: string | null;
  /** Selects a slot; unsaved edits of the active one are dropped (ask first). */
  select: (slot: number) => void;
  /**
   * Fills a slot, names it and makes it active. From `current`, the live values go in, unsaved
   * edits included. From `defaults`, the slot is cleared, so it holds the firmware's values, and
   * the active preset's unsaved edits are dropped.
   */
  create: (preset: NewPreset) => Promise<void>;
  /** Drops a slot's stored edits and name. */
  reset: (slot: number) => void;
  /** Deletes a preset; the later ones move up (§4.5). */
  remove: (slot: number) => void;
  /** Stores a new name; empty = the firmware's name. */
  rename: (slot: number, name: string) => Promise<void>;
  /** Reads the list again. */
  reload: () => void;
}

/**
 * @param error - Anything thrown.
 * @returns Its message.
 */
function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/**
 * The device's presets (§4.5): the list, the active slot and its dirty flag, kept in step with
 * PRESET_CHANGED, and the preset actions.
 *
 * @param client - The connected device, or null (state `off`).
 * @param enabled - The device has presets (HELLO caps bit 5).
 * @param onValuesChanged - Called when the live values were replaced (another preset loaded, or
 *   the active one reset), so the editor reads them again.
 * @returns The state and the actions.
 */
export function usePresets(
  client: SysexClient | null,
  enabled: boolean,
  onValuesChanged: () => void,
): PresetsController {
  const isOn = client !== null && enabled;
  const [state, setState] = useState<PresetsState>(isOn ? { kind: 'loading' } : { kind: 'off' });
  const [action, setAction] = useState<PresetAction | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [attempt, setAttempt] = useState(0);
  const onValuesChangedRef = useRef(onValuesChanged);
  onValuesChangedRef.current = onValuesChanged;
  // The active slot as last read, so a PRESET_CHANGED for another slot is told apart from a dirty change.
  const activeRef = useRef(0);

  // biome-ignore lint/correctness/useExhaustiveDependencies: `attempt` is the reload trigger.
  useEffect(() => {
    if (client === null || enabled === false) {
      setState({ kind: 'off' });
      return;
    }
    const controller = new AbortController();
    setState((prev) => (prev.kind === 'ready' ? prev : { kind: 'loading' }));
    readPresets(client, { signal: controller.signal }).then(
      (presets) => {
        const active = presets[0]?.active ?? 1;
        activeRef.current = active;
        const dirty = presets.some((info) => info.slot === active && isPresetDirty(info));
        setState({ kind: 'ready', presets, active, dirty });
      },
      (error: unknown) => {
        if (controller.signal.aborted === false) {
          setState({ kind: 'error', message: messageOf(error) });
        }
      },
    );
    return () => controller.abort();
  }, [client, enabled, attempt]);

  useEffect(
    () =>
      client?.onMessage((message) => {
        if (message.kind !== 'preset') {
          return;
        }
        // The list's flags follow too: a COMMIT that cleared dirty may have stored edits.
        setAttempt((n) => n + 1);
        if (message.slot !== activeRef.current) {
          // Another preset was loaded (here, from the keyboard or another host): its values are new.
          activeRef.current = message.slot;
          onValuesChangedRef.current();
        }
        setState((prev) => (prev.kind === 'ready' ? { ...prev, active: message.slot, dirty: message.dirty } : prev));
      }),
    [client],
  );

  const reload = useCallback(() => {
    setActionError(null);
    setAttempt((n) => n + 1);
  }, []);

  const run = useCallback(
    async (name: PresetAction, work: (device: SysexClient) => Promise<void>, rethrow: boolean) => {
      if (client === null) {
        return;
      }
      setAction(name);
      setActionError(null);
      try {
        await work(client);
      } catch (error) {
        if (rethrow) {
          throw error;
        }
        setActionError(messageOf(error));
      } finally {
        setAction(null);
      }
    },
    [client],
  );

  const select = useCallback(
    (slot: number) => {
      void run(
        'select',
        async (device) => {
          const isReload = slot === activeRef.current;
          await selectPreset(device, slot);
          if (isReload) {
            // The same slot again: reloaded, its unsaved edits dropped, but PRESET_CHANGED names
            // the same slot, so nothing else would re-read the values.
            onValuesChangedRef.current();
          }
        },
        false,
      );
    },
    [run],
  );

  const create = useCallback(
    ({ slot, name, from }: NewPreset) =>
      run(
        'create',
        async (device) => {
          if (from === 'current') {
            // SAVE first: PRESET_NAME keeps the values it finds stored.
            await savePreset(device, slot);
            await renamePreset(device, slot, name);
          } else {
            // An empty record with a name: the slot loads the firmware's values.
            await resetPreset(device, slot);
            await renamePreset(device, slot, name);
            await selectPreset(device, slot);
            if (slot === activeRef.current) {
              onValuesChangedRef.current();
            }
          }
          setAttempt((n) => n + 1);
        },
        true,
      ),
    [run],
  );

  const reset = useCallback(
    (slot: number) => {
      void run(
        'reset',
        async (device) => {
          await resetPreset(device, slot);
          setAttempt((n) => n + 1);
          if (slot === activeRef.current) {
            onValuesChangedRef.current();
          }
        },
        false,
      );
    },
    [run],
  );

  const rename = useCallback(
    (slot: number, name: string) =>
      run(
        'rename',
        async (device) => {
          await renamePreset(device, slot, name);
          setAttempt((n) => n + 1);
        },
        true,
      ),
    [run],
  );

  const remove = useCallback(
    (slot: number) => {
      void run(
        'delete',
        async (device) => {
          await deletePreset(device, slot);
          setAttempt((n) => n + 1);
          // The presets moved and the active one may be another now: its values are new.
          onValuesChangedRef.current();
        },
        false,
      );
    },
    [run],
  );

  return { state, action, actionError, select, create, reset, rename, remove, reload };
}

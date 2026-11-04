import { useCallback, useEffect, useState } from 'react';
import { RequestError, RequestFailure, type SysexClient } from '../protocol/client.ts';
import {
  commit,
  factoryReset,
  getSchema,
  type ParamValues,
  readAllValues,
  setParam,
  valueKey,
} from '../protocol/config.ts';
import type { HelloInfo } from '../protocol/messages.ts';
import { offersPresets } from '../protocol/presets.ts';
import { applyImport, exportProfile, planImport } from '../protocol/profile.ts';
import { isPreset, isVolatile, type ParamSpec, type Schema } from '../protocol/schema.ts';
import { Capability, Role, Status } from '../protocol/wire.ts';

/** Where loading the schema and values is. */
export type ConfigState =
  | { kind: 'off' }
  | { kind: 'loading' }
  | { kind: 'error'; message: string }
  | { kind: 'ready'; schema: Schema; values: ParamValues };

/** A button-driven action that is running. */
export type ConfigAction = 'save' | 'reset' | 'import';

/** What an import did, for the reader. */
export interface ImportReport {
  /** Values the device took. */
  applied: number;
  /** Names the schema does not have. */
  unknownNames: string[];
  /** Read-only params in the file. */
  readOnlyNames: string[];
  /** Values that were not written, one sentence each. */
  problems: string[];
  /** Set when the file came from another board. */
  boardWarning: string | null;
}

/** What {@link useConfig} hands the card. */
export interface ConfigController {
  state: ConfigState;
  /** Per-value errors, by `valueKey`. */
  fieldErrors: ReadonlyMap<string, string>;
  /**
   * Values were set since the last COMMIT or FACTORY_RESET. On a device with presets this counts
   * only the global params: the active preset's own edits are the device's `dirty` (PRESET_CHANGED).
   */
  isDirty: boolean;
  action: ConfigAction | null;
  actionError: string | null;
  importReport: ImportReport | null;
  /** Sends SET_PARAM for one value, live. */
  setValue: (param: ParamSpec, index: number, value: number) => void;
  /** Shows (or clears, with null) an error for a value that could not be sent at all. */
  setFieldError: (param: ParamSpec, index: number, message: string | null) => void;
  reload: () => void;
  /** Reads every value again, keeping the schema and the form on screen (another preset loaded). */
  refreshValues: () => void;
  save: () => void;
  reset: () => void;
  importText: (text: string) => void;
  /** The shown values as profile JSON, or null before they load. */
  exportJson: () => string | null;
}

/**
 * @param error - Anything thrown.
 * @returns Its message.
 */
function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/**
 * Words for a refused SET_PARAM.
 *
 * @param param - The param.
 * @param error - What the request threw.
 * @returns The field's error.
 */
function setErrorText(param: ParamSpec, error: unknown): string {
  const status = error instanceof RequestError ? error.status : undefined;
  if (status === Status.BadValue) {
    return `The device refused this value (BAD_VALUE): it takes ${param.min}..${param.max}.`;
  }
  if (status === Status.BadParam) {
    return 'The device refused this param (BAD_PARAM): unknown, out of range or read-only.';
  }
  return messageOf(error);
}

/**
 * @param map - A map.
 * @param key - The key to change.
 * @param value - The new value, or null to remove it.
 * @returns A copy with the change.
 */
function withEntry<V>(map: ReadonlyMap<string, V>, key: string, value: V | null): Map<string, V> {
  const next = new Map(map);
  if (value === null) {
    next.delete(key);
  } else {
    next.set(key, value);
  }
  return next;
}

/**
 * @param info - HELLO from the connected device, or null.
 * @returns Whether it is the app and offers config (§4.1).
 */
export function offersConfig(info: HelloInfo | null): boolean {
  return info !== null && info.role === Role.App && (info.caps & Capability.Config) !== 0;
}

/**
 * Loads the schema and every value from the device, keeps them in step with PARAM_CHANGED, and
 * runs the editor's actions: live SET_PARAM, COMMIT, FACTORY_RESET, reload and JSON import/export.
 *
 * @param client - The connected device, or null when there is none to configure (state `off`).
 * @param device - Its HELLO, for the profile's board id and firmware version.
 * @returns The state and the actions.
 */
export function useConfig(client: SysexClient | null, device: HelloInfo | null): ConfigController {
  const [state, setState] = useState<ConfigState>(client === null ? { kind: 'off' } : { kind: 'loading' });
  const [fieldErrors, setFieldErrors] = useState<ReadonlyMap<string, string>>(new Map());
  const [isDirty, setIsDirty] = useState(false);
  const [action, setAction] = useState<ConfigAction | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [importReport, setImportReport] = useState<ImportReport | null>(null);
  const [attempt, setAttempt] = useState(0);
  const hasPresets = device !== null && offersPresets(device);
  /** Whether a stored edit of this param counts towards {@link ConfigController.isDirty}. */
  const countsAsEdit = useCallback(
    (param: ParamSpec) => isVolatile(param) === false && (hasPresets === false || isPreset(param) === false),
    [hasPresets],
  );

  // biome-ignore lint/correctness/useExhaustiveDependencies: `attempt` is the reload trigger.
  useEffect(() => {
    if (client === null) {
      setState({ kind: 'off' });
      return;
    }
    const controller = new AbortController();
    setState({ kind: 'loading' });
    setFieldErrors(new Map());
    (async () => {
      const schema = await getSchema(client, { signal: controller.signal });
      const values = await readAllValues(client, schema, { signal: controller.signal });
      return { schema, values };
    })().then(
      ({ schema, values }) => setState({ kind: 'ready', schema, values }),
      (error: unknown) => {
        if (controller.signal.aborted === false) {
          setState({ kind: 'error', message: messageOf(error) });
        }
      },
    );
    return () => controller.abort();
  }, [client, attempt]);

  // A new client is a new device session: values set before it were lost or stored already.
  // biome-ignore lint/correctness/useExhaustiveDependencies: runs when the client changes.
  useEffect(() => {
    setIsDirty(false);
    setImportReport(null);
    setActionError(null);
  }, [client]);

  useEffect(
    () =>
      client?.onMessage((message) => {
        if (message.kind !== 'param') {
          return;
        }
        const key = valueKey(message.id, message.index);
        setState((prev) =>
          prev.kind === 'ready' ? { ...prev, values: withEntry(prev.values, key, message.value) } : prev,
        );
        setFieldErrors((prev) => withEntry(prev, key, null));
      }),
    [client],
  );

  const setFieldError = useCallback((param: ParamSpec, index: number, message: string | null) => {
    setFieldErrors((prev) => withEntry(prev, valueKey(param.id, index), message));
  }, []);

  const setValue = useCallback(
    (param: ParamSpec, index: number, value: number) => {
      if (client === null) {
        return;
      }
      const key = valueKey(param.id, index);
      setFieldErrors((prev) => withEntry(prev, key, null));
      setParam(client, { id: param.id, index, value }).then(
        (inEffect) => {
          setState((prev) =>
            prev.kind === 'ready' ? { ...prev, values: withEntry(prev.values, key, inEffect) } : prev,
          );
          if (countsAsEdit(param)) {
            setIsDirty(true);
          }
        },
        (error: unknown) => {
          const isStopped = error instanceof RequestError && error.failure === RequestFailure.Aborted;
          if (isStopped === false) {
            setFieldErrors((prev) => withEntry(prev, key, setErrorText(param, error)));
          }
        },
      );
    },
    [client, countsAsEdit],
  );

  const run = useCallback((name: ConfigAction, work: () => Promise<void>) => {
    setAction(name);
    setActionError(null);
    work()
      .catch((error: unknown) => setActionError(messageOf(error)))
      .finally(() => setAction(null));
  }, []);

  const reload = useCallback(() => {
    setImportReport(null);
    setActionError(null);
    setAttempt((n) => n + 1);
  }, []);

  const refreshValues = useCallback(() => {
    const current = state;
    if (client === null || current.kind !== 'ready') {
      return;
    }
    setFieldErrors(new Map());
    readAllValues(client, current.schema).then(
      (values) => setState((prev) => (prev.kind === 'ready' ? { ...prev, values } : prev)),
      (error: unknown) => setActionError(`The values could not be read again: ${messageOf(error)}`),
    );
  }, [client, state]);

  const save = useCallback(() => {
    if (client === null) {
      return;
    }
    run('save', async () => {
      await commit(client);
      setIsDirty(false);
    });
  }, [client, run]);

  const reset = useCallback(() => {
    if (client === null) {
      return;
    }
    run('reset', async () => {
      await factoryReset(client);
      setIsDirty(false);
      setImportReport(null);
      setAttempt((n) => n + 1);
    });
  }, [client, run]);

  const importText = useCallback(
    (text: string) => {
      const current = state;
      if (client === null || device === null || current.kind !== 'ready') {
        return;
      }
      setImportReport(null);
      run('import', async () => {
        let json: unknown;
        try {
          json = JSON.parse(text);
        } catch (error) {
          throw new Error(`The file is not JSON: ${messageOf(error)}`);
        }
        const plan = planImport(current.schema, json, device.boardId);
        const result = await applyImport(client, plan);
        setState((prev) => {
          if (prev.kind !== 'ready') {
            return prev;
          }
          const values = new Map(prev.values);
          for (const { id, index, value } of result.applied) {
            values.set(valueKey(id, index), value);
          }
          return { ...prev, values };
        });
        const byId = new Map(current.schema.params.map((param) => [param.id, param]));
        const touchesStored = result.applied.some(({ id }) => {
          const param = byId.get(id);
          return param !== undefined && countsAsEdit(param);
        });
        if (touchesStored) {
          setIsDirty(true);
        }
        setImportReport({
          applied: result.applied.length,
          unknownNames: plan.unknownNames,
          readOnlyNames: plan.readOnlyNames,
          problems: [...plan.problems, ...result.failures],
          boardWarning: plan.boardWarning,
        });
      });
    },
    [client, countsAsEdit, device, run, state],
  );

  const exportJson = useCallback(() => {
    if (device === null || state.kind !== 'ready') {
      return null;
    }
    const profile = exportProfile(state.schema, state.values, device);
    return `${JSON.stringify(profile, null, 2)}\n`;
  }, [device, state]);

  return {
    state,
    fieldErrors,
    isDirty,
    action,
    actionError,
    importReport,
    setValue,
    setFieldError,
    reload,
    refreshValues,
    save,
    reset,
    importText,
    exportJson,
  };
}

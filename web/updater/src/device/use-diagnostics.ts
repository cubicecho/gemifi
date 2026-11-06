import { useCallback, useEffect, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';
import { type Diagnostic, readDiagnostics } from '../protocol/diagnostics.ts';
import type { Schema } from '../protocol/schema.ts';

/** Where reading the diagnostics is. */
export type DiagnosticsState =
  | { kind: 'none' }
  | { kind: 'loading' }
  | { kind: 'ok'; diagnostics: Diagnostic[] }
  | { kind: 'error'; message: string };

/**
 * Reads the diagnostics (§4.8) once the device's schema is in, again whenever the schema is read
 * again, and on demand.
 *
 * @param client - The connected device with a config, or null.
 * @param schema - Its schema, or null before it is read (or without a config).
 * @returns The latest reading and `refresh` to read again.
 */
export function useDiagnostics(
  client: SysexClient | null,
  schema: Schema | null,
): { state: DiagnosticsState; refresh: () => void } {
  const [state, setState] = useState<DiagnosticsState>({ kind: 'none' });
  const [attempt, setAttempt] = useState(0);

  // biome-ignore lint/correctness/useExhaustiveDependencies: `attempt` is the refresh trigger.
  useEffect(() => {
    if (client === null || schema === null) {
      setState({ kind: 'none' });
      return;
    }
    const controller = new AbortController();
    setState((prev) => (prev.kind === 'ok' ? prev : { kind: 'loading' }));
    readDiagnostics(client, schema, { signal: controller.signal }).then(
      (diagnostics) => setState({ kind: 'ok', diagnostics }),
      (error: unknown) => {
        if (controller.signal.aborted === false) {
          setState({ kind: 'error', message: error instanceof Error ? error.message : String(error) });
        }
      },
    );
    return () => controller.abort();
  }, [client, schema, attempt]);

  const refresh = useCallback(() => setAttempt((n) => n + 1), []);
  return { state, refresh };
}

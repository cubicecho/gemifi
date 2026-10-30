import { useCallback, useEffect, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';
import { hello } from '../protocol/flash.ts';
import type { HelloInfo } from '../protocol/messages.ts';

/** Where asking HELLO is. */
export type HelloState =
  | { kind: 'none' }
  | { kind: 'loading' }
  | { kind: 'ok'; info: HelloInfo }
  | { kind: 'error'; message: string };

/**
 * Asks the connected device HELLO whenever the connection changes, and on demand.
 *
 * @param client - The connected device, or null.
 * @returns The latest answer and `refresh` to ask again.
 */
export function useHello(client: SysexClient | null): { state: HelloState; refresh: () => void } {
  const [state, setState] = useState<HelloState>({ kind: 'none' });
  const [attempt, setAttempt] = useState(0);

  // biome-ignore lint/correctness/useExhaustiveDependencies: `attempt` is the refresh trigger.
  useEffect(() => {
    if (client === null) {
      setState({ kind: 'none' });
      return;
    }
    const controller = new AbortController();
    setState({ kind: 'loading' });
    hello(client, { signal: controller.signal }).then(
      (info) => setState({ kind: 'ok', info }),
      (error: unknown) => {
        if (controller.signal.aborted === false) {
          setState({ kind: 'error', message: error instanceof Error ? error.message : String(error) });
        }
      },
    );
    return () => controller.abort();
  }, [client, attempt]);

  const refresh = useCallback(() => setAttempt((n) => n + 1), []);
  return { state, refresh };
}

import { useCallback, useState } from 'react';
import { runBuild } from './api.ts';

/** Where the last build is. Each state names the device it is for. */
export type BuildState =
  | { kind: 'idle' }
  | { kind: 'running'; deviceId: string; log: string }
  | { kind: 'done'; deviceId: string; log: string; exitCode: number | null }
  | { kind: 'failed'; deviceId: string; log: string; message: string };

/**
 * Runs builds one at a time, keeping the streamed log.
 *
 * @param onFinished - Called after every build, so the device list (and its images) is asked again.
 * @returns The last build's state and `start`.
 */
export function useBuild(onFinished: () => void): { state: BuildState; start: (deviceId: string) => void } {
  const [state, setState] = useState<BuildState>({ kind: 'idle' });

  const start = useCallback(
    (deviceId: string) => {
      setState({ kind: 'running', deviceId, log: '' });
      runBuild(deviceId, { onLog: (log) => setState({ kind: 'running', deviceId, log }) })
        .then(
          ({ log, exitCode }) => setState({ kind: 'done', deviceId, log, exitCode }),
          (error: unknown) =>
            setState((last) => ({
              kind: 'failed',
              deviceId,
              log: last.kind === 'running' ? last.log : '',
              message: error instanceof Error ? error.message : String(error),
            })),
        )
        .finally(onFinished);
    },
    [onFinished],
  );

  return { state, start };
}

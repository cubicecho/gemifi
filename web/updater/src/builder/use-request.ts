import { useCallback, useEffect, useState } from 'react';

/** A request's state, in the shape `QueryState` takes. */
export interface RequestState<T> {
  data: T | undefined;
  isPending: boolean;
  isError: boolean;
  error: unknown;
  /** Asks again, keeping the last answer on screen until the new one lands. */
  refetch: () => void;
}

/** What the hook holds: the answer and which key it answered. */
interface Held<T> {
  key: string;
  data: T | undefined;
  error: unknown;
  isPending: boolean;
}

/**
 * Runs a builder request when `key` changes, and again on `refetch`. An answer to a request that
 * was superseded is dropped.
 *
 * @typeParam T - What the request answers with.
 * @param load - The request, from `api.ts`.
 * @param key - What the request depends on; a new key asks again and forgets the old answer.
 * @returns The request's state.
 */
export function useRequest<T>(load: () => Promise<T>, key: string): RequestState<T> {
  const [held, setHeld] = useState<Held<T>>({ key, data: undefined, error: undefined, isPending: true });
  const [attempt, setAttempt] = useState(0);

  // biome-ignore lint/correctness/useExhaustiveDependencies: `key` and `attempt` are the triggers; `load` is new every render.
  useEffect(() => {
    let isCurrent = true;
    setHeld((last) =>
      last.key === key ? { ...last, isPending: true } : { key, data: undefined, error: undefined, isPending: true },
    );
    load().then(
      (data) => {
        if (isCurrent) {
          setHeld({ key, data, error: undefined, isPending: false });
        }
      },
      (error: unknown) => {
        if (isCurrent) {
          setHeld((last) => ({ key, data: last.key === key ? last.data : undefined, error, isPending: false }));
        }
      },
    );
    return () => {
      isCurrent = false;
    };
  }, [key, attempt]);

  const refetch = useCallback(() => setAttempt((n) => n + 1), []);

  const isCurrentKey = held.key === key;
  const data = isCurrentKey ? held.data : undefined;
  const error = isCurrentKey ? held.error : undefined;
  return {
    data,
    // Pending only while there is nothing to show: a refetch keeps the last answer on screen.
    isPending: data === undefined && (isCurrentKey === false || held.isPending),
    isError: data === undefined && error !== undefined,
    error,
    refetch,
  };
}

import { useCallback, useEffect, useState, useSyncExternalStore } from 'react';
import type { SysexClient, UnlockPrompt } from '../protocol/client.ts';

/** How often the countdown redraws. */
const TICK_MS = 250;
/** Milliseconds in a second. */
const MS_PER_SECOND = 1000;

/**
 * The unlock prompt (§4.9) of the connected device's client: set while a request that answered
 * LOCKED waits for the user to unlock the device.
 *
 * @param client - The connected device, or null.
 * @returns The prompt, or null.
 */
export function useUnlockPrompt(client: SysexClient | null): UnlockPrompt | null {
  const subscribe = useCallback(
    (onChange: () => void) => (client === null ? () => undefined : client.onUnlockPrompt(onChange)),
    [client],
  );
  const read = useCallback(() => client?.unlockPrompt() ?? null, [client]);
  return useSyncExternalStore(subscribe, read);
}

/**
 * Whole seconds until a time, redrawn while it counts down.
 *
 * @param endsAt - A `Date.now()` time, or null.
 * @returns The seconds left, 0 once it has passed.
 */
export function useSecondsLeft(endsAt: number | null): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (endsAt === null) {
      return;
    }
    setNow(Date.now());
    const timer = setInterval(() => setNow(Date.now()), TICK_MS);
    return () => clearInterval(timer);
  }, [endsAt]);
  return endsAt === null ? 0 : Math.max(0, Math.ceil((endsAt - now) / MS_PER_SECOND));
}

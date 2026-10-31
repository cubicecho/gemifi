import { useCallback, useSyncExternalStore } from 'react';

/** Fired on `window` after this page changes its own URL, since `pushState` fires nothing. */
const NAVIGATE_EVENT = 'gemifi:navigate';

/**
 * @param onChange - Called when the URL changes, by Back and Forward or by this page.
 * @returns The unsubscribe.
 */
function subscribe(onChange: () => void): () => void {
  window.addEventListener('popstate', onChange);
  window.addEventListener(NAVIGATE_EVENT, onChange);
  return () => {
    window.removeEventListener('popstate', onChange);
    window.removeEventListener(NAVIGATE_EVENT, onChange);
  };
}

/**
 * Sets search params as one history entry, so Back undoes them together.
 *
 * @param values - Each param's new value; null clears it rather than leaving it empty.
 */
export function setSearchParams(values: Readonly<Record<string, string | null>>): void {
  const url = new URL(window.location.href);
  for (const [name, value] of Object.entries(values)) {
    if (value === null) {
      url.searchParams.delete(name);
    } else {
      url.searchParams.set(name, value);
    }
  }
  const isSame = url.href === window.location.href;
  if (isSame) {
    return;
  }
  window.history.pushState(null, '', url);
  window.dispatchEvent(new Event(NAVIGATE_EVENT));
}

/**
 * One search param, read from the URL and kept in step with Back and Forward.
 *
 * @param name - The param.
 * @returns Its value (null when absent) and a setter that adds a history entry.
 */
export function useSearchParam(name: string): [string | null, (value: string | null) => void] {
  const value = useSyncExternalStore(subscribe, () => new URLSearchParams(window.location.search).get(name));
  const setValue = useCallback((next: string | null) => setSearchParams({ [name]: next }), [name]);
  return [value, setValue];
}

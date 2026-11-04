import { useEffect, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';

/**
 * Under `npm run dev`, with `?demo` in the URL: a pretend Code 61 to stand in for the connected
 * device. In a production build `import.meta.env.DEV` is false, so the demo is never loaded.
 *
 * @returns The demo's client, or null.
 */
export function useDemoClient(): SysexClient | null {
  const [client, setClient] = useState<SysexClient | null>(null);
  useEffect(() => {
    if (import.meta.env.DEV && new URLSearchParams(window.location.search).has('demo')) {
      void import('./demo-device.ts').then((demo) => setClient(demo.demoClient()));
    }
  }, []);
  return client;
}

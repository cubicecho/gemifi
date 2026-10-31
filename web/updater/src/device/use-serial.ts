import { useCallback, useState, useSyncExternalStore } from 'react';
import { SerialLink, type SerialUnavailable, serialUnavailableReason } from '../serial/serial-link.ts';

/** Where opening a serial port is. */
export type SerialAccess =
  | { kind: 'unavailable'; reason: SerialUnavailable }
  | { kind: 'idle' }
  | { kind: 'requesting' }
  | { kind: 'failed'; message: string };

/**
 * Owns the page's one serial link and re-renders on every change to it. Like the MIDI manager,
 * the link lives as long as the page.
 *
 * @returns The link, its snapshot, the request state, and `connect` / `disconnect`.
 */
export function useSerial() {
  const [link] = useState(() => new SerialLink());
  const snapshot = useSyncExternalStore(link.subscribe, link.getSnapshot);
  const [access, setAccess] = useState<SerialAccess>(() => {
    const reason = serialUnavailableReason();
    return reason === null ? { kind: 'idle' } : { kind: 'unavailable', reason };
  });

  const connect = useCallback(
    (baudRate: number) => {
      setAccess({ kind: 'requesting' });
      link.request(baudRate).then(
        () => setAccess({ kind: 'idle' }),
        (error: unknown) => {
          // Closing the browser's chooser is not an error worth showing.
          const isCancelled = error instanceof DOMException && error.name === 'NotFoundError';
          const message = error instanceof Error ? error.message : String(error);
          setAccess(isCancelled ? { kind: 'idle' } : { kind: 'failed', message });
        },
      );
    },
    [link],
  );
  const disconnect = useCallback(() => {
    void link.close();
  }, [link]);

  return { link, snapshot, access, connect, disconnect };
}

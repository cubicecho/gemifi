import { useCallback, useState, useSyncExternalStore } from 'react';
import {
  type ConnectionSnapshot,
  DeviceManager,
  type MidiUnavailable,
  midiUnavailableReason,
  requestSysexAccess,
} from '../midi/device-manager.ts';

/** Where asking for MIDI access is. */
export type AccessState =
  | { kind: 'unavailable'; reason: MidiUnavailable }
  | { kind: 'idle' }
  | { kind: 'requesting' }
  | { kind: 'denied'; message: string }
  | { kind: 'ready'; manager: DeviceManager };

/** A snapshot with nothing connected, for before access is granted. */
const EMPTY: ConnectionSnapshot = Object.freeze({ pairs: [], selected: null, client: null });

/** @returns A store that never changes, for before access is granted. */
const emptySubscribe = () => () => undefined;

/**
 * Owns the MIDI access request and the device manager, and re-renders on every port change.
 * The manager lives as long as the page: closing it in an effect cleanup would kill it under
 * StrictMode's double mount.
 *
 * @returns The access state, the current connection snapshot, and `connect` to ask for access.
 */
export function useConnection(): { access: AccessState; snapshot: ConnectionSnapshot; connect: () => void } {
  const [access, setAccess] = useState<AccessState>(() => {
    const reason = midiUnavailableReason();
    return reason === null ? { kind: 'idle' } : { kind: 'unavailable', reason };
  });
  const manager = access.kind === 'ready' ? access.manager : null;
  const snapshot = useSyncExternalStore(manager?.subscribe ?? emptySubscribe, manager?.getSnapshot ?? (() => EMPTY));

  const connect = useCallback(() => {
    setAccess({ kind: 'requesting' });
    requestSysexAccess().then(
      (midiAccess) => setAccess({ kind: 'ready', manager: new DeviceManager(midiAccess) }),
      (error: unknown) =>
        setAccess({ kind: 'denied', message: error instanceof Error ? error.message : String(error) }),
    );
  }, []);

  return { access, snapshot, connect };
}

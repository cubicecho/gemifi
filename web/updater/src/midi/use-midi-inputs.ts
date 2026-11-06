import { useEffect, useState } from 'react';
import type { DeviceManager } from './device-manager.ts';

/**
 * Every connected MIDI input, kept up to date as ports come and go.
 *
 * @param manager - The Web MIDI manager, or null before access is granted (and in the ?demo page).
 * @returns The inputs; empty without a manager.
 */
export function useMidiInputs(manager: DeviceManager | null): readonly MIDIInput[] {
  const [inputs, setInputs] = useState<readonly MIDIInput[]>(() => manager?.inputs() ?? []);
  useEffect(() => {
    if (manager === null) {
      setInputs([]);
      return;
    }
    setInputs(manager.inputs());
    return manager.subscribe(() => setInputs(manager.inputs()));
  }, [manager]);
  return inputs;
}

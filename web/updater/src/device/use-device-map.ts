import { useEffect, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';
import { type Component, listComponents } from '../protocol/feedback.ts';
import { type DeviceLayout, getLayout, offersLayout } from '../protocol/layout.ts';
import type { HelloInfo } from '../protocol/messages.ts';
import { Capability, Role } from '../protocol/wire.ts';

/** What the device says about its controls: where they are, and what they are. */
export interface DeviceMap {
  /** Still reading. */
  isLoading: boolean;
  /** The layout (caps bit 8), or null without one or when it could not be read. */
  layout: DeviceLayout | null;
  /** The components (caps bit 6: YAML ids, adc ranges), or null without them. */
  components: readonly Component[] | null;
  /** Why something could not be read, or null. */
  error: string | null;
}

const EMPTY: DeviceMap = { isLoading: false, layout: null, components: null, error: null };

/**
 * Reads the layout (GET_LAYOUT) and the components (GET_COMPONENT) once per device session.
 *
 * @param client - The connected device, or null.
 * @param info - Its HELLO, or null.
 * @returns What could be read.
 */
export function useDeviceMap(client: SysexClient | null, info: HelloInfo | null): DeviceMap {
  const [map, setMap] = useState<DeviceMap>(EMPTY);
  const hasLayout = offersLayout(info);
  const hasComponents = info !== null && info.role === Role.App && (info.caps & Capability.Feedback) !== 0;
  useEffect(() => {
    if (client === null || (hasLayout === false && hasComponents === false)) {
      setMap(EMPTY);
      return;
    }
    const controller = new AbortController();
    const { signal } = controller;
    setMap({ ...EMPTY, isLoading: true });
    (async () => {
      const errors: string[] = [];
      let layout: DeviceLayout | null = null;
      let components: Component[] | null = null;
      if (hasLayout) {
        try {
          layout = await getLayout(client, { signal });
        } catch (error) {
          errors.push(`The layout could not be read: ${String(error instanceof Error ? error.message : error)}`);
        }
      }
      if (hasComponents) {
        try {
          components = await listComponents(client, { signal });
        } catch (error) {
          errors.push(`The components could not be read: ${String(error instanceof Error ? error.message : error)}`);
        }
      }
      if (signal.aborted === false) {
        setMap({ isLoading: false, layout, components, error: errors.length === 0 ? null : errors.join(' ') });
      }
    })();
    return () => controller.abort();
  }, [client, hasLayout, hasComponents]);
  return map;
}

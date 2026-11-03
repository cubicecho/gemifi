/**
 * Finds the device files a connected device was built from, by the node name it reports in HELLO
 * (protocol/sysex.md §4.1.1).
 */

import { type BuilderDevice, ROOT_MINE } from './api.ts';

/**
 * @param devices - Every device file.
 * @param nodeName - What the connected device reports; null when it reports none.
 * @returns The files with that node name, the user's own before the presets.
 */
export function devicesWithNodeName(devices: readonly BuilderDevice[], nodeName: string | null): BuilderDevice[] {
  if (nodeName === null) {
    return [];
  }
  const matches = devices.filter((device) => device.nodeName === nodeName);
  const mine = matches.filter((device) => device.root === ROOT_MINE);
  const others = matches.filter((device) => device.root !== ROOT_MINE);
  return [...mine, ...others];
}

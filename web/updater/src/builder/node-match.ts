/**
 * Finds the device files a connected device was built from, by the node name it reports in HELLO
 * (protocol/sysex.md §4.1.1), and tells whether it runs a file's config, by its `config.hash`.
 */

import { type ParamValues, valueKey } from '../protocol/config.ts';
import type { Schema } from '../protocol/schema.ts';
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

/** The read-only param a device reports its config hash in (core/README.md "Config hash"). */
export const CONFIG_HASH_PARAM = 'config.hash';

/** How a device file compares with the config a connected device runs. */
export const ConfigMatch = {
  /** The device runs this file as it is now. */
  Same: 'same',
  /** The device runs this file's last build, and the file has changed since. */
  LastBuild: 'last-build',
  /** The device runs a different config. */
  Different: 'different',
  /** One side does not say (older firmware or builder, or a file that does not resolve). */
  Unknown: 'unknown',
} as const;
export type ConfigMatch = (typeof ConfigMatch)[keyof typeof ConfigMatch];

/**
 * @param schema - The connected device's schema.
 * @param values - Its values.
 * @returns Its `config.hash`, or null when its firmware has none (built before config hashes).
 */
export function runningConfigHash(schema: Schema, values: ParamValues): number | null {
  const param = schema.params.find((spec) => spec.name === CONFIG_HASH_PARAM);
  if (param === undefined) {
    return null;
  }
  return values.get(valueKey(param.id, 0)) ?? param.defaults[0] ?? null;
}

/**
 * @param device - A device file.
 * @param running - The connected device's `config.hash`, or null when it has none.
 * @returns How the two compare.
 */
export function configMatch(device: BuilderDevice, running: number | null): ConfigMatch {
  if (running === null || device.configHash === null) {
    return ConfigMatch.Unknown;
  }
  if (device.configHash === running) {
    return ConfigMatch.Same;
  }
  return device.builtConfigHash === running ? ConfigMatch.LastBuild : ConfigMatch.Different;
}

/**
 * @param hash - A config hash.
 * @returns It as 8 hex digits, as the builder prints it.
 */
export function formatConfigHash(hash: number): string {
  return hash.toString(HEX).padStart(HASH_DIGITS, '0');
}

const HEX = 16;
/** 31 bits, written as 8 hex digits. */
const HASH_DIGITS = 8;

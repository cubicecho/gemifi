import type { BuilderDevice, Platform } from './api.ts';

/** What each platform is called on screen. */
const PLATFORM_LABELS = Object.freeze({
  'nuvoton-nuc1xx': 'Nuvoton NUC1xx',
  esp32: 'ESP32',
  esp32s2: 'ESP32-S2',
  esp32s3: 'ESP32-S3',
  esp8266: 'ESP8266',
}) satisfies Readonly<Record<Platform, string>>;

/**
 * @param platform - A device's platform, or null when its file names none this page knows.
 * @returns What it is called on screen.
 */
export function platformLabel(platform: Platform | null): string {
  return platform === null ? 'Unknown platform' : PLATFORM_LABELS[platform];
}

/**
 * @param device - A device file.
 * @returns Its display name (the YAML's `friendly_name`, else its `name`), or its file name when the
 *   YAML gives neither.
 */
export function deviceTitle(device: BuilderDevice): string {
  return device.name === '' ? device.file : device.name;
}

/**
 * @param device - A preset's device file.
 * @returns The preset name `/api/new` takes for it: the file name without `.yaml`.
 */
export function presetNameOf(device: BuilderDevice): string {
  return device.file.replace(/\.ya?ml$/, '');
}

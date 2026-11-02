/**
 * What each file a build writes is for, so the builder can hand the right one to the Firmware card
 * (updates over SysEx) or the first-install card (the chip's ROM bootloader). Names follow
 * `tools/builder/gemifi_build.py`: `<stem>.gfw` and `<stem>.bin` on the NUC1xx; `<stem>.bin`,
 * `<stem>-factory.bin`, `<stem>-bootloader.bin` and `<stem>-partitions.bin` on ESP32 chips; one
 * `<stem>.bin` on the ESP8266.
 */

import { Platform } from './api.ts';

/** What a built file is for. */
export const ImageKind = {
  /** A `.gfw`, flashed through the gemifi bootloader. */
  Gfw: 'gfw',
  /** An ESP app, flashed to the slot the device is not running from. */
  OtaApp: 'ota-app',
  /** An ESP8266 image: the app update, and also the first install at 0x0. */
  Esp8266App: 'esp8266-app',
  /** An ESP32 merged image, written at 0x0 for the first install. */
  Factory: 'factory',
  /** A piece the factory image already holds: the second-stage bootloader or the partition table. */
  Part: 'part',
  /** The NUC1xx app without its header, for SWD or a debugger. */
  Raw: 'raw',
} as const;
export type ImageKind = (typeof ImageKind)[keyof typeof ImageKind];

/** What a kind of file can be used for, and the line that says so. */
interface ImageUse {
  /** It goes to the Firmware card. */
  canFlash: boolean;
  /** It goes to the first-install card. */
  canInstall: boolean;
  description: string;
}

/** Every kind's uses. */
export const IMAGE_USES = Object.freeze({
  [ImageKind.Gfw]: { canFlash: true, canInstall: false, description: 'Firmware for the gemifi bootloader' },
  [ImageKind.OtaApp]: {
    canFlash: true,
    canInstall: false,
    description: 'App update, for a chip already running gemifi',
  },
  [ImageKind.Esp8266App]: {
    canFlash: true,
    canInstall: true,
    description: 'App update, and the first install at 0x0',
  },
  [ImageKind.Factory]: { canFlash: false, canInstall: true, description: 'Merged image for the first install at 0x0' },
  [ImageKind.Part]: { canFlash: false, canInstall: false, description: 'Already inside the factory image' },
  [ImageKind.Raw]: { canFlash: false, canInstall: false, description: 'Raw app, for SWD or a debugger' },
}) satisfies Readonly<Record<ImageKind, ImageUse>>;

/** Suffixes of the ESP32 parts the factory image already holds. */
const PART_SUFFIXES = ['-bootloader.bin', '-partitions.bin'] as const;
/** Suffix of the ESP32 merged image. */
const FACTORY_SUFFIX = '-factory.bin';

/**
 * @param platform - The device's platform.
 * @param file - A built file's name.
 * @returns What the file is for, or null for a file this page does not know.
 */
export function imageKind(platform: Platform, file: string): ImageKind | null {
  const name = file.toLowerCase();
  if (name.endsWith('.gfw')) {
    return ImageKind.Gfw;
  }
  const isBin = name.endsWith('.bin');
  if (isBin === false) {
    return null;
  }
  if (platform === Platform.Nuc1xx || platform === Platform.Stm32f1) {
    return ImageKind.Raw;
  }
  if (platform === Platform.Esp8266) {
    return ImageKind.Esp8266App;
  }
  if (name.endsWith(FACTORY_SUFFIX)) {
    return ImageKind.Factory;
  }
  if (PART_SUFFIXES.some((suffix) => name.endsWith(suffix))) {
    return ImageKind.Part;
  }
  return ImageKind.OtaApp;
}

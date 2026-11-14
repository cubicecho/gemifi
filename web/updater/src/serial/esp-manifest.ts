/**
 * What to write for a first install through an ESP chip's ROM bootloader: one merged factory image
 * at 0x0 (what PlatformIO's or esptool's `merge_bin` makes), or a manifest in the ESP Web Tools
 * format, `{ name, version, builds: [{ chipFamily, parts: [{ path, offset }] }] }`, with its part
 * files picked alongside it. Framework-free; nothing here talks to a chip.
 */

/** One part of a build: a file and where it goes in flash. */
export interface ManifestPart {
  path: string;
  offset: number;
}

/** One build: the parts for one chip family. */
export interface ManifestBuild {
  /** As ESP Web Tools writes it: `ESP32`, `ESP32-S2`, `ESP32-S3`… */
  chipFamily: string;
  parts: ManifestPart[];
}

/** An ESP Web Tools manifest. */
export interface EspManifest {
  name: string;
  version: string;
  builds: ManifestBuild[];
}

/** What reading a manifest found. */
export type ManifestResult = { ok: true; manifest: EspManifest } | { ok: false; problem: string };

/** One file and its flash address, ready for esptool. */
export interface FlashPart {
  name: string;
  address: number;
  data: Uint8Array;
}

/** A picked file. */
export interface PickedBytes {
  name: string;
  bytes: Uint8Array;
}

/**
 * @param value - Anything.
 * @returns Whether it is a plain object.
 */
function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && Array.isArray(value) === false;
}

/**
 * Reads an offset: a number, or a string in decimal or `0x` hex (some manifests write `"0x10000"`).
 *
 * @param value - The manifest's `offset`.
 * @returns The offset, or null.
 */
function readOffset(value: unknown): number | null {
  const n = typeof value === 'string' ? Number(value.trim()) : value;
  const isOffset = typeof n === 'number' && Number.isInteger(n) && n >= 0;
  return isOffset ? n : null;
}

/**
 * Parses and checks a manifest.
 *
 * @param text - The `manifest.json` text.
 * @returns The manifest, or what is wrong with it.
 */
export function parseManifest(text: string): ManifestResult {
  let json: unknown;
  try {
    json = JSON.parse(text);
  } catch (error) {
    return { ok: false, problem: `It is not JSON: ${error instanceof Error ? error.message : String(error)}` };
  }
  if (isRecord(json) === false || Array.isArray(json.builds) === false) {
    return { ok: false, problem: 'It has no "builds" list, so it is not an ESP Web Tools manifest.' };
  }
  const builds: ManifestBuild[] = [];
  for (const [i, build] of (json.builds as unknown[]).entries()) {
    if (isRecord(build) === false || typeof build.chipFamily !== 'string' || Array.isArray(build.parts) === false) {
      return { ok: false, problem: `Build ${i + 1} needs a "chipFamily" and a "parts" list.` };
    }
    const parts: ManifestPart[] = [];
    for (const part of build.parts as unknown[]) {
      const offset = isRecord(part) ? readOffset(part.offset) : null;
      if (isRecord(part) === false || typeof part.path !== 'string' || offset === null) {
        return { ok: false, problem: `A part of the ${build.chipFamily} build needs a "path" and an "offset".` };
      }
      parts.push({ path: part.path, offset });
    }
    if (parts.length === 0) {
      return { ok: false, problem: `The ${build.chipFamily} build has no parts.` };
    }
    builds.push({ chipFamily: build.chipFamily, parts });
  }
  if (builds.length === 0) {
    return { ok: false, problem: 'The manifest lists no builds.' };
  }
  const name = typeof json.name === 'string' ? json.name : 'firmware';
  const version = typeof json.version === 'string' ? json.version : '';
  return { ok: true, manifest: { name, version, builds } };
}

/**
 * Normalises a chip name for matching: `esp32-s3`, `ESP32S3` and `ESP32-S3` are one family.
 *
 * @param name - A manifest `chipFamily` or esptool's `CHIP_NAME`.
 * @returns The key.
 */
function familyKey(name: string): string {
  return name.toUpperCase().replace(/[^A-Z0-9]/g, '');
}

/**
 * Picks the build for the chip esptool found.
 *
 * @param manifest - The manifest.
 * @param chipName - esptool's `CHIP_NAME`, e.g. `ESP32-S3`.
 * @returns The build, or null when the manifest has none for that chip.
 */
export function selectBuild(manifest: EspManifest, chipName: string): ManifestBuild | null {
  const key = familyKey(chipName);
  return manifest.builds.find((build) => familyKey(build.chipFamily) === key) ?? null;
}

/**
 * @param path - A manifest path or a file name.
 * @returns Its last segment.
 */
function baseName(path: string): string {
  return path.split(/[\\/]/).at(-1) ?? path;
}

/**
 * Matches a build's parts to the files picked with the manifest, by file name.
 *
 * @param build - The chosen build.
 * @param files - The picked files.
 * @returns The parts in manifest order, or the file names that were not picked.
 */
export function resolveParts(
  build: ManifestBuild,
  files: readonly PickedBytes[],
): { ok: true; parts: FlashPart[] } | { ok: false; missing: string[] } {
  const byName = new Map(files.map((file) => [baseName(file.name), file]));
  const parts: FlashPart[] = [];
  const missing: string[] = [];
  for (const part of build.parts) {
    const name = baseName(part.path);
    const file = byName.get(name);
    if (file === undefined) {
      missing.push(name);
    } else {
      parts.push({ name, address: part.offset, data: file.bytes });
    }
  }
  return missing.length > 0 ? { ok: false, missing } : { ok: true, parts };
}

/** An ESP image header (`esp_image_header_t`) starts with this byte. */
const IMAGE_MAGIC = 0xe9;
/** Where the header's `chip_id` (u16, little-endian) is. */
const CHIP_ID_OFFSET = 12;
/** Where a merged image's bootloader starts on the S3, C3 and later. */
const BOOTLOADER_AT_0 = 0x0;
/** … on the ESP32 and S2 (the bytes before it are 0xFF). */
const BOOTLOADER_AT_1000 = 0x1000;
/** … on the C5 and P4. */
const BOOTLOADER_AT_2000 = 0x2000;
const BOOTLOADER_OFFSETS = [BOOTLOADER_AT_0, BOOTLOADER_AT_1000, BOOTLOADER_AT_2000] as const;

/** The header's `chip_id` (esptool's `IMAGE_CHIP_ID`) for each chip. */
const CHIP_NAMES: Readonly<Record<number, string>> = {
  0: 'ESP32',
  2: 'ESP32-S2',
  5: 'ESP32-C3',
  9: 'ESP32-S3',
  12: 'ESP32-C2',
  13: 'ESP32-C6',
  16: 'ESP32-H2',
  18: 'ESP32-P4',
  20: 'ESP32-C61',
  23: 'ESP32-C5',
};

/**
 * Finds which chip a part was built for, from its first ESP image header: the part's start, or for a
 * part written at 0x0 (a merged image) the first bootloader offset that has one.
 *
 * @param part - A part.
 * @returns The header's `chip_id`, or null when the part has no image header (a partition table,
 * OTA data, or not an ESP image).
 */
export function imageChipId(part: FlashPart): number | null {
  const offsets = part.address === 0 ? BOOTLOADER_OFFSETS : [0];
  for (const offset of offsets) {
    if (part.data.length >= offset + CHIP_ID_OFFSET + 2 && part.data[offset] === IMAGE_MAGIC) {
      return (part.data[offset + CHIP_ID_OFFSET] ?? 0) | ((part.data[offset + CHIP_ID_OFFSET + 1] ?? 0) << 8);
    }
  }
  return null;
}

/**
 * Checks that every part with an ESP image header was built for the chip esptool found, as esptool's
 * own `write_flash` does (esptool-js doesn't): a wrong one only fails once it is written and booted.
 *
 * @param parts - What would be written.
 * @param chipId - The chip's `IMAGE_CHIP_ID`; undefined skips the check (the ESP8266 has none).
 * @param chipName - esptool's `CHIP_NAME`, for the message.
 * @returns What is wrong, or null.
 */
export function checkPartsChip(
  parts: readonly FlashPart[],
  chipId: number | undefined,
  chipName: string,
): string | null {
  if (chipId === undefined) {
    return null;
  }
  for (const part of parts) {
    const built = imageChipId(part);
    if (built !== null && built !== chipId) {
      const builtName = CHIP_NAMES[built] ?? `chip ID ${built}`;
      return `${part.name} is built for an ${builtName}, but this chip is an ${chipName}. Nothing was written: pick the ${chipName} build.`;
    }
  }
  return null;
}

/** What the picked files are. */
export type InstallPlan =
  | { kind: 'merged'; part: FlashPart }
  | { kind: 'manifest'; manifest: EspManifest; files: PickedBytes[] }
  | { kind: 'invalid'; problem: string };

/**
 * Works out what was picked: one `.bin` (a merged image, written at 0x0), or a `manifest.json`
 * with its part files.
 *
 * @param files - The files, `bytes` read.
 * @returns The plan.
 */
export function planInstall(files: readonly PickedBytes[]): InstallPlan {
  const manifests = files.filter((file) => file.name.toLowerCase().endsWith('.json'));
  if (manifests.length > 1) {
    return { kind: 'invalid', problem: 'Pick one manifest.json at a time.' };
  }
  const [manifestFile] = manifests;
  if (manifestFile !== undefined) {
    const result = parseManifest(new TextDecoder().decode(manifestFile.bytes));
    if (result.ok === false) {
      return { kind: 'invalid', problem: `${manifestFile.name}: ${result.problem}` };
    }
    return { kind: 'manifest', manifest: result.manifest, files: files.filter((file) => file !== manifestFile) };
  }
  const [single, ...rest] = files;
  if (single === undefined) {
    return { kind: 'invalid', problem: 'Pick a merged .bin, or a manifest.json and its files.' };
  }
  if (rest.length > 0) {
    return {
      kind: 'invalid',
      problem:
        'Several .bin files need a manifest.json saying where each goes. Pick one merged image, or add the manifest.',
    };
  }
  if (single.bytes.length === 0) {
    return { kind: 'invalid', problem: `${single.name} is empty.` };
  }
  return { kind: 'merged', part: { name: single.name, address: 0, data: single.bytes } };
}

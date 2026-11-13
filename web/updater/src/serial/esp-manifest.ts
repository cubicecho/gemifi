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

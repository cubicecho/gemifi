/**
 * The typed client for the gemifi builder API (`tools/builder/dashboard.py`), served on the same
 * origin as this page under `api/`. Every request in the builder UI goes through here; components
 * never call `fetch` themselves. Responses are checked before they are trusted.
 */

/** The chip families a device file can target. */
export const Platform = {
  Nuc1xx: 'nuvoton-nuc1xx',
  Stm32f1: 'stm32f1',
  Esp32: 'esp32',
  Esp32S2: 'esp32s2',
  Esp32S3: 'esp32s3',
} as const;
export type Platform = (typeof Platform)[keyof typeof Platform];

/** The toolchains a builder can have installed. */
export const Toolchain = { Nuc1xx: 'nuvoton-nuc1xx', Stm32f1: 'stm32f1', Esp: 'esp' } as const;
export type Toolchain = (typeof Toolchain)[keyof typeof Toolchain];

/** Which toolchain builds each platform. */
export const PLATFORM_TOOLCHAIN = Object.freeze({
  [Platform.Nuc1xx]: Toolchain.Nuc1xx,
  [Platform.Stm32f1]: Toolchain.Stm32f1,
  [Platform.Esp32]: Toolchain.Esp,
  [Platform.Esp32S2]: Toolchain.Esp,
  [Platform.Esp32S3]: Toolchain.Esp,
}) satisfies Readonly<Record<Platform, Toolchain>>;

/** The device files shipped with gemifi, usually read-only. */
export const ROOT_PRESETS = 'presets' as const;
/** The user's own device files. */
export const ROOT_MINE = 'mine' as const;
export type DeviceRoot = typeof ROOT_PRESETS | typeof ROOT_MINE;

/** The `name` `/api/info` answers with; anything else is not a gemifi builder. */
export const BUILDER_NAME = 'gemifi builder';

/** What `/api/info` says about the builder. */
export interface BuilderInfo {
  name: string;
  version: string;
  /** Where new devices are written. */
  userDir: string;
  /** Which toolchains are installed. */
  toolchains: Readonly<Record<Toolchain, boolean>>;
}

/** One device file. */
export interface BuilderDevice {
  /** `presets/x.yaml` or `mine/x.yaml`. */
  id: string;
  root: DeviceRoot;
  file: string;
  /** The YAML's `friendly_name`, else its `name` (the node name); empty when it gives neither. */
  name: string;
  /** The YAML's `name:`, which its firmware reports in HELLO; empty when it is missing or not a valid one. */
  nodeName: string;
  /** Null when the file names no platform this page knows, as a half-edited file may. */
  platform: Platform | null;
  description: string;
  /** The file names of the last build's output; empty when it has not been built. */
  images: string[];
  /** Built, but the file has changed since: the images are not this file's any more. */
  isStale: boolean;
  writable: boolean;
  /**
   * The file's config hash as it is now (what param `config.hash` of firmware built from it reports),
   * or null when it does not resolve, or the builder is older.
   */
  configHash: number | null;
  /** The config hash of the last build's images, or null when there is none. */
  builtConfigHash: number | null;
}

/** The device list. */
export interface DeviceListing {
  devices: BuilderDevice[];
  userDir: string;
}

/** A preset a new device can start from. */
export interface BuilderPreset {
  name: string;
  platform: Platform | null;
  description: string;
}

/** How serious a diagnostic is. */
export type DiagnosticLevel = 'error' | 'warning';

/** One problem the validator found. */
export interface Diagnostic {
  level: DiagnosticLevel;
  /** Where in the YAML tree, e.g. `controls[3].note`. */
  path: string;
  /** The 1-based line, or null when the validator could not place it. */
  line: number | null;
  /** The file the line is in when it is another one (a package or `!include`), else null. */
  file: string | null;
  msg: string;
}

/** One preset slot as the validator read it. */
export interface PresetSlot {
  /** 1-based. */
  slot: number;
  name: string;
  /** The Program Change it sends when selected, or null. */
  program: number | null;
  /** How many values it changes from slot 1. */
  changes: number;
  /** Listed in the YAML; false for an empty slot that starts as a copy of slot 1. */
  listed: boolean;
}

/** The device's presets as the validator read them. */
export interface PresetsSummary {
  slots: PresetSlot[];
  /** Whether the file has a `presets:` section. */
  given: boolean;
  /** The largest `slots` the platform's store fits, or null when the builder did not say. */
  maxSlots: number | null;
}

/** One DMA channel the builder gave to a feature (stm32f1). */
export interface DmaChannel {
  /** DMA1 channel, 1..7. */
  channel: number;
  /** The peripheral request on it, e.g. `USART2_TX`. */
  request: string;
  /** The feature that uses it, e.g. `rgb_serial 'pad_leds'`. */
  user: string;
}

/** The DMA1 channel map (core/README.md "DMA channels"). */
export interface DmaMap {
  channels: DmaChannel[];
  /** The channels nothing uses. */
  free: number[];
}

/** How the DIN MIDI jacks reach USB. */
export const DinMode = { SeparatePort: 'separate_port', UsbMerge: 'usb_merge', DinOnly: 'din_only' } as const;
export type DinMode = (typeof DinMode)[keyof typeof DinMode];

/** `din_midi:` as the validator read it. */
export interface DinMidiSummary {
  mode: DinMode;
  /** Whether it has an `rx_pin` (MIDI In). */
  midiIn: boolean;
  thru: boolean;
  controllerOut: boolean;
  usbToDin: boolean;
  dma: boolean;
}

/** What a second USB-MIDI port carries. */
export type MidiPortRole = 'daw' | 'din';

/** A USB-MIDI port the device shows besides its own. */
export interface MidiPort {
  /** The USB-MIDI cable number. */
  cable: number;
  name: string;
  role: MidiPortRole;
}

/** What validating a device file found. */
export interface ValidationResult {
  ok: boolean;
  /** The one-line summary: name, platform, component counts, the preset budget. */
  summary: string;
  diags: Diagnostic[];
  /** Null when the file is not valid, or from an older builder. */
  presets: PresetsSummary | null;
  /** Null on a platform without a DMA map (only stm32f1 has one), on an invalid file, or from an older builder. */
  dma: DmaMap | null;
  /** Null when the file has no `din_midi:`. */
  dinMidi: DinMidiSummary | null;
  /** The ports besides the device's own; empty when it has none. */
  midiPorts: MidiPort[];
  /** The checked text's config hash (param `config.hash`); null when it is not valid, or from an older builder. */
  configHash: number | null;
}

/** How a build ended. */
export interface BuildResult {
  /** The build's exit code, or null when the stream ended without one. */
  exitCode: number | null;
  log: string;
}

/** A request the builder refused, or an answer that was not what this page expects. */
export class BuilderError extends Error {
  /** The HTTP status, or null when the answer arrived but could not be read. */
  readonly status: number | null;

  /**
   * @param message - What went wrong, in the reader's terms.
   * @param status - The HTTP status, or null.
   */
  constructor(message: string, status: number | null) {
    super(message);
    this.name = 'BuilderError';
    this.status = status;
  }
}

/** The HTTP status of a build refused because another one is running. */
export const STATUS_BUILD_RUNNING = 409;

/** The line a build log ends with: `[exit N]`. */
const EXIT_LINE = /^\[exit (-?\d+)\]\s*$/;

type JsonRecord = Record<string, unknown>;

/**
 * @param value - Anything.
 * @returns Whether it is a plain object.
 */
function isRecord(value: unknown): value is JsonRecord {
  return typeof value === 'object' && value !== null && Array.isArray(value) === false;
}

/**
 * @param value - Anything.
 * @returns Whether it is one of the known platforms.
 */
function isPlatform(value: unknown): value is Platform {
  return Object.values<unknown>(Platform).includes(value);
}

/**
 * @param what - What was being read, for the error.
 * @param detail - What was wrong with it.
 * @returns The error to throw.
 */
function badAnswer(what: string, detail: string): BuilderError {
  return new BuilderError(`The builder's answer for ${what} was not understood: ${detail}.`, null);
}

/**
 * @param record - A JSON object.
 * @param key - The field.
 * @param what - What is being read, for the error.
 * @returns The field, which must be a string.
 */
function readString(record: JsonRecord, key: string, what: string): string {
  const value = record[key];
  if (typeof value !== 'string') {
    throw badAnswer(what, `"${key}" is not text`);
  }
  return value;
}

/**
 * @param record - A JSON object.
 * @param key - The field.
 * @param what - What is being read, for the error.
 * @returns The field, which must be a boolean.
 */
function readBoolean(record: JsonRecord, key: string, what: string): boolean {
  const value = record[key];
  if (typeof value !== 'boolean') {
    throw badAnswer(what, `"${key}" is not true or false`);
  }
  return value;
}

/**
 * @param record - A JSON object.
 * @param key - The field.
 * @param what - What is being read, for the error.
 * @returns The field, which must be a list.
 */
function readList(record: JsonRecord, key: string, what: string): unknown[] {
  const value = record[key];
  if (Array.isArray(value) === false) {
    throw badAnswer(what, `"${key}" is not a list`);
  }
  return value;
}

/**
 * The builder reads a platform without validating the file, so a file being edited can say `?`.
 *
 * @param record - A JSON object.
 * @returns Its `platform`, or null when it is not one this page knows.
 */
function readPlatform(record: JsonRecord): Platform | null {
  return isPlatform(record.platform) ? record.platform : null;
}

/**
 * @param value - One entry of a list.
 * @param what - What is being read, for the error.
 * @returns It, which must be an object.
 */
function asRecord(value: unknown, what: string): JsonRecord {
  if (isRecord(value) === false) {
    throw badAnswer(what, 'an entry is not an object');
  }
  return value;
}

/**
 * Reads a failed response's `{"error": "..."}`, or says what the status was.
 *
 * @param response - A response that is not ok.
 * @returns The error to throw.
 */
async function refusal(response: Response): Promise<BuilderError> {
  let message = `The builder answered ${response.status} ${response.statusText}`.trim();
  try {
    const body: unknown = await response.json();
    if (isRecord(body) && typeof body.error === 'string') {
      message = body.error;
    }
  } catch {
    // Not JSON: keep the status line.
  }
  return new BuilderError(message, response.status);
}

/**
 * @param path - The API path, without the leading `api/`.
 * @param init - The request.
 * @returns The response, once it is known to be ok.
 */
async function request(path: string, init?: RequestInit): Promise<Response> {
  let response: Response;
  try {
    response = await fetch(`api/${path}`, init);
  } catch (error) {
    const reason = error instanceof Error ? error.message : String(error);
    throw new BuilderError(`The builder could not be reached (${reason}). Check that it is still running.`, null);
  }
  if (response.ok === false) {
    throw await refusal(response);
  }
  return response;
}

/**
 * @param path - The API path, without the leading `api/`.
 * @param body - The JSON body, for a POST; left out for a GET.
 * @returns The parsed JSON object.
 */
async function requestJson(path: string, body?: JsonRecord): Promise<JsonRecord> {
  const init: RequestInit | undefined =
    body === undefined
      ? undefined
      : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) };
  const response = await request(path, init);
  let json: unknown;
  try {
    json = await response.json();
  } catch {
    throw badAnswer(path, 'it is not JSON');
  }
  if (isRecord(json) === false) {
    throw badAnswer(path, 'it is not an object');
  }
  return json;
}

/**
 * Asks whether this page is served by a gemifi builder. On GitHub Pages or any static host the
 * request 404s or answers with HTML, and the answer is null.
 *
 * @returns The builder's facts, or null when there is no builder here.
 */
export async function fetchBuilderInfo(): Promise<BuilderInfo | null> {
  let json: JsonRecord;
  try {
    json = await requestJson('info');
  } catch {
    return null;
  }
  const isBuilder = json.name === BUILDER_NAME;
  if (isBuilder === false) {
    return null;
  }
  const what = 'the builder info';
  const toolchains: JsonRecord = isRecord(json.toolchains) ? json.toolchains : {};
  return {
    name: BUILDER_NAME,
    version: typeof json.version === 'string' ? json.version : '',
    userDir: readString(json, 'user_dir', what),
    toolchains: {
      [Toolchain.Nuc1xx]: toolchains[Toolchain.Nuc1xx] === true,
      [Toolchain.Stm32f1]: toolchains[Toolchain.Stm32f1] === true,
      [Toolchain.Esp]: toolchains[Toolchain.Esp] === true,
    },
  };
}

/**
 * @param value - One entry of `/api/devices`.
 * @returns The device.
 */
function parseDevice(value: unknown): BuilderDevice {
  const what = 'the device list';
  const record = asRecord(value, what);
  const root = record.root;
  if (root !== ROOT_PRESETS && root !== ROOT_MINE) {
    throw badAnswer(what, `"${String(root)}" is not a device folder`);
  }
  const listed = readList(record, 'images', what);
  const images = listed.filter((name): name is string => typeof name === 'string');
  if (images.length !== listed.length) {
    throw badAnswer(what, 'an image name is not text');
  }
  return {
    id: readString(record, 'id', what),
    root,
    file: readString(record, 'file', what),
    name: readString(record, 'name', what),
    // Optional: an older builder does not send it, and nothing then matches a connected device.
    nodeName: typeof record.node_name === 'string' ? record.node_name : '',
    platform: readPlatform(record),
    description: typeof record.description === 'string' ? record.description : '',
    images,
    // Optional: an older builder does not say, and its images are then taken as current.
    isStale: record.stale === true,
    writable: readBoolean(record, 'writable', what),
    // Optional: an older builder does not send them, and nothing then compares configs.
    configHash: isInteger(record.config_hash) ? record.config_hash : null,
    builtConfigHash: isInteger(record.built_config_hash) ? record.built_config_hash : null,
  };
}

/** @returns Every device file, presets and the user's own. */
export async function listDevices(): Promise<DeviceListing> {
  const json = await requestJson('devices');
  return {
    devices: readList(json, 'devices', 'the device list').map(parseDevice),
    userDir: typeof json.user_dir === 'string' ? json.user_dir : '',
  };
}

/** @returns The presets a new device can start from. */
export async function listPresets(): Promise<BuilderPreset[]> {
  const json = await requestJson('presets');
  const what = 'the preset list';
  return readList(json, 'presets', what).map((value) => {
    const record = asRecord(value, what);
    return {
      name: readString(record, 'name', what),
      platform: readPlatform(record),
      description: typeof record.description === 'string' ? record.description : '',
    };
  });
}

/**
 * @param id - The device's id.
 * @returns The device file's YAML.
 */
export async function readDeviceFile(id: string): Promise<string> {
  const json = await requestJson(`file?id=${encodeURIComponent(id)}`);
  return readString(json, 'text', 'the device file');
}

/**
 * @param value - One entry of `diags`.
 * @returns The diagnostic.
 */
function parseDiagnostic(value: unknown): Diagnostic {
  const what = 'the check';
  const record = asRecord(value, what);
  const line = record.line;
  const isPlaced = typeof line === 'number' && Number.isInteger(line) && line > 0;
  return {
    level: record.level === 'warning' ? 'warning' : 'error',
    path: typeof record.path === 'string' ? record.path : '',
    line: isPlaced ? line : null,
    file: typeof record.file === 'string' && record.file !== '' ? record.file : null,
    msg: readString(record, 'msg', what),
  };
}

/**
 * @param value - Anything.
 * @returns Whether it is a whole number.
 */
function isInteger(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value);
}

/**
 * The extra facts are optional (an older builder does not send them), so a missing or malformed one
 * reads as null rather than failing the whole check.
 *
 * @param value - The answer's `presets`.
 * @returns The presets, or null.
 */
function parsePresets(value: unknown): PresetsSummary | null {
  if (isRecord(value) === false || Array.isArray(value.slots) === false) {
    return null;
  }
  const slots = value.slots.filter(isRecord).map(
    (slot): PresetSlot => ({
      slot: isInteger(slot.slot) ? slot.slot : 0,
      name: typeof slot.name === 'string' ? slot.name : '',
      program: isInteger(slot.program) ? slot.program : null,
      changes: isInteger(slot.changes) ? slot.changes : 0,
      listed: slot.listed !== false,
    }),
  );
  const storage = isRecord(value.storage) ? value.storage : {};
  return { slots, given: value.given === true, maxSlots: isInteger(storage.max_slots) ? storage.max_slots : null };
}

/**
 * @param value - The answer's `dma`.
 * @returns The channel map, or null.
 */
function parseDma(value: unknown): DmaMap | null {
  if (isRecord(value) === false || Array.isArray(value.channels) === false) {
    return null;
  }
  const channels = value.channels
    .filter(isRecord)
    .filter((entry) => isInteger(entry.channel))
    .map(
      (entry): DmaChannel => ({
        channel: entry.channel as number,
        request: typeof entry.request === 'string' ? entry.request : '',
        user: typeof entry.user === 'string' ? entry.user : '',
      }),
    );
  const free = Array.isArray(value.free) ? value.free.filter(isInteger) : [];
  return { channels, free };
}

/**
 * @param value - Anything.
 * @returns Whether it is a DIN MIDI mode.
 */
function isDinMode(value: unknown): value is DinMode {
  return Object.values<unknown>(DinMode).includes(value);
}

/**
 * @param value - The answer's `din_midi`.
 * @returns The DIN MIDI settings, or null.
 */
function parseDinMidi(value: unknown): DinMidiSummary | null {
  if (isRecord(value) === false || isDinMode(value.mode) === false) {
    return null;
  }
  return {
    mode: value.mode,
    midiIn: value.midi_in === true,
    thru: value.thru === true,
    controllerOut: value.controller_out !== false,
    usbToDin: value.usb_to_din === true,
    dma: value.dma === true,
  };
}

/**
 * @param value - The answer's `midi_ports`.
 * @returns The extra ports; empty when there are none or the answer has no list.
 */
function parseMidiPorts(value: unknown): MidiPort[] {
  if (Array.isArray(value) === false) {
    return [];
  }
  return value
    .filter(isRecord)
    .filter((port) => port.role === 'daw' || port.role === 'din')
    .map(
      (port): MidiPort => ({
        cable: isInteger(port.cable) ? port.cable : 1,
        name: typeof port.name === 'string' ? port.name : '',
        role: port.role as MidiPortRole,
      }),
    );
}

/**
 * Checks a device file without saving it.
 *
 * @param text - The YAML.
 * @param id - The device it belongs to, so messages name its file.
 * @returns Whether it is valid, a one-line summary, the diagnostics, and what the builder read.
 */
export async function validateDevice(text: string, id?: string): Promise<ValidationResult> {
  const json = await requestJson('validate', id === undefined ? { text } : { id, text });
  const what = 'the check';
  const dma = parseDma(json.dma);
  const fullSummary = typeof json.summary === 'string' ? json.summary : '';
  // The summary's second line is the DMA map in text; with the map itself, it is drawn from that instead.
  const summary = dma === null ? fullSummary : (fullSummary.split('\n')[0] ?? '').trim();
  return {
    ok: readBoolean(json, 'ok', what),
    summary,
    diags: readList(json, 'diags', what).map(parseDiagnostic),
    presets: parsePresets(json.presets),
    dma,
    dinMidi: parseDinMidi(json.din_midi),
    midiPorts: parseMidiPorts(json.midi_ports),
    configHash: isInteger(json.config_hash) ? json.config_hash : null,
  };
}

/**
 * @param id - The device's id; it must be writable.
 * @param text - The YAML to write.
 */
export async function saveDevice(id: string, text: string): Promise<void> {
  await requestJson('save', { id, text });
}

/**
 * @param name - The new device's name.
 * @param preset - The preset it starts from.
 * @returns The new device's id.
 */
export async function createDevice(name: string, preset: string): Promise<string> {
  const json = await requestJson('new', { name, preset });
  return readString(json, 'id', 'the new device');
}

/** @param id - The device to delete; only the user's own can be. */
export async function deleteDevice(id: string): Promise<void> {
  await requestJson('delete', { id });
}

/**
 * Finds the exit code at the end of a build log.
 *
 * @param log - The whole log so far.
 * @returns The code from its last `[exit N]` line, or null when it has none.
 */
export function parseBuildExit(log: string): number | null {
  const lines = log.trimEnd().split('\n');
  const match = EXIT_LINE.exec(lines.at(-1) ?? '');
  return match === null ? null : Number(match[1]);
}

/** What {@link runBuild} needs. */
export interface BuildOptions {
  /** Called with the whole log each time more of it arrives. */
  onLog: (log: string) => void;
  signal?: AbortSignal | undefined;
}

/**
 * Builds a device and streams its log.
 *
 * @param id - The device to build, as saved.
 * @param options - The log callback and an abort signal.
 * @returns The exit code and the whole log.
 */
export async function runBuild(id: string, options: BuildOptions): Promise<BuildResult> {
  const response = await request('build', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id }),
    signal: options.signal,
  });
  if (response.body === null) {
    throw badAnswer('the build', 'it has no body');
  }
  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let log = '';
  for (;;) {
    const { done, value } = await reader.read();
    if (done) {
      break;
    }
    log += decoder.decode(value, { stream: true });
    options.onLog(log);
  }
  log += decoder.decode();
  options.onLog(log);
  return { exitCode: parseBuildExit(log), log };
}

/**
 * @param id - The device.
 * @param file - One of its `images`.
 * @returns The URL that serves that file, for a download link.
 */
export function imageUrl(id: string, file: string): string {
  return `api/image?id=${encodeURIComponent(id)}&file=${encodeURIComponent(file)}`;
}

/**
 * @param id - The device.
 * @param file - One of its `images`.
 * @returns The file's bytes.
 */
export async function fetchImage(id: string, file: string): Promise<Uint8Array> {
  const response = await request(`image?id=${encodeURIComponent(id)}&file=${encodeURIComponent(file)}`);
  return new Uint8Array(await response.arrayBuffer());
}

/**
 * The config schema (`protocol/sysex.md` §4.4): the little-endian blob GET_SCHEMA reads, which
 * says which params a device has, their types, ranges, defaults and labels. Pure: bytes in, a
 * description out.
 */

import { DecodeError } from './encoding.ts';
import { ParamFlag, ParamType, SCHEMA_VERSION } from './wire.ts';

/** Thrown when schema bytes are truncated or not a layout this module can read. */
export class SchemaError extends DecodeError {
  override name = 'SchemaError';
}

/** One param as the schema describes it. */
export interface ParamSpec {
  /** Stable id, 1..0x3FFF. */
  id: number;
  /** How to show it. An unknown type on the wire is `int` here; {@link ParamSpec.rawType} keeps it. */
  type: ParamType;
  /** The `type` byte as sent. */
  rawType: number;
  /** Index into {@link Schema.groups}. */
  group: number;
  /** How many values: 1 for a plain param, more for an array (index 0..count-1). */
  count: number;
  /** The flag bits as sent; unknown ones are kept but mean nothing here. */
  flags: number;
  /** Smallest value. */
  min: number;
  /** Largest value. */
  max: number;
  /** The default for each index (`count` entries, the one default repeated when not per index). */
  defaults: number[];
  /** Stable machine key, e.g. `midi.channel`. */
  name: string;
  /** Display text, e.g. `MIDI channel`. */
  label: string;
  /** Unit shown beside the value, e.g. `ms`; null when the schema gives none. */
  unit: string | null;
  /** A label per index, or null. */
  indexLabels: string[] | null;
  /** An enum's option labels (option i is `min + i`), or null for other types. */
  options: string[] | null;
}

/** A parsed schema. */
export interface Schema {
  /** `schema_version`. */
  version: number;
  /** Group labels. */
  groups: string[];
  /** Params in schema order. */
  params: ParamSpec[];
}

/** Bytes in a little-endian u16. */
const U16_BYTES = 2;
/** Bytes in a little-endian i32. */
const I32_BYTES = 4;
/** Bits in a byte. */
const BYTE_BITS = 8;
/** Mask of a byte. */
const BYTE_MASK = 0xff;

/** Every type value this module draws as itself; anything else is drawn as an int. */
const KNOWN_TYPES: ReadonlySet<number> = new Set(Object.values(ParamType));

/** Reads the schema front to back, naming what it was reading when the bytes run out. */
class SchemaReader {
  readonly #bytes: ArrayLike<number>;
  #offset = 0;

  /**
   * @param bytes - The unpacked schema.
   */
  constructor(bytes: ArrayLike<number>) {
    this.#bytes = bytes;
  }

  /**
   * @param count - Bytes wanted.
   * @param what - The field, for the error.
   * @returns The offset they start at.
   */
  #take(count: number, what: string): number {
    const start = this.#offset;
    if (start + count > this.#bytes.length) {
      throw new SchemaError(
        `the schema ends early: ${what} needs ${count} bytes at offset ${start}, but only ${this.#bytes.length - start} are left`,
      );
    }
    this.#offset += count;
    return start;
  }

  /**
   * @param index - Offset of a byte.
   * @returns That byte.
   */
  #at(index: number): number {
    return (this.#bytes[index] ?? 0) & BYTE_MASK;
  }

  /**
   * @param what - The field.
   * @returns A u8.
   */
  u8(what: string): number {
    return this.#at(this.#take(1, what));
  }

  /**
   * @param what - The field.
   * @returns A little-endian u16.
   */
  u16(what: string): number {
    const start = this.#take(U16_BYTES, what);
    return this.#at(start) | (this.#at(start + 1) << BYTE_BITS);
  }

  /**
   * @param what - The field.
   * @returns A little-endian signed 32-bit integer.
   */
  i32(what: string): number {
    const start = this.#take(I32_BYTES, what);
    let value = 0;
    for (let i = 0; i < I32_BYTES; i++) {
      value |= this.#at(start + i) << (BYTE_BITS * i);
    }
    return value | 0;
  }

  /**
   * @param what - The field.
   * @returns A `u8 len` + ASCII string.
   */
  string(what: string): string {
    const length = this.u8(`${what} length`);
    const start = this.#take(length, what);
    let text = '';
    for (let i = 0; i < length; i++) {
      text += String.fromCharCode(this.#at(start + i));
    }
    return text;
  }

  /**
   * @param count - How many strings.
   * @param what - The field.
   * @returns The strings.
   */
  strings(count: number, what: string): string[] {
    return Array.from({ length: count }, (_, i) => this.string(`${what} ${i}`));
  }
}

/**
 * Parses one param record.
 *
 * @param reader - Positioned at the record.
 * @param position - The param's place in the list, for errors.
 * @returns The param.
 */
function readParam(reader: SchemaReader, position: number): ParamSpec {
  const at = `param ${position}`;
  const id = reader.u16(`${at} id`);
  const rawType = reader.u8(`${at} type`);
  const group = reader.u8(`${at} group`);
  const count = reader.u16(`${at} count`);
  const flags = reader.u8(`${at} flags`);
  if (count === 0) {
    throw new SchemaError(`${at} (id ${id}) has a count of 0`);
  }
  const min = reader.i32(`${at} min`);
  const max = reader.i32(`${at} max`);
  const isPerIndex = (flags & ParamFlag.PerIndexDefault) !== 0;
  const defaults = isPerIndex
    ? Array.from({ length: count }, (_, i) => reader.i32(`${at} default ${i}`))
    : new Array<number>(count).fill(reader.i32(`${at} default`));
  const name = reader.string(`${at} name`);
  const label = reader.string(`${at} label`);
  const unit = flags & ParamFlag.HasUnit ? reader.string(`${at} unit`) : null;
  const indexLabels = flags & ParamFlag.IndexLabels ? reader.strings(count, `${at} index label`) : null;
  const options = rawType === ParamType.Enum ? reader.strings(reader.u8(`${at} option count`), `${at} option`) : null;
  const type = KNOWN_TYPES.has(rawType) ? (rawType as ParamType) : ParamType.Int;
  return { id, type, rawType, group, count, flags, min, max, defaults, name, label, unit, indexLabels, options };
}

/**
 * Parses a schema (§4.4). Unknown param types read as ints and unknown flag bits are ignored, so
 * newer firmware still opens in this editor.
 *
 * @param bytes - The unpacked schema, as GET_SCHEMA's windows put together.
 * @returns The groups and params.
 */
export function parseSchema(bytes: ArrayLike<number>): Schema {
  const reader = new SchemaReader(bytes);
  const version = reader.u8('schema_version');
  if (version !== SCHEMA_VERSION) {
    throw new SchemaError(`schema version ${version} is not one this editor reads (it reads ${SCHEMA_VERSION})`);
  }
  const groupCount = reader.u8('group_count');
  const paramCount = reader.u16('param_count');
  const groups = reader.strings(groupCount, 'group label');
  const params = Array.from({ length: paramCount }, (_, i) => readParam(reader, i));
  return { version, groups, params };
}

/**
 * @param param - A param.
 * @returns Whether the device refuses SET_PARAM for it.
 */
export function isReadOnly(param: ParamSpec): boolean {
  return (param.flags & ParamFlag.ReadOnly) !== 0;
}

/**
 * @param param - A param.
 * @returns Whether COMMIT leaves it out of the stored profile.
 */
export function isVolatile(param: ParamSpec): boolean {
  return (param.flags & ParamFlag.Volatile) !== 0;
}

/** Note names from C, as MIDI counts them. */
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'] as const;
/** The octave MIDI note 0 is in (so 60 is C4). */
const FIRST_OCTAVE = -1;
/** The highest MIDI note. */
const MAX_NOTE = 127;

/**
 * Names a MIDI note, with 60 as C4.
 *
 * @param note - 0..127.
 * @returns E.g. `C4`, `F#-1`; an empty string outside 0..127.
 */
export function noteName(note: number): string {
  const isNote = Number.isInteger(note) && note >= 0 && note <= MAX_NOTE;
  if (isNote === false) {
    return '';
  }
  return `${NOTE_NAMES[note % NOTE_NAMES.length]}${Math.floor(note / NOTE_NAMES.length) + FIRST_OCTAVE}`;
}

/** One thing a group section draws: a plain param, or a table of array params that share rows. */
export type SchemaItem =
  | { kind: 'param'; param: ParamSpec }
  | {
      kind: 'table';
      /** Rows. */
      count: number;
      /** The rows' labels, or null to number them. */
      indexLabels: string[] | null;
      /** Columns, in schema order. */
      params: ParamSpec[];
    };

/** One group of the form. */
export interface SchemaSection {
  /** The group's label. */
  title: string;
  /** Its params and tables, in the order their first param appears. */
  items: SchemaItem[];
}

/**
 * @param labels - Index labels, or null.
 * @returns A key equal for equal label lists.
 */
function labelsKey(labels: string[] | null): string {
  return labels === null ? '' : JSON.stringify(labels);
}

/**
 * Lays the schema out as the editor draws it (§4.4's rendering rule): one section per group, in
 * group order, with array params in the same group, of the same `count` and with the same index
 * labels gathered into one table (a row per index, a column per param). A param whose group index
 * is out of range gets a section of its own named after the index.
 *
 * @param schema - The schema.
 * @returns The sections that have params.
 */
export function layoutSchema(schema: Schema): SchemaSection[] {
  const sections = new Map<number, SchemaSection>();
  const tables = new Map<string, Extract<SchemaItem, { kind: 'table' }>>();
  const groupIndices = [...new Set(schema.params.map((param) => param.group))].sort((a, b) => a - b);
  for (const group of groupIndices) {
    sections.set(group, { title: schema.groups[group] ?? `Group ${group}`, items: [] });
  }
  for (const param of schema.params) {
    const section = sections.get(param.group);
    if (section === undefined) {
      continue;
    }
    if (param.count === 1) {
      section.items.push({ kind: 'param', param });
      continue;
    }
    const key = `${param.group}/${param.count}/${labelsKey(param.indexLabels)}`;
    const table = tables.get(key);
    if (table === undefined) {
      const created = { kind: 'table' as const, count: param.count, indexLabels: param.indexLabels, params: [param] };
      tables.set(key, created);
      section.items.push(created);
    } else {
      table.params.push(param);
    }
  }
  return [...sections.values()];
}

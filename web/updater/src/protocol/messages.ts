/**
 * Payloads of each command and message in `protocol/sysex.md` §4: builders for requests and
 * parsers for replies and unsolicited messages. Payloads here are the 7-bit bytes inside a frame.
 */

import { encodeS32, encodeU14, encodeU28, encodeU32, PayloadReader, pack } from './encoding.ts';
import type { Frame } from './frame.ts';
import { Capability, type RebootMode, Role, UID_BYTES, Unsolicited } from './wire.ts';

/** What HELLO tells about a device. */
export interface HelloInfo {
  /** 0 app, 1 bootloader. */
  role: Role;
  /** Board id (§7). */
  boardId: number;
  /** Firmware version, `major<<24 | minor<<16 | patch`. */
  fwVersion: number;
  /** Protocol version the device speaks. */
  protoVersion: number;
  /** Unpacked bytes per FW_DATA; 0 in the app. */
  maxChunk: number;
  /** Capability bits (§4.1). */
  caps: number;
  /** Chip UID, 12 bytes. */
  uid: number[];
}

/** A LOG message (0x60). */
export interface LogMessage {
  kind: 'log';
  /** 0 error, 1 warn, 2 info, 3 debug. */
  level: number;
  /** Device time in milliseconds. */
  tMs: number;
  /** The text. */
  text: string;
}

/** An EVENT message (0x61). */
export interface EventMessage {
  kind: 'event';
  /** 1 button, 2 key, 3 analog, 4 encoder; others reserved. */
  type: number;
  /** Which button, key or pot. */
  index: number;
  /** Its new value. */
  value: number;
  /** Device time in milliseconds. */
  tMs: number;
}

/** A PARAM_CHANGED message (0x62): the device changed a value by itself. */
export interface ParamChangedMessage {
  kind: 'param';
  /** The param's id. */
  id: number;
  /** The index within it. */
  index: number;
  /** The value now in effect. */
  value: number;
}

/** Any unsolicited message this module understands. */
export type DeviceMessage = LogMessage | EventMessage | ParamChangedMessage;

/**
 * The REBOOT request payload.
 *
 * @param mode - 0 app, 1 bootloader.
 * @returns The payload.
 */
export function rebootPayload(mode: RebootMode): number[] {
  return [mode];
}

/** FW_BEGIN's fields. */
export interface FwBeginFields {
  /** Image size in bytes. */
  size: number;
  /** CRC-32 of the image with its crc field taken as 0. */
  crc32: number;
  /** Board id. */
  boardId: number;
  /** Firmware version. */
  fwVersion: number;
}

/**
 * The FW_BEGIN request payload: `size` u32, `crc32` u32, `board_id` u14, `fw_version` u32.
 *
 * @param fields - The image's facts.
 * @returns The payload.
 */
export function fwBeginPayload(fields: FwBeginFields): number[] {
  return [
    ...encodeU32(fields.size),
    ...encodeU32(fields.crc32),
    ...encodeU14(fields.boardId),
    ...encodeU32(fields.fwVersion),
  ];
}

/**
 * The FW_DATA request payload: `offset` u28, then the data packed 8→7.
 *
 * @param offset - Where the data goes in APROM.
 * @param data - The unpacked bytes.
 * @returns The payload.
 */
export function fwDataPayload(offset: number, data: ArrayLike<number>): number[] {
  return [...encodeU28(offset), ...pack(data)];
}

/**
 * The GET_SCHEMA request payload: `offset` u28, `max` u14.
 *
 * @param offset - The first schema byte wanted.
 * @param max - How many unpacked bytes, 1..64.
 * @returns The payload.
 */
export function getSchemaPayload(offset: number, max: number): number[] {
  return [...encodeU28(offset), ...encodeU14(max)];
}

/** One GET_SCHEMA reply. */
export interface SchemaWindow {
  /** The whole schema's size in bytes. */
  total: number;
  /** The echoed offset. */
  offset: number;
  /** The unpacked bytes from `offset` on. */
  data: number[];
}

/**
 * Parses GET_SCHEMA's reply fields: `total` u28, `offset` u28, then packed data.
 *
 * @param body - The reply payload without its status byte.
 * @returns The window.
 */
export function parseSchemaWindow(body: ArrayLike<number>): SchemaWindow {
  const reader = new PayloadReader(body);
  return { total: reader.u28(), offset: reader.u28(), data: reader.restPacked() };
}

/** One value of one param. */
export interface ParamValue {
  /** The param's id. */
  id: number;
  /** The index within it; 0 for a plain param. */
  index: number;
  /** The value. */
  value: number;
}

/**
 * The GET_PARAM request payload: `id` u14, `index` u14.
 *
 * @param id - The param.
 * @param index - The index within it.
 * @returns The payload.
 */
export function getParamPayload(id: number, index: number): number[] {
  return [...encodeU14(id), ...encodeU14(index)];
}

/**
 * The SET_PARAM request payload: `id` u14, `index` u14, `value` s32.
 *
 * @param value - Which value, and what to set it to.
 * @returns The payload.
 */
export function setParamPayload({ id, index, value }: ParamValue): number[] {
  return [...encodeU14(id), ...encodeU14(index), ...encodeS32(value)];
}

/**
 * Parses GET_PARAM's and SET_PARAM's reply fields, and PARAM_CHANGED's payload: `id` u14,
 * `index` u14, `value` s32.
 *
 * @param body - The reply payload without its status byte.
 * @returns The value.
 */
export function parseParamValue(body: ArrayLike<number>): ParamValue {
  const reader = new PayloadReader(body);
  return { id: reader.u14(), index: reader.u14(), value: reader.s32() };
}

/** A run of values of one param: `index .. index + values.length - 1`. */
export interface ParamRun {
  /** The param's id. */
  id: number;
  /** The first index. */
  index: number;
  /** The values, 1..16 of them. */
  values: number[];
}

/**
 * The GET_PARAMS request payload: `id` u14, `index` u14, `n` u14.
 *
 * @param id - The param.
 * @param index - The first index.
 * @param n - How many values, 1..16.
 * @returns The payload.
 */
export function getParamsPayload(id: number, index: number, n: number): number[] {
  return [...encodeU14(id), ...encodeU14(index), ...encodeU14(n)];
}

/**
 * The SET_PARAMS request payload: `id` u14, `index` u14, `n` u14, then `n` × s32.
 *
 * @param run - The param, the first index and the values.
 * @returns The payload.
 */
export function setParamsPayload({ id, index, values }: ParamRun): number[] {
  return [...encodeU14(id), ...encodeU14(index), ...encodeU14(values.length), ...values.flatMap(encodeS32)];
}

/**
 * Parses GET_PARAMS's reply fields: `id` u14, `index` u14, `n` u14, then `n` × s32.
 *
 * @param body - The reply payload without its status byte.
 * @returns The run.
 */
export function parseParamRun(body: ArrayLike<number>): ParamRun {
  const reader = new PayloadReader(body);
  const id = reader.u14();
  const index = reader.u14();
  const n = reader.u14();
  return { id, index, values: Array.from({ length: n }, () => reader.s32()) };
}

/**
 * Parses SET_PARAMS's reply fields: `id` u14, `index` u14, `n` u14.
 *
 * @param body - The reply payload without its status byte.
 * @returns The echoed address and count.
 */
export function parseParamRunEcho(body: ArrayLike<number>): { id: number; index: number; n: number } {
  const reader = new PayloadReader(body);
  return { id: reader.u14(), index: reader.u14(), n: reader.u14() };
}

/**
 * Parses HELLO's reply fields (after the status byte).
 *
 * @param body - The reply payload without its status byte.
 * @returns The device's facts.
 */
export function parseHello(body: ArrayLike<number>): HelloInfo {
  const reader = new PayloadReader(body);
  const role = reader.byte() === Role.Boot ? Role.Boot : Role.App;
  return {
    role,
    boardId: reader.u14(),
    fwVersion: reader.u32(),
    protoVersion: reader.byte(),
    maxChunk: reader.u14(),
    caps: reader.u14(),
    uid: reader.packed(UID_BYTES),
  };
}

/**
 * Parses FW_BEGIN's reply fields: `max_chunk` u14.
 *
 * @param body - The reply payload without its status byte.
 * @returns The device's chunk limit.
 */
export function parseFwBegin(body: ArrayLike<number>): number {
  return new PayloadReader(body).u14();
}

/**
 * Parses FW_DATA's reply fields: the `offset` u28 echo.
 *
 * @param body - The reply payload without its status byte.
 * @returns The echoed offset.
 */
export function parseFwData(body: ArrayLike<number>): number {
  return new PayloadReader(body).u28();
}

/**
 * Parses an unsolicited frame, if it is one this module understands.
 *
 * @param frame - Any decoded frame.
 * @returns The message, or null for anything else.
 */
export function parseDeviceMessage(frame: Frame): DeviceMessage | null {
  if (frame.cmd === Unsolicited.Log) {
    const reader = new PayloadReader(frame.payload);
    return { kind: 'log', level: reader.byte(), tMs: reader.u28(), text: reader.restAsText() };
  }
  if (frame.cmd === Unsolicited.Event) {
    const reader = new PayloadReader(frame.payload);
    return { kind: 'event', type: reader.byte(), index: reader.u14(), value: reader.u14(), tMs: reader.u28() };
  }
  if (frame.cmd === Unsolicited.ParamChanged) {
    return { kind: 'param', ...parseParamValue(frame.payload) };
  }
  return null;
}

/** Bits in an EVENT `value`. */
const EVENT_VALUE_BITS = 14;

/**
 * An encoder EVENT's `value` as the signed delta it carries (14-bit two's complement, §4).
 *
 * @param value - The u14 on the wire.
 * @returns −8192..8191.
 */
export function encoderDelta(value: number): number {
  const half = 2 ** (EVENT_VALUE_BITS - 1);
  return value >= half ? value - 2 * half : value;
}

/** The names of the capability bits, in bit order. */
const CAPABILITY_NAMES: ReadonlyArray<readonly [number, string]> = [
  [Capability.FirmwareUpload, 'firmware upload'],
  [Capability.Config, 'config'],
  [Capability.Log, 'log'],
  [Capability.Event, 'events'],
];

/**
 * Names the capability bits that are set.
 *
 * @param caps - HELLO's `caps`.
 * @returns The names, in bit order; unknown bits as `bit N`.
 */
export function capabilityNames(caps: number): string[] {
  const names = CAPABILITY_NAMES.filter(([bit]) => caps & bit).map(([, name]) => name);
  const known = CAPABILITY_NAMES.reduce((all, [bit]) => all | bit, 0);
  const CAPS_BITS = 14;
  for (let bit = 0; bit < CAPS_BITS; bit++) {
    const isUnknownSet = (caps & ~known & (1 << bit)) !== 0;
    if (isUnknownSet) {
      names.push(`bit ${bit}`);
    }
  }
  return names;
}

/** Shift of the major part of a firmware version. */
const MAJOR_SHIFT = 24;
/** Shift of the minor part. */
const MINOR_SHIFT = 16;
/** Mask of one version part. */
const PART_MASK = 0xff;
/** Mask of the patch part. */
const PATCH_MASK = 0xffff;

/**
 * Formats `major<<24 | minor<<16 | patch` (§5).
 *
 * @param version - The packed version.
 * @returns E.g. `1.2.3`.
 */
export function formatVersion(version: number): string {
  const major = Math.floor(version / 2 ** MAJOR_SHIFT) & PART_MASK;
  const minor = (version >>> MINOR_SHIFT) & PART_MASK;
  const patch = version & PATCH_MASK;
  return `${major}.${minor}.${patch}`;
}

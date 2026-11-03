/**
 * The words and fixed numbers of the gemifi SysEx protocol, v1 (`protocol/sysex.md`).
 * Facts, not tunables: a different value here is a different protocol.
 */

/** SysEx start byte. */
export const SYSEX_START = 0xf0;
/** SysEx end byte. */
export const SYSEX_END = 0xf7;
/** MIDI "non-commercial / development" ID (§1). A build constant so a real manufacturer ID can replace it. */
export const MANUFACTURER_ID = 0x7d;
/** First project signature byte, ASCII `g` (§1). */
const SIGNATURE_G = 0x67;
/** Second project signature byte, ASCII `m` (§1). */
const SIGNATURE_M = 0x6d;
/** Project signature, ASCII `g` `m` (§1). */
export const SIGNATURE = [SIGNATURE_G, SIGNATURE_M] as const;
/** The protocol version this module speaks (§1). */
export const PROTOCOL_VERSION = 1;

/** Bytes before the payload: F0, ID, two signature bytes, ver, cmd, seq, len_lo, len_hi. */
export const HEADER_BYTES = 9;
/** Bytes after the payload: chk, F7. */
export const TRAILER_BYTES = 2;
/** Offset of `ver` in a frame: the first byte the checksum covers. */
export const VERSION_OFFSET = 4;
/** The largest value a 7-bit byte holds. */
export const DATA_MASK = 0x7f;
/** The largest `seq` (§1). */
export const MAX_SEQ = 127;
/** The largest payload a 14-bit length can describe. */
export const MAX_PAYLOAD_BYTES = 0x3fff;

/** The largest request `cmd` (§1); replies are therefore 0x40–0x5F. */
export const REQUEST_LAST = 0x1f;
/** Added to a request's `cmd` to make its reply's (§1). */
export const REPLY_FLAG = 0x40;
/** Host → device notifications start here (§1, §4.6.2); they are never answered. */
export const NOTIFICATION_FIRST = 0x20;
/** The last notification `cmd` (§1). */
export const NOTIFICATION_LAST = 0x3f;
/** Unsolicited device → host messages start here (§1). */
export const UNSOLICITED_FIRST = 0x60;

/** Request commands (§4). */
export const Command = {
  Hello: 0x01,
  GetSchema: 0x02,
  GetParam: 0x03,
  SetParam: 0x04,
  Commit: 0x07,
  FactoryReset: 0x08,
  GetParams: 0x09,
  SetParams: 0x0a,
  Reboot: 0x10,
  GetComponent: 0x11,
  FwBegin: 0x18,
  FwData: 0x19,
  FwEnd: 0x1a,
  FwAbort: 0x1b,
} as const;
export type Command = (typeof Command)[keyof typeof Command];

/** Host → device notifications (§4.6.2): framed like requests, never answered. */
export const Notification = {
  FbLed: 0x20,
  FbLedRgb: 0x21,
  FbLeds: 0x22,
  FbValue: 0x23,
  FbRing: 0x24,
  FbText: 0x25,
  FbRelease: 0x26,
} as const;
export type Notification = (typeof Notification)[keyof typeof Notification];

/** Unsolicited device → host messages (§4). */
export const Unsolicited = {
  Log: 0x60,
  Event: 0x61,
  ParamChanged: 0x62,
} as const;
export type Unsolicited = (typeof Unsolicited)[keyof typeof Unsolicited];

/** Reply status codes (§3). */
export const Status = {
  Ok: 0,
  BadChecksum: 1,
  BadCmd: 2,
  BadLen: 3,
  BadState: 4,
  BadAddr: 5,
  FlashErr: 6,
  BadImage: 7,
  BadBoard: 8,
  Busy: 9,
  BadParam: 10,
  BadValue: 11,
} as const;
export type Status = (typeof Status)[keyof typeof Status];

/** The spec's name for each status code, for error messages. */
export const STATUS_NAMES: Readonly<Record<number, string>> = Object.freeze({
  0: 'OK',
  1: 'BAD_CHECKSUM',
  2: 'BAD_CMD',
  3: 'BAD_LEN',
  4: 'BAD_STATE',
  5: 'BAD_ADDR',
  6: 'FLASH_ERR',
  7: 'BAD_IMAGE',
  8: 'BAD_BOARD',
  9: 'BUSY',
  10: 'BAD_PARAM',
  11: 'BAD_VALUE',
});

/** What each status code means, in a reader's words (§3). */
export const STATUS_MEANINGS: Readonly<Record<number, string>> = Object.freeze({
  0: 'OK',
  1: 'the device saw a wrong frame checksum',
  2: 'the device does not know this command in its current role',
  3: 'the payload length is wrong for the command',
  4: 'the command is not allowed now',
  5: 'the offset is out of range, misaligned or out of order',
  6: 'erasing or programming flash failed',
  7: 'the final CRC or header check failed',
  8: "the image's board id does not match this device",
  9: 'the device is busy',
  10: 'the param is unknown, the index is out of range, or the param is read-only',
  11: "the value is outside the param's range",
});

/** The role a device answers HELLO with (§4). */
export const Role = { App: 0, Boot: 1 } as const;
export type Role = (typeof Role)[keyof typeof Role];

/** REBOOT's `mode` (§4). */
export const RebootMode = { App: 0, Boot: 1 } as const;
export type RebootMode = (typeof RebootMode)[keyof typeof RebootMode];

/** LOG levels (§4). */
export const LogLevel = { Error: 0, Warn: 1, Info: 2, Debug: 3 } as const;
export type LogLevel = (typeof LogLevel)[keyof typeof LogLevel];

/** EVENT types (§4). Others are reserved and shown as numbers. */
export const EventType = { Button: 1, Key: 2, Analog: 3, Encoder: 4 } as const;
export type EventType = (typeof EventType)[keyof typeof EventType];

/** Capability bits in HELLO's `caps` (§4.1). */
export const Capability = {
  FirmwareUpload: 1 << 0,
  Config: 1 << 1,
  Log: 1 << 2,
  Event: 1 << 3,
  Feedback: 1 << 6,
} as const;
export type Capability = (typeof Capability)[keyof typeof Capability];

/** GET_COMPONENT's `kind` (§4.6.1): the EVENT type numbers where they overlap, then LED and display. */
export const ComponentKind = { Button: 1, KeyMatrix: 2, Analog: 3, Encoder: 4, Led: 5, Display: 6 } as const;
export type ComponentKind = (typeof ComponentKind)[keyof typeof ComponentKind];

/** A component index field's "none" (§4.6.1): a button without an LED, an encoder without a centre LED. */
export const NONE_INDEX = 0x3fff;
/** The longest lstring (§2). */
export const LSTRING_MAX = 31;

/** An LED's `colours` (§4.6.1). */
export const LedColours = { Single: 1, Bicolour: 2, Rgb: 3 } as const;
export type LedColours = (typeof LedColours)[keyof typeof LedColours];

/** A display's `type` (§4.6.1). */
export const DisplayType = { Characters: 1, Segments: 2 } as const;
export type DisplayType = (typeof DisplayType)[keyof typeof DisplayType];

/** FB_LED / FB_LEDS `level`s (§4.6.2): 0 off, 1 blink, 2..127 on; on a bicolour LED 2..63 is colour 1, 64..127 colour 2. */
export const LedLevel = { Off: 0, Blink: 1, OnFirst: 2, Colour1Last: 63, Colour2First: 64, On: 127 } as const;
export type LedLevel = (typeof LedLevel)[keyof typeof LedLevel];

/** FB_RING's `mode` (§4.6.2), as Mackie Control's rings. */
export const RingMode = { Dot: 0, BoostCut: 1, Wrap: 2, Spread: 3 } as const;
export type RingMode = (typeof RingMode)[keyof typeof RingMode];

/** FB_RELEASE's `what` (§4.6.2). */
export const ReleaseWhat = { Everything: 0, Leds: 1, Values: 2, Text: 3 } as const;
export type ReleaseWhat = (typeof ReleaseWhat)[keyof typeof ReleaseWhat];

/** The most levels one FB_LEDS carries (§4.6.2). */
export const FB_LEDS_MAX = 64;
/** The most text bytes one FB_TEXT carries (§4.6.2). */
export const FB_TEXT_MAX = 64;

/** Flash page size the upload rules are written against (§4.2). */
export const PAGE_BYTES = 512;
/** FW_DATA chunks are a multiple of this many bytes (§4). */
export const CHUNK_ALIGN_BYTES = 4;
/** Hex digits in a board id as the spec writes it (`0x0001`). */
export const BOARD_ID_DIGITS = 4;
/** Radix of hex output. */
export const HEX_RADIX = 16;
/** One more than the largest u32. */
export const U32_LIMIT = 0x1_0000_0000;
/** Bytes in the chip UID HELLO carries, before packing (§4). */
export const UID_BYTES = 12;

/** The largest signed 32-bit value (§2, s32). */
export const S32_MAX = 0x7fff_ffff;
/** The smallest signed 32-bit value (§2, s32). */
export const S32_MIN = -0x8000_0000;

/** The schema layout this module parses (§4.4). */
export const SCHEMA_VERSION = 1;
/** The most unpacked schema bytes one GET_SCHEMA may ask for (§4.3). */
export const SCHEMA_WINDOW_BYTES = 64;
/** The most values one GET_PARAMS or SET_PARAMS carries (§4.3). */
export const PARAMS_RUN_MAX = 16;

/** A param's `type` in the schema (§4.4). Unknown values are shown as plain ints. */
export const ParamType = { Int: 0, Bool: 1, Enum: 2, Note: 3 } as const;
export type ParamType = (typeof ParamType)[keyof typeof ParamType];

/** A param's `flags` bits in the schema (§4.4). Unknown bits are ignored. */
export const ParamFlag = {
  ReadOnly: 1 << 0,
  Volatile: 1 << 1,
  PerIndexDefault: 1 << 2,
  IndexLabels: 1 << 3,
  HasUnit: 1 << 4,
} as const;
export type ParamFlag = (typeof ParamFlag)[keyof typeof ParamFlag];

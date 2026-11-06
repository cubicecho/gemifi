/**
 * A pretend Code 61 for `npm run dev` with `?demo` in the URL: HELLO, the config (§4.3) and the
 * presets (§4.5), in memory, so the Presets view can be worked on without a keyboard. It has the
 * physical unlock (§4.7): FACTORY_RESET and REBOOT(1) answer LOCKED, and the pretend user holds
 * the combo a few seconds later. It reports the diagnostics (§4.8) too. It is never part of a
 * production build (see `useDemoClient`).
 */

import { SysexClient, type Transport } from '../protocol/client.ts';
import { isDiagnostic } from '../protocol/diagnostics.ts';
import {
  encodeLstring,
  encodeS32,
  encodeU14,
  encodeU28,
  encodeU32,
  PayloadReader,
  pack,
} from '../protocol/encoding.ts';
import { decodeFrame, encodeFrame } from '../protocol/frame.ts';
import type { ParamSpec } from '../protocol/schema.ts';
import { encodeSchema, makeParam } from '../protocol/test-support.ts';
import {
  Capability,
  Command,
  ComponentKind,
  DiagParam,
  LedColours,
  LedOwner,
  NOTIFICATION_FIRST,
  NOTIFICATION_LAST,
  ParamFlag,
  ParamType,
  PresetFlag,
  PresetParam,
  Role,
  Status,
  UNLOCK_WINDOW_SECONDS,
  UnlockState,
  Unsolicited,
} from '../protocol/wire.ts';

const P = ParamFlag.Preset;
const BUTTONS = ['Oct -', 'Oct +', 'Sustain', 'Shift'];
const ANALOGS = ['Mod wheel', 'Expression'];
const PADS = ['Pad 1', 'Pad 2', 'Pad 3', 'Pad 4'];

/** Host feedback (§4.6): the Code 61's LEDs, roughly, and its 6-character display. */
const DEMO_LEDS: readonly { id: string; name: string; colours: number }[] = [
  ...['Channel', 'Edit', 'Zone', '1', '2', '3', '4'].map((name) => ({
    id: `${name.toLowerCase()}_led`,
    name,
    colours: LedColours.Single,
  })),
  ...Array.from({ length: 9 }, (_, i) => ({ id: `b${i + 1}_led`, name: `B${i + 1}`, colours: LedColours.Rgb })),
  ...Array.from({ length: 16 }, (_, i) => ({ id: `pad${i + 1}_led`, name: `Pad ${i + 1}`, colours: LedColours.Rgb })),
];
const DEMO_DISPLAY_COLS = 6;

/** The unlock combo (§4.7): the Code 61's boot combo, held for 2 s. */
const UNLOCK_BUTTONS = ['Up', 'Down'];
const UNLOCK_HOLD_MS = 2000;
/** How long after a LOCKED reply the pretend user has held the combo. */
const DEMO_UNLOCK_AFTER_MS = 4000;
/** An unlocked device locks again after this long without a request (§4.7). */
const RELOCK_IDLE_MS = 5 * 60 * 1000;
/** How long the pretend reboot takes before BOOTED. */
const DEMO_REBOOT_MS = 1500;
const MS_PER_SECOND = 1000;

/** The diagnostics (§4.8), read-only and VOLATILE in the Debug group. */
const DIAG = ParamFlag.ReadOnly | ParamFlag.Volatile;
const RESET_REASONS = ['unknown', 'power-on', 'reset pin', 'software', 'watchdog', 'brown-out', 'crash', 'low-power'];
const SETTINGS_STATES = ['loaded', 'none stored', 'some reset', 'ignored (safe mode)', 'wiped'];
/** A reset_reason of software, for after a pretend reboot. */
const RESET_SOFTWARE = 3;

/** Roughly the Code 61's groups and params, with the per-preset ones flagged. */
const GROUPS = ['MIDI', 'Velocity', 'Buttons', 'Analog', 'Pads', 'Presets', 'Debug'];
const PARAMS: ParamSpec[] = [
  makeParam({ id: 1, name: 'midi.channel', label: 'MIDI channel', group: 0, min: 1, max: 16, defaults: [1], flags: P }),
  makeParam({
    id: 2,
    name: 'midi.transpose',
    label: 'Transpose',
    group: 0,
    min: -24,
    max: 24,
    defaults: [0],
    flags: P,
  }),
  makeParam({ id: 3, name: 'midi.octave', label: 'Octave', group: 0, min: -4, max: 4, defaults: [0], flags: P }),
  makeParam({ id: 30, name: 'midi.thru', label: 'MIDI thru', group: 0, type: ParamType.Bool, max: 1, defaults: [1] }),
  makeParam({
    id: 4,
    name: 'velocity.curve',
    label: 'Velocity curve',
    group: 1,
    type: ParamType.Enum,
    max: 3,
    options: ['Normal', 'Soft', 'Hard', 'Fixed'],
    flags: P,
  }),
  makeParam({ id: 5, name: 'velocity.fixed', label: 'Fixed velocity', group: 1, min: 1, defaults: [100], flags: P }),
  makeParam({ id: 6, name: 'velocity.fast', label: 'Fastest hit', group: 1, max: 5000, defaults: [400], unit: 'µs' }),
  makeParam({
    id: 7,
    name: 'velocity.slow',
    label: 'Slowest hit',
    group: 1,
    max: 60000,
    defaults: [30000],
    unit: 'µs',
  }),
  makeParam({
    id: 9,
    name: 'button.action',
    label: 'Action',
    group: 2,
    type: ParamType.Enum,
    count: 4,
    max: 6,
    defaults: [5, 6, 1, 0],
    indexLabels: BUTTONS,
    options: ['None', 'CC momentary', 'CC toggle', 'CC ramp', 'Note', 'Octave down', 'Octave up'],
    flags: P,
  }),
  makeParam({
    id: 10,
    name: 'button.number',
    label: 'Number',
    group: 2,
    count: 4,
    defaults: [0, 0, 64, 0],
    indexLabels: BUTTONS,
    flags: P,
  }),
  makeParam({
    id: 8,
    name: 'analog.cc',
    label: 'CC',
    group: 3,
    count: 2,
    defaults: [1, 11],
    indexLabels: ANALOGS,
    flags: P,
  }),
  makeParam({
    id: 29,
    name: 'analog.mode',
    label: 'Sends',
    group: 3,
    type: ParamType.Enum,
    count: 2,
    max: 2,
    indexLabels: ANALOGS,
    options: ['CC', 'Pitch bend', 'Channel pressure'],
    flags: P,
  }),
  makeParam({
    id: 20,
    name: 'pad.note',
    label: 'Note',
    group: 4,
    type: ParamType.Note,
    count: 4,
    defaults: [36, 38, 42, 46],
    indexLabels: PADS,
    flags: P,
  }),
  makeParam({ id: 24, name: 'pad.threshold', label: 'Hit threshold', group: 4, max: 4095, defaults: [300] }),
  makeParam({
    id: PresetParam.Active,
    name: 'preset.active',
    label: 'Active preset',
    group: 5,
    min: 1,
    max: 3,
    defaults: [1],
  }),
  makeParam({
    id: PresetParam.Program,
    name: 'preset.program',
    label: 'Program change',
    group: 5,
    min: -1,
    defaults: [-1],
    flags: P,
  }),
  makeParam({
    id: PresetParam.Dirty,
    name: 'preset.dirty',
    label: 'Unsaved edits',
    group: 5,
    type: ParamType.Bool,
    max: 1,
    flags: ParamFlag.Volatile | ParamFlag.ReadOnly,
  }),
  makeParam({
    id: PresetParam.Count,
    name: 'preset.count',
    label: 'Presets in use',
    group: 5,
    min: 1,
    max: 3,
    defaults: [1],
    flags: ParamFlag.ReadOnly,
  }),
  makeParam({
    id: 13,
    name: 'debug.events',
    label: 'Send events',
    group: 6,
    type: ParamType.Bool,
    max: 1,
    flags: ParamFlag.Volatile,
  }),
  makeParam({
    id: DiagParam.ResetReason,
    name: 'diag.reset_reason',
    label: 'Reset reason',
    group: 6,
    type: ParamType.Enum,
    max: RESET_REASONS.length - 1,
    defaults: [1],
    options: RESET_REASONS,
    flags: DIAG,
  }),
  makeParam({
    id: DiagParam.BootLoop,
    name: 'diag.boot_loop',
    label: 'Boot-loop fallback',
    group: 6,
    type: ParamType.Bool,
    max: 1,
    flags: DIAG,
  }),
  makeParam({
    id: DiagParam.Settings,
    name: 'diag.settings',
    label: 'Settings at power-up',
    group: 6,
    type: ParamType.Enum,
    max: SETTINGS_STATES.length - 1,
    options: SETTINGS_STATES,
    flags: DIAG,
  }),
  makeParam({
    id: DiagParam.ScanMax,
    name: 'diag.scan_max',
    label: 'Longest scan',
    group: 6,
    max: 0x7fff_ffff,
    defaults: [212],
    unit: 'µs',
    flags: DIAG,
  }),
  makeParam({
    id: DiagParam.LoopMax,
    name: 'diag.loop_max',
    label: 'Longest main loop',
    group: 6,
    max: 0x7fff_ffff,
    defaults: [1840],
    unit: 'µs',
    flags: DIAG,
  }),
  makeParam({
    id: DiagParam.ScanPeriod,
    name: 'diag.scan_period',
    label: 'Scan period',
    group: 6,
    max: 0x7fff_ffff,
    defaults: [1000],
    unit: 'µs',
    flags: DIAG,
  }),
  makeParam({
    id: DiagParam.Uptime,
    name: 'diag.uptime',
    label: 'Uptime',
    group: 6,
    max: 0x7fff_ffff,
    unit: 's',
    flags: DIAG,
  }),
];
const SLOTS = 3;
/** Slot 1 is the device file's; the others start as copies of it. */
const YAML_NAMES = ['Default'];

/** What a slot has stored. */
interface StoredSlot {
  name: string | null;
  values: Map<string, number>;
}

/** @returns `id:index` for every value of the params `keep` takes. */
function keysOf(keep: (param: ParamSpec) => boolean): Array<{ key: string; param: ParamSpec; index: number }> {
  return PARAMS.filter(keep).flatMap((param) =>
    Array.from({ length: param.count }, (_, index) => ({ key: `${param.id}:${index}`, param, index })),
  );
}

const isPerPreset = (param: ParamSpec) => (param.flags & ParamFlag.Preset) !== 0;

/** The pretend device behind an in-memory transport. */
class DemoDevice {
  readonly #schemaBytes = encodeSchema({ groups: GROUPS, params: PARAMS });
  readonly #live = new Map<string, number>();
  readonly #stored = new Map<number, StoredSlot>();
  #active = 1;
  #dirty = false;
  /** Presets in use: slots 1..used; the rest are free (§4.5). */
  #used = 2;
  #listeners = new Set<(bytes: Uint8Array) => void>();
  /** Physical unlock (§4.7). */
  #isUnlocked = false;
  /** When the unlock window closes, as a `Date.now()` time; 0 when it is closed. */
  #windowEndsAt = 0;
  #unlockTimer: ReturnType<typeof setTimeout> | undefined;
  #relockTimer: ReturnType<typeof setTimeout> | undefined;
  #startedAt = Date.now();

  constructor() {
    for (const { key, param, index } of keysOf(() => true)) {
      this.#live.set(key, param.defaults[index] ?? 0);
    }
    // A second preset to start with, so there is something to switch to and delete.
    this.#stored.set(2, { name: 'Drums', values: new Map([['1:0', 10]]) });
  }

  /** @returns The live values of `slot` as it would load: stored, else the defaults. */
  #valuesOf(slot: number): Map<string, number> {
    const stored = this.#stored.get(slot)?.values;
    return new Map(
      keysOf(isPerPreset).map(({ key, param, index }) => [key, stored?.get(key) ?? param.defaults[index] ?? 0]),
    );
  }

  /** PRESET_MOVE: the preset in `from` goes to `to`, the ones between shift towards `from`. */
  #move(from: number, to: number): void {
    const order = Array.from({ length: this.#used }, (_, i) => ({
      name: this.#stored.get(i + 1)?.name ?? (i < YAML_NAMES.length ? (YAML_NAMES[i] ?? null) : null),
      values: this.#valuesOf(i + 1),
      isActive: i + 1 === this.#active,
    }));
    const [moving] = order.splice(from - 1, 1);
    if (moving === undefined) {
      return;
    }
    order.splice(to - 1, 0, moving);
    order.forEach((preset, i) => {
      this.#stored.set(i + 1, { name: preset.name, values: preset.values });
    });
    this.#load(order.findIndex((preset) => preset.isActive) + 1);
  }

  /** PRESET_DELETE: the later presets move up, the last one in use is freed. */
  #delete(slot: number): void {
    for (let s = slot; s < this.#used; s++) {
      const next = this.#stored.get(s + 1);
      const name = next?.name ?? (s + 1 <= YAML_NAMES.length ? (YAML_NAMES[s] ?? null) : null);
      this.#stored.set(s, { name, values: this.#valuesOf(s + 1) });
    }
    this.#stored.delete(this.#used);
    const was = this.#active;
    this.#used -= 1;
    const next = was === slot ? Math.min(slot, this.#used) : was > slot ? was - 1 : was;
    this.#load(next);
  }

  transport(): Transport {
    return {
      send: (bytes) => setTimeout(() => this.#handle(bytes), 15),
      subscribe: (listener) => {
        this.#listeners.add(listener);
        return () => this.#listeners.delete(listener);
      },
    };
  }

  #emit(bytes: Uint8Array): void {
    for (const listener of this.#listeners) {
      listener(bytes);
    }
  }

  /** Opens (or restarts) the unlock window; the pretend user holds the combo a little later. */
  #openWindow(): void {
    this.#windowEndsAt = Date.now() + UNLOCK_WINDOW_SECONDS * MS_PER_SECOND;
    clearTimeout(this.#unlockTimer);
    this.#unlockTimer = setTimeout(() => {
      this.#isUnlocked = true;
      this.#windowEndsAt = 0;
      this.#emit(encodeFrame(Unsolicited.Unlocked, 0, [UnlockState.Unlocked]));
      this.#keepUnlocked();
    }, DEMO_UNLOCK_AFTER_MS);
  }

  /** Locks again after five minutes without a request (§4.7). */
  #keepUnlocked(): void {
    clearTimeout(this.#relockTimer);
    if (this.#isUnlocked === false) {
      return;
    }
    this.#relockTimer = setTimeout(() => this.#lock(), RELOCK_IDLE_MS);
  }

  #lock(): void {
    clearTimeout(this.#unlockTimer);
    clearTimeout(this.#relockTimer);
    const wasUnlocked = this.#isUnlocked;
    this.#isUnlocked = false;
    this.#windowEndsAt = 0;
    if (wasUnlocked) {
      this.#emit(encodeFrame(Unsolicited.Unlocked, 0, [UnlockState.Locked]));
    }
  }

  /** @returns Seconds left in the unlock window, 0 when it is closed. */
  #windowSeconds(): number {
    return Math.max(0, Math.ceil((this.#windowEndsAt - Date.now()) / MS_PER_SECOND));
  }

  /** @returns The UNLOCK reply's fields (§4.7). */
  #unlockFields(): number[] {
    return [
      this.#isUnlocked ? UnlockState.Unlocked : UnlockState.Locked,
      this.#windowSeconds(),
      ...encodeU14(UNLOCK_HOLD_MS),
      UNLOCK_BUTTONS.length,
      ...UNLOCK_BUTTONS.flatMap(encodeLstring),
    ];
  }

  /** @returns HELLO's fields after the status, which BOOTED repeats (§4.6.3). */
  #helloFields(): number[] {
    return [
      Role.App,
      ...encodeU14(2),
      ...encodeU32(0x0002_003e),
      0x01,
      ...encodeU14(256),
      ...encodeU14(
        Capability.Config |
          Capability.Log |
          Capability.Event |
          Capability.Presets |
          Capability.Feedback |
          Capability.Unlock,
      ),
      ...pack([0xde, 0x70, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]),
      14,
      ...Array.from('m-audio-code61', (c) => c.charCodeAt(0)),
    ];
  }

  /** A pretend restart: locked again, a fresh uptime, then BOOTED. */
  #restart(): void {
    this.#lock();
    setTimeout(() => {
      this.#startedAt = Date.now();
      this.#live.set(`${DiagParam.ResetReason}:0`, RESET_SOFTWARE);
      this.#emit(encodeFrame(Unsolicited.Booted, 0, this.#helloFields()));
    }, DEMO_REBOOT_MS);
  }

  #changed(): void {
    this.#emit(encodeFrame(Unsolicited.PresetChanged, 0, [...encodeU14(this.#active), this.#dirty ? 1 : 0]));
  }

  #load(slot: number): void {
    for (const [key, value] of this.#valuesOf(slot)) {
      this.#live.set(key, value);
    }
    this.#active = slot;
    this.#live.set(`${PresetParam.Active}:0`, slot);
    this.#dirty = false;
  }

  #name(slot: number): string {
    return this.#stored.get(slot)?.name ?? YAML_NAMES[slot - 1] ?? `Preset ${slot}`;
  }

  #info(slot: number): number[] {
    const flags =
      (this.#stored.has(slot) ? PresetFlag.Stored : 0) |
      (slot === this.#active && this.#dirty ? PresetFlag.Dirty : 0) |
      (slot <= YAML_NAMES.length ? PresetFlag.Yaml : 0) |
      (slot <= this.#used ? PresetFlag.InUse : 0);
    const program = slot === this.#active ? (this.#live.get(`${PresetParam.Program}:0`) ?? -1) : -1;
    return [
      ...encodeU14(slot),
      ...encodeU14(SLOTS),
      ...encodeU14(this.#active),
      flags,
      ...encodeU14(1),
      ...encodeU14(1),
      ...encodeS32(program),
      ...encodeLstring(this.#name(slot)),
    ];
  }

  #handle(bytes: Uint8Array): void {
    const result = decodeFrame(bytes);
    if (result.ok === false) {
      return;
    }
    const { cmd, seq, payload } = result.frame;
    const reply = (status: number, fields: number[] = []) =>
      this.#emit(encodeFrame(cmd | 0x40, seq, [status, ...fields]));
    const reader = new PayloadReader(payload);
    const isNotification = cmd >= NOTIFICATION_FIRST && cmd <= NOTIFICATION_LAST;
    if (isNotification) {
      return; // FB_*: never answered; the demo has no LEDs to light
    }
    this.#keepUnlocked();
    // FACTORY_RESET and REBOOT(1) need the user's OK on the device first (§4.7).
    const isGuarded = cmd === Command.FactoryReset || (cmd === Command.Reboot && payload[0] === 1);
    if (isGuarded && this.#isUnlocked === false) {
      this.#openWindow();
      reply(Status.Locked);
      return;
    }
    switch (cmd) {
      case Command.Hello:
        reply(Status.Ok, this.#helloFields());
        return;
      case Command.Unlock: {
        const open = payload[0];
        if (payload.length !== 1) {
          reply(Status.BadLen);
          return;
        }
        if (open !== 0 && open !== 1) {
          reply(Status.BadCmd);
          return;
        }
        if (open === 1 && this.#isUnlocked === false) {
          this.#openWindow();
        }
        reply(Status.Ok, this.#unlockFields());
        return;
      }
      case Command.Reboot:
        reply(Status.Ok);
        this.#restart();
        return;
      case Command.GetSchema: {
        const offset = reader.u28();
        const max = reader.u14();
        const data = this.#schemaBytes.slice(offset, offset + max);
        reply(Status.Ok, [...encodeU28(this.#schemaBytes.length), ...encodeU28(offset), ...pack(data)]);
        return;
      }
      case Command.GetParam:
      case Command.SetParam: {
        const id = reader.u14();
        const index = reader.u14();
        const param = PARAMS.find((p) => p.id === id);
        if (param === undefined || index >= param.count) {
          reply(Status.BadParam);
          return;
        }
        const key = `${id}:${index}`;
        if (cmd === Command.SetParam) {
          const value = reader.s32();
          if ((param.flags & ParamFlag.ReadOnly) !== 0) {
            reply(Status.BadParam);
            return;
          }
          if (value < param.min || value > param.max) {
            reply(Status.BadValue);
            return;
          }
          if (id === PresetParam.Active) {
            if (value > this.#used) {
              reply(Status.BadValue);
              return;
            }
            this.#load(value);
            reply(Status.Ok, [...encodeU14(id), ...encodeU14(index), ...encodeS32(value)]);
            this.#changed();
            return;
          }
          this.#live.set(key, value);
          if (isPerPreset(param) && this.#dirty === false) {
            this.#dirty = true;
            setTimeout(() => this.#changed(), 0);
          }
        }
        const now =
          id === PresetParam.Dirty
            ? Number(this.#dirty)
            : id === PresetParam.Count
              ? this.#used
              : id === DiagParam.Uptime
                ? Math.floor((Date.now() - this.#startedAt) / MS_PER_SECOND)
                : (this.#live.get(key) ?? 0);
        reply(Status.Ok, [...encodeU14(id), ...encodeU14(index), ...encodeS32(now)]);
        return;
      }
      case Command.Commit:
      case Command.PresetSave: {
        const slot = cmd === Command.Commit ? this.#active : reader.u14();
        if (slot < 1 || slot > SLOTS) {
          reply(Status.BadParam);
          return;
        }
        const values = new Map(keysOf(isPerPreset).map(({ key }) => [key, this.#live.get(key) ?? 0]));
        this.#stored.set(slot, { name: this.#stored.get(slot)?.name ?? null, values });
        this.#used = Math.max(this.#used, slot);
        this.#active = slot;
        this.#live.set(`${PresetParam.Active}:0`, slot);
        this.#dirty = false;
        reply(Status.Ok);
        this.#changed();
        return;
      }
      case Command.PresetReset: {
        const slot = reader.u14();
        this.#stored.delete(slot);
        reply(Status.Ok);
        if (slot === this.#active) {
          this.#load(slot);
          this.#changed();
        }
        return;
      }
      case Command.PresetName: {
        const slot = reader.u14();
        const name = reader.lstring();
        const old = this.#stored.get(slot);
        if (slot > this.#used) {
          // Naming a free slot makes it a preset: a copy of preset 1's firmware values.
          this.#stored.set(slot, { name: name === '' ? null : name, values: new Map() });
          this.#used = slot;
        } else if (name === '' && (old === undefined || old.values.size === 0)) {
          this.#stored.delete(slot);
        } else {
          this.#stored.set(slot, { name: name === '' ? null : name, values: old?.values ?? new Map() });
        }
        reply(Status.Ok);
        return;
      }
      case Command.PresetInfo: {
        const asked = reader.u14();
        const slot = asked === 0 ? this.#active : asked;
        if (slot < 1 || slot > SLOTS) {
          reply(Status.BadParam);
          return;
        }
        reply(Status.Ok, this.#info(slot));
        return;
      }
      case Command.PresetDelete: {
        const slot = reader.u14();
        if (slot < 1 || slot > this.#used) {
          reply(Status.BadParam);
          return;
        }
        if (this.#used === 1) {
          reply(Status.BadState);
          return;
        }
        this.#delete(slot);
        reply(Status.Ok);
        this.#changed();
        return;
      }
      case Command.PresetMove: {
        const from = reader.u14();
        const to = reader.u14();
        if (from < 1 || from > this.#used || to < 1 || to > this.#used) {
          reply(Status.BadParam);
          return;
        }
        this.#move(from, to);
        reply(Status.Ok);
        this.#changed();
        return;
      }
      case Command.GetComponent: {
        const kind = reader.byte();
        const index = reader.u14();
        const head = (count: number, id: string, name: string) => [
          kind,
          ...encodeU14(index),
          ...encodeU14(count),
          ...encodeLstring(id),
          ...encodeLstring(name),
          ...encodeLstring(''),
        ];
        const led = DEMO_LEDS[index];
        if (kind === ComponentKind.Led && led !== undefined) {
          reply(Status.Ok, [...head(DEMO_LEDS.length, led.id, led.name), led.colours, LedOwner.HostThenLocal]);
        } else if (kind === ComponentKind.Display && index === 0) {
          reply(Status.Ok, [...head(1, 'display', 'Display'), DEMO_DISPLAY_COLS, 1, 1]);
        } else {
          reply(Status.BadParam);
        }
        return;
      }
      case Command.FactoryReset:
        this.#stored.clear();
        this.#used = YAML_NAMES.length;
        for (const { key, param, index } of keysOf((p) => isDiagnostic(p) === false)) {
          this.#live.set(key, param.defaults[index] ?? 0);
        }
        this.#load(1);
        reply(Status.Ok);
        this.#changed();
        return;
      default:
        reply(Status.BadCmd);
    }
  }
}

/** @returns A client talking to a new pretend Code 61. */
export function demoClient(): SysexClient {
  return new SysexClient(new DemoDevice().transport());
}

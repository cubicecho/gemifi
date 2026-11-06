import { describe, expect, it } from 'vitest';
import { layoutSchema } from '../protocol/schema.ts';
import { makeParam } from '../protocol/test-support.ts';
import { ParamFlag, PresetParam } from '../protocol/wire.ts';
import { settingsSections, splitByScope } from './settings-view.tsx';

const P = ParamFlag.Preset;

describe('splitByScope', () => {
  const schema = {
    version: 1,
    groups: ['MIDI', 'Velocity', 'Presets'],
    params: [
      makeParam({ id: 1, name: 'midi.channel', label: 'MIDI channel', group: 0, flags: P }),
      makeParam({ id: 30, name: 'midi.thru', label: 'MIDI thru', group: 0 }),
      makeParam({ id: 6, name: 'velocity.fast', label: 'Fastest hit', group: 1 }),
      makeParam({ id: 8, name: 'analog.cc', label: 'CC', group: 1, count: 2, flags: P }),
      makeParam({ id: 29, name: 'analog.mode', label: 'Sends', group: 1, count: 2 }),
      makeParam({ id: PresetParam.Active, name: 'preset.active', label: 'Active preset', group: 2 }),
      makeParam({ id: PresetParam.Program, name: 'preset.program', label: 'Program', group: 2, flags: P }),
      makeParam({ id: PresetParam.Dirty, name: 'preset.dirty', label: 'Dirty', group: 2, flags: ParamFlag.Volatile }),
    ],
  };
  const { perPreset, shared } = splitByScope(layoutSchema(schema));
  const ids = (sections: typeof perPreset) =>
    sections.map((section) => [
      section.title,
      section.items.flatMap((item) => (item.kind === 'param' ? [item.param.id] : item.params.map((p) => p.id))),
    ]);

  it('puts what each preset has its own of first, and splits a table by scope', () => {
    expect(ids(perPreset)).toEqual([
      ['MIDI', [1]],
      ['Velocity', [8]],
      ['On switching to this preset', [PresetParam.Program]],
    ]);
  });

  it('leaves the active slot and the dirty flag to the Presets card', () => {
    expect(ids(shared)).toEqual([
      ['MIDI', [30]],
      ['Velocity', [6, 29]],
    ]);
  });
});

describe('settingsSections', () => {
  it('leaves the diagnostics (§4.8) to the Device card, and keeps the rest of Debug', () => {
    const RO = ParamFlag.ReadOnly | ParamFlag.Volatile;
    const schema = {
      version: 1,
      groups: ['MIDI', 'Debug', 'Stats'],
      params: [
        makeParam({ id: 1, name: 'midi.channel', label: 'MIDI channel', group: 0 }),
        makeParam({ id: 13, name: 'debug.events', label: 'Send events', group: 1, flags: ParamFlag.Volatile }),
        makeParam({ id: 65, name: 'diag.reset_reason', label: 'Reset reason', group: 1, flags: RO }),
        makeParam({ id: 71, name: 'diag.uptime', label: 'Uptime', group: 2, flags: RO }),
      ],
    };
    const titles = settingsSections(layoutSchema(schema)).map((section) => [
      section.title,
      section.items.flatMap((item) => (item.kind === 'param' ? [item.param.id] : item.params.map((p) => p.id))),
    ]);
    expect(titles).toEqual([
      ['MIDI', [1]],
      ['Debug', [13]],
    ]);
  });
});

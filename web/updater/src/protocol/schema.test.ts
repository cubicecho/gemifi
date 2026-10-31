import { describe, expect, it } from 'vitest';
import { isReadOnly, isVolatile, layoutSchema, noteName, parseSchema, SchemaError } from './schema.ts';
import { EXAMPLE_SCHEMA, encodeSchema, makeParam } from './test-support.ts';
import { ParamFlag, ParamType } from './wire.ts';

describe('parseSchema (§4.4)', () => {
  it('round-trips the example schema', () => {
    expect(parseSchema(encodeSchema(EXAMPLE_SCHEMA))).toEqual(EXAMPLE_SCHEMA);
  });

  it('reads a hand-written schema byte for byte', () => {
    // version 1, 1 group "G", 1 param: id 0x0102, int, group 0, count 1, flags 0, min -2, max 300, default 7.
    const bytes = [
      1, 1, 1, 0, 1, 0x47, 0x02, 0x01, 0, 0, 1, 0, 0, 0xfe, 0xff, 0xff, 0xff, 0x2c, 0x01, 0, 0, 7, 0, 0, 0, 1, 0x78, 1,
      0x58,
    ];
    expect(parseSchema(bytes)).toEqual({
      version: 1,
      groups: ['G'],
      params: [
        {
          id: 0x0102,
          type: ParamType.Int,
          rawType: 0,
          group: 0,
          count: 1,
          flags: 0,
          min: -2,
          max: 300,
          defaults: [7],
          name: 'x',
          label: 'X',
          unit: null,
          indexLabels: null,
          options: null,
        },
      ],
    });
  });

  it('repeats one default across an array, and reads per-index defaults and index labels', () => {
    const shared = makeParam({ id: 1, name: 'a', label: 'A', count: 3, defaults: [5] });
    const perIndex = makeParam({ id: 2, name: 'b', label: 'B', count: 2, defaults: [1, 2], indexLabels: ['L', 'R'] });
    const schema = parseSchema(encodeSchema({ groups: ['G'], params: [shared, perIndex] }));
    expect(schema.params[0]?.defaults).toEqual([5, 5, 5]);
    expect(schema.params[1]?.defaults).toEqual([1, 2]);
    expect(schema.params[1]?.indexLabels).toEqual(['L', 'R']);
    expect((schema.params[1]?.flags ?? 0) & ParamFlag.PerIndexDefault).toBeTruthy();
  });

  it('reads a u16 count and a unit string', () => {
    const keys = makeParam({
      id: 7,
      name: 'key.note',
      label: 'Note',
      type: ParamType.Note,
      count: 300,
      defaults: [60],
    });
    const delay = makeParam({ id: 8, name: 'delay', label: 'Delay', min: 0, max: 5000, unit: 'ms' });
    const schema = parseSchema(encodeSchema({ groups: ['G'], params: [keys, delay] }));
    expect(schema.params[0]?.count).toBe(300);
    expect(schema.params[0]?.defaults).toHaveLength(300);
    expect(schema.params[1]?.unit).toBe('ms');
    expect(schema.params[1]?.label).toBe('Delay');
  });

  it('shows an unknown type as an int and ignores unknown flag bits', () => {
    const odd = makeParam({ id: 3, name: 'odd', label: 'Odd', rawType: 9, type: ParamType.Int, flags: 0x80 | 0x20 });
    const parsed = parseSchema(encodeSchema({ groups: ['G'], params: [odd] })).params[0];
    expect(parsed?.type).toBe(ParamType.Int);
    expect(parsed?.rawType).toBe(9);
    expect(parsed?.options).toBeNull();
    expect(parsed?.indexLabels).toBeNull();
    expect(parsed && isReadOnly(parsed)).toBe(false);
  });

  it('reads the READ_ONLY and VOLATILE bits', () => {
    const params = EXAMPLE_SCHEMA.params;
    expect(params.filter(isVolatile).map((p) => p.name)).toEqual(['debug.events']);
    const ro = makeParam({ id: 1, name: 'fw', label: 'Firmware', flags: ParamFlag.ReadOnly });
    expect(isReadOnly(parseSchema(encodeSchema({ groups: [], params: [ro] })).params[0] ?? ro)).toBe(true);
  });

  it('throws a SchemaError naming the field on every truncation', () => {
    const bytes = encodeSchema(EXAMPLE_SCHEMA);
    for (let length = 0; length < bytes.length; length++) {
      expect(() => parseSchema(bytes.slice(0, length)), `at ${length}`).toThrow(SchemaError);
    }
    expect(() => parseSchema(bytes.slice(0, 10))).toThrow(/the schema ends early: .* at offset/);
  });

  it('refuses another schema version and a count of 0', () => {
    expect(() => parseSchema([2, 0, 0, 0])).toThrow(/schema version 2/);
    const zero = encodeSchema({ groups: [], params: [makeParam({ id: 1, name: 'z', label: 'Z' })] });
    zero[8] = 0; // count, low byte: after the 4-byte header, the id u16, type and group
    expect(() => parseSchema(zero)).toThrow(/count of 0/);
  });

  it('ignores bytes after the last param', () => {
    expect(parseSchema([...encodeSchema(EXAMPLE_SCHEMA), 0xaa, 0xbb])).toEqual(EXAMPLE_SCHEMA);
  });
});

describe('layoutSchema (§4.4 rendering rule)', () => {
  it('makes a section per group and one table of the buttons', () => {
    const sections = layoutSchema(EXAMPLE_SCHEMA);
    expect(sections.map((s) => s.title)).toEqual(['MIDI', 'Velocity', 'Knob', 'Buttons', 'Debug']);
    const buttons = sections[3]?.items ?? [];
    expect(buttons).toHaveLength(1);
    const table = buttons[0];
    expect(table?.kind).toBe('table');
    if (table?.kind === 'table') {
      expect(table.count).toBe(7);
      expect(table.params.map((p) => p.name)).toEqual(['button.action', 'button.number']);
      expect(table.indexLabels?.[0]).toBe('Sustain');
    }
    expect(sections[0]?.items.map((i) => i.kind)).toEqual(['param', 'param', 'param']);
  });

  it('keeps arrays apart when count, labels or group differ', () => {
    const a = makeParam({ id: 1, name: 'a', label: 'A', count: 4 });
    const b = makeParam({ id: 2, name: 'b', label: 'B', count: 4 });
    const c = makeParam({ id: 3, name: 'c', label: 'C', count: 8 });
    const d = makeParam({ id: 4, name: 'd', label: 'D', count: 4, indexLabels: ['1', '2', '3', '4'] });
    const e = makeParam({ id: 5, name: 'e', label: 'E', count: 4, group: 1 });
    const sections = layoutSchema({ version: 1, groups: ['X', 'Y'], params: [a, b, c, d, e] });
    const tables = sections[0]?.items.map((i) => (i.kind === 'table' ? i.params.map((p) => p.name) : null));
    expect(tables).toEqual([['a', 'b'], ['c'], ['d']]);
    expect(sections[1]?.items).toHaveLength(1);
  });

  it('names a section for a group index past the list, and skips empty groups', () => {
    const p = makeParam({ id: 1, name: 'p', label: 'P', group: 5 });
    expect(layoutSchema({ version: 1, groups: ['Empty'], params: [p] }).map((s) => s.title)).toEqual(['Group 5']);
  });
});

describe('noteName', () => {
  it('names MIDI notes with 60 as C4', () => {
    expect(noteName(60)).toBe('C4');
    expect(noteName(0)).toBe('C-1');
    expect(noteName(61)).toBe('C#4');
    expect(noteName(69)).toBe('A4');
    expect(noteName(127)).toBe('G9');
    expect(noteName(128)).toBe('');
    expect(noteName(-1)).toBe('');
  });
});

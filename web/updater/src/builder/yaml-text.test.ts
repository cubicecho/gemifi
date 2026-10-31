import { describe, expect, it } from 'vitest';
import { insertIndent, lineRange } from './yaml-text.ts';

const TEXT = 'name: Keys\nplatform: esp32\ncontrols:\n  - pin: 4';

describe('the YAML editor text', () => {
  it('finds a line by its number', () => {
    expect(lineRange(TEXT, 1)).toEqual({ start: 0, end: 10 });
    expect(TEXT.slice(lineRange(TEXT, 2).start, lineRange(TEXT, 2).end)).toBe('platform: esp32');
    expect(TEXT.slice(lineRange(TEXT, 4).start, lineRange(TEXT, 4).end)).toBe('  - pin: 4');
  });

  it('stops at the last line when the number is past the end', () => {
    expect(lineRange(TEXT, 99)).toEqual(lineRange(TEXT, 4));
  });

  it('inserts two spaces at the caret', () => {
    expect(insertIndent('a\nb', { start: 2, end: 2 })).toEqual({ text: 'a\n  b', caret: 4 });
  });

  it('replaces a selection with the indent', () => {
    expect(insertIndent('abc', { start: 1, end: 2 })).toEqual({ text: 'a  c', caret: 3 });
  });
});

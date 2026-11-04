import { describe, expect, it } from 'vitest';
import { Colour, colourToRgb, DEMO_EFFECTS, type DemoFrame, frameChanges, seededRandom } from './effects.ts';

const CANVAS = { leds: 32, displays: [6] };

/**
 * Runs an effect for a while.
 *
 * @param id - The effect's id.
 * @param ms - How long.
 * @param canvas - The LEDs and displays.
 * @returns A frame every 40 ms.
 */
function run(id: string, ms: number, canvas = CANVAS): DemoFrame[] {
  const effect = DEMO_EFFECTS.find((e) => e.id === id);
  if (effect === undefined) {
    throw new Error(`no effect ${id}`);
  }
  const step = effect.start(canvas, seededRandom(7));
  const frames: DemoFrame[] = [];
  for (let t = 0; t <= ms; t += 40) {
    frames.push(step(t));
  }
  return frames;
}

describe('demo effects', () => {
  it('offers five effects with distinct ids', () => {
    expect(DEMO_EFFECTS).toHaveLength(5);
    expect(new Set(DEMO_EFFECTS.map((e) => e.id)).size).toBe(5);
  });

  for (const effect of DEMO_EFFECTS) {
    it(`${effect.id}: one colour per LED and one text per display, the display's width`, () => {
      for (const frame of run(effect.id, 3000, { leds: 32, displays: [6, 3] })) {
        expect(frame.leds).toHaveLength(32);
        expect(frame.leds.every((c) => Number.isInteger(c) && c >= Colour.Off && c <= Colour.White)).toBe(true);
        expect(frame.texts.map((t) => t.length)).toEqual([6, 3]);
        expect(frame.texts.every((t) => /^[\x20-\x7e]*$/.test(t))).toBe(true);
      }
    });

    it(`${effect.id}: copes with no LEDs, one LED and no display`, () => {
      for (const canvas of [
        { leds: 0, displays: [6] },
        { leds: 1, displays: [] },
      ]) {
        const frames = run(effect.id, 1000, canvas);
        expect(frames.every((f) => f.leds.length === canvas.leds && f.texts.length === canvas.displays.length)).toBe(
          true,
        );
      }
    });

    it(`${effect.id}: something changes`, () => {
      const frames = run(effect.id, 5000);
      const distinct = new Set(frames.map((f) => `${f.leds.join(',')}|${f.texts.join('|')}`));
      expect(distinct.size).toBeGreaterThan(3);
    });
  }

  it('scanner: one white eye that reaches both ends', () => {
    const frames = run('scanner', 6000);
    const eyes = frames.map((f) => f.leds.indexOf(Colour.White));
    expect(frames.every((f) => f.leds.filter((c) => c === Colour.White).length === 1)).toBe(true);
    expect(Math.min(...eyes)).toBe(0);
    expect(Math.max(...eyes)).toBe(31);
  });

  it('breathe: from none lit to all lit and back, in one colour at a time', () => {
    const frames = run('breathe', 4000);
    const lit = frames.map((f) => f.leds.filter((c) => c !== Colour.Off).length);
    expect(Math.min(...lit)).toBe(0);
    expect(Math.max(...lit)).toBe(32);
    expect(frames.every((f) => new Set(f.leds.filter((c) => c !== Colour.Off)).size <= 1)).toBe(true);
  });

  it('rainbow: every LED lit, and neighbours differ', () => {
    const [frame] = run('rainbow', 0);
    expect(frame?.leds.every((c) => c !== Colour.Off)).toBe(true);
    expect(frame?.leds[0]).not.toBe(frame?.leds[1]);
  });

  it('fireworks: flashes white and leaves coloured sparks', () => {
    const frames = run('fireworks', 4000);
    expect(frames.some((f) => f.leds.includes(Colour.White))).toBe(true);
    expect(frames.some((f) => f.leds.some((c) => c !== Colour.Off && c !== Colour.White))).toBe(true);
    expect(frames.some((f) => f.texts[0]?.includes('*'))).toBe(true);
  });
});

describe('colourToRgb', () => {
  it('turns each colour bit into a full channel', () => {
    expect(colourToRgb(Colour.Off)).toEqual({ r: 0, g: 0, b: 0 });
    expect(colourToRgb(Colour.Yellow)).toEqual({ r: 127, g: 127, b: 0 });
    expect(colourToRgb(Colour.White)).toEqual({ r: 127, g: 127, b: 127 });
  });
});

describe('frameChanges', () => {
  it('sends everything the first time, then only differences', () => {
    const a: DemoFrame = { leds: [0, 1, 2], texts: ['ab'] };
    expect(frameChanges(null, a)).toEqual({ leds: [0, 1, 2], displays: [0] });
    expect(frameChanges(a, { leds: [0, 3, 2], texts: ['ab'] })).toEqual({ leds: [1], displays: [] });
    expect(frameChanges(a, { leds: [0, 1, 2], texts: ['ac'] })).toEqual({ leds: [], displays: [0] });
  });
});

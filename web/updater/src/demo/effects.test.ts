import { describe, expect, it } from 'vitest';
import {
  Colour,
  channels,
  colourToRgb,
  DEMO_EFFECTS,
  type DemoFrame,
  frameChanges,
  previewCss,
  seededRandom,
} from './effects.ts';

const CANVAS = { leds: 32, displays: [6] };

/**
 * What an on/off RGB LED shows for a colour (protocol/sysex.md 4.6.2): each channel at least half
 * the brightest.
 *
 * @param colour - A colour.
 * @returns Bit 0 red, 1 green, 2 blue.
 */
function onOff(colour: number): number {
  const [r, g, b] = channels(colour);
  const top = Math.max(r, g, b);
  if (top === 0) {
    return 0;
  }
  return (2 * r >= top ? 1 : 0) | (2 * g >= top ? 2 : 0) | (2 * b >= top ? 4 : 0);
}

/**
 * @param colour - A colour.
 * @returns Whether it is lit but not at full (a fade only an any-colour LED shows).
 */
function isDim(colour: number): boolean {
  const top = Math.max(...channels(colour));
  return top > 0 && top < 255;
}

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
        expect(frame.leds.every((c) => Number.isInteger(c) && c >= 0 && c <= 0xffffff)).toBe(true);
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

  it('breathe: from none lit to all lit at full and back, in one colour at a time', () => {
    const frames = run('breathe', 4000);
    const lit = frames.map((f) => f.leds.filter((c) => c !== Colour.Off).length);
    expect(Math.min(...lit)).toBe(0);
    expect(Math.max(...lit)).toBe(32);
    expect(frames.some((f) => f.leds.every((c) => c === Colour.Blue))).toBe(true);
    expect(frames.every((f) => new Set(f.leds.filter((c) => c !== Colour.Off).map(onOff)).size <= 1)).toBe(true);
  });

  it('breathe: fades (dim LEDs), and on/off LEDs still light one by one', () => {
    const frames = run('breathe', 4000);
    expect(frames.some((f) => f.leds.some(isDim))).toBe(true);
    const lit = new Set(frames.map((f) => f.leds.filter((c) => c !== Colour.Off).length));
    expect(lit.size).toBeGreaterThan(8);
  });

  it('rainbow: every LED lit, neighbours differ, and it moves smoothly', () => {
    const [frame, next] = run('rainbow', 40);
    expect(frame?.leds.every((c) => c !== Colour.Off)).toBe(true);
    expect(frame?.leds[0]).not.toBe(frame?.leds[1]);
    expect(next?.leds[0]).not.toBe(frame?.leds[0]); // 40 ms is a quarter of a step
    const basic = new Set<number>(Object.values(Colour));
    expect(next?.leds.some((c) => basic.has(c) === false)).toBe(true);
  });

  it('scanner: the tail fades', () => {
    const [frame] = run('scanner', 0, { leds: 32, displays: [] });
    // At the start the eye is at 0 going forward: no tail. A little later it has one.
    const later = run('scanner', 400, { leds: 32, displays: [] }).at(-1);
    expect(frame?.leds.filter(isDim)).toHaveLength(0);
    expect(later?.leds.filter(isDim).length).toBeGreaterThanOrEqual(2);
  });

  it('fireworks: flashes white and leaves coloured sparks', () => {
    const frames = run('fireworks', 4000);
    expect(frames.some((f) => f.leds.includes(Colour.White))).toBe(true);
    expect(frames.some((f) => f.leds.some((c) => c !== Colour.Off && c !== Colour.White))).toBe(true);
    expect(frames.some((f) => f.texts[0]?.includes('*'))).toBe(true);
  });
});

describe('colourToRgb', () => {
  it('scales each 8-bit channel to 0..127', () => {
    expect(colourToRgb(Colour.Off)).toEqual({ r: 0, g: 0, b: 0 });
    expect(colourToRgb(Colour.Yellow)).toEqual({ r: 127, g: 127, b: 0 });
    expect(colourToRgb(Colour.White)).toEqual({ r: 127, g: 127, b: 127 });
    expect(colourToRgb(0x804002)).toEqual({ r: 64, g: 32, b: 1 });
  });
});

describe('previewCss', () => {
  it('shows off as the off look, and a dim colour as its hue mixed in by its brightness', () => {
    expect(previewCss(Colour.Off)).toContain('currentColor 12%');
    expect(previewCss(Colour.Red)).toMatch(/^color-mix\(in oklab, rgb\(255 0 0\) 100%, /);
    expect(previewCss(0x400000)).toMatch(/rgb\(255 0 0\) 25%/);
    expect(previewCss(0x402000)).toMatch(/rgb\(255 128 0\) 25%/);
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

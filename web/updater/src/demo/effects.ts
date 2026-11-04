/**
 * LED and display effects for the feedback demo: what every LED and display shows at a moment,
 * as a DAW script would send it. Framework-free and deterministic for a given random source, so
 * the effects can be tested without a device.
 *
 * gemifi LEDs are on/off per colour channel (`protocol/sysex.md` §4.6.2), with no brightness. So a
 * colour here is 3 bits (red, green, blue) and "dimmer" means fewer LEDs lit. The LEDs are a strip
 * in their component order; a display is a row of characters.
 */

/** A colour: bit 0 red, bit 1 green, bit 2 blue. 0 is off. */
export type LedColour = number;

export const Colour = {
  Off: 0,
  Red: 1,
  Green: 2,
  Yellow: 3,
  Blue: 4,
  Magenta: 5,
  Cyan: 6,
  White: 7,
} as const;

/** What the device should show at one moment. */
export interface DemoFrame {
  /** One colour per LED, in component order. */
  leds: LedColour[];
  /** One text per display, each exactly that display's width. */
  texts: string[];
}

/** What an effect is drawn on. */
export interface DemoCanvas {
  /** How many LEDs. */
  leds: number;
  /** Each display's width in characters. */
  displays: readonly number[];
}

/** One effect, running: call it with the effect's time in ms (it only moves forward). */
export type EffectRun = (timeMs: number) => DemoFrame;

/** An effect the demo offers. */
export interface DemoEffect {
  id: string;
  name: string;
  /** One sentence for the reader. */
  description: string;
  /**
   * Starts the effect.
   *
   * @param canvas - The LEDs and displays.
   * @param random - A source of numbers in 0..1 (Math.random, or a seeded one in tests).
   * @returns The running effect.
   */
  start(canvas: DemoCanvas, random: () => number): EffectRun;
}

/** The colours of a rainbow, in hue order. */
const WHEEL: readonly LedColour[] = [Colour.Red, Colour.Yellow, Colour.Green, Colour.Cyan, Colour.Blue, Colour.Magenta];

/** Burst colours for fireworks: anything but white, which is the flash. */
const BURST_COLOURS: readonly LedColour[] = WHEEL;

/** Breathing colours, one per breath. */
const BREATH_COLOURS: readonly LedColour[] = [Colour.Blue, Colour.Cyan, Colour.Magenta, Colour.Green];

/**
 * @param random - The random source.
 * @param n - How many choices.
 * @returns An integer in 0..n-1.
 */
function pick(random: () => number, n: number): number {
  return Math.min(n - 1, Math.floor(random() * n));
}

/**
 * Where strip position `i` of `n` lands on a display `cols` wide.
 *
 * @param i - The position, 0..n-1.
 * @param n - The strip's length.
 * @param cols - The display's width.
 * @returns The column, 0..cols-1.
 */
function toColumn(i: number, n: number, cols: number): number {
  if (n <= 1 || cols <= 1) {
    return 0;
  }
  return Math.round((i / (n - 1)) * (cols - 1));
}

/**
 * @param cols - A display's width.
 * @returns A row of spaces to draw on.
 */
function blankRow(cols: number): string[] {
  return Array.from({ length: cols }, () => ' ');
}

/**
 * Puts a character on a row, if the column is on it.
 *
 * @param row - The row.
 * @param col - The column.
 * @param c - The character.
 */
function put(row: string[], col: number, c: string): void {
  const isOnRow = col >= 0 && col < row.length;
  if (isOnRow) {
    row[col] = c;
  }
}

/**
 * A Fisher–Yates shuffle of 0..n-1.
 *
 * @param n - How many.
 * @param random - The random source.
 * @returns The order.
 */
function shuffled(n: number, random: () => number): number[] {
  const order = Array.from({ length: n }, (_, i) => i);
  for (let i = n - 1; i > 0; i--) {
    const j = pick(random, i + 1);
    const t = order[i] ?? 0;
    order[i] = order[j] ?? 0;
    order[j] = t;
  }
  return order;
}

/** One firework: a flash, then a ring of its colour spreading out, then sparkles. */
interface Burst {
  at: number;
  born: number;
  colour: LedColour;
  radius: number;
}

/** Fireworks timing and odds. */
const FW_FLASH_MS = 90; // the white flash at the launch point
const FW_STEP_MS = 70; // the ring grows one LED this often
const FW_LIFE_MS = 700; // a burst's whole life
const FW_FADE_MS = 350; // after this the display shows dots, not crosses
const FW_LEDS_PER_RADIUS = 6; // the largest ring is a sixth of the LEDs...
const FW_MIN_MAX_RADIUS = 2; // ... but at least this
const FW_LAUNCH_MIN_MS = 150; // between launches: at least this
const FW_LAUNCH_SPREAD_MS = 650; // ... plus up to this
const FW_SPARK_CHANCE = 0.5; // a ring LED leaves a spark
const FW_SPARK_WHITE = 0.3; // a spark is white instead of the burst's colour
const FW_SPARK_SHOWN = 0.7; // a dying spark is lit in a frame (it flickers)
const FW_SPARK_MIN_MS = 200; // a spark lasts at least this
const FW_SPARK_SPREAD_MS = 500; // ... plus up to this
const FW_DISPLAY_SPREAD = 3; // the display's ring reaches a third of its width

/** Fireworks: random launches, a white flash, a coloured ring spreading out, sparkles fading. */
const fireworks: DemoEffect = {
  id: 'fireworks',
  name: 'Fireworks',
  description: 'Random launches: a white flash, a coloured ring spreading out, then sparkles that die away.',
  start({ leds, displays }, random) {
    let bursts: Burst[] = [];
    const sparks = new Map<number, { colour: LedColour; until: number }>();
    let nextLaunch = 0;
    return (t) => {
      if (t >= nextLaunch) {
        const radius = 1 + pick(random, Math.max(FW_MIN_MAX_RADIUS, Math.ceil(leds / FW_LEDS_PER_RADIUS)));
        const colour = BURST_COLOURS[pick(random, BURST_COLOURS.length)] ?? Colour.Red;
        bursts.push({ at: pick(random, leds), born: t, colour, radius });
        nextLaunch = t + FW_LAUNCH_MIN_MS + random() * FW_LAUNCH_SPREAD_MS;
      }
      const out: LedColour[] = Array.from({ length: leds }, () => Colour.Off);
      const rows = displays.map(blankRow);
      for (const [i, spark] of sparks) {
        if (t >= spark.until) {
          sparks.delete(i);
        } else if (random() < FW_SPARK_SHOWN) {
          out[i] = spark.colour; // flickers as it dies
        }
      }
      const alive: Burst[] = [];
      for (const burst of bursts) {
        const age = t - burst.born;
        if (age > FW_LIFE_MS) {
          continue;
        }
        alive.push(burst);
        if (age < FW_FLASH_MS) {
          out[burst.at] = Colour.White;
          for (const [d, row] of rows.entries()) {
            put(row, toColumn(burst.at, leds, displays[d] ?? 0), '*');
          }
          continue;
        }
        const r = Math.min(burst.radius, 1 + Math.floor((age - FW_FLASH_MS) / FW_STEP_MS));
        for (const i of [burst.at - r, burst.at + r]) {
          if (i >= 0 && i < leds) {
            out[i] = burst.colour;
            if (random() < FW_SPARK_CHANCE) {
              const colour = random() < FW_SPARK_WHITE ? Colour.White : burst.colour;
              sparks.set(i, { colour, until: t + FW_SPARK_MIN_MS + random() * FW_SPARK_SPREAD_MS });
            }
          }
        }
        for (const [d, row] of rows.entries()) {
          const cols = displays[d] ?? 0;
          const centre = toColumn(burst.at, leds, cols);
          const spread = Math.round((r / Math.max(1, burst.radius)) * Math.max(1, cols / FW_DISPLAY_SPREAD));
          const mark = age > FW_FADE_MS ? '.' : '+';
          put(row, centre - spread, mark);
          put(row, centre + spread, mark);
        }
      }
      bursts = alive;
      return { leds: out, texts: rows.map((row) => row.join('')) };
    };
  },
};

/** Fire: heat is 0..1 per LED. */
const FIRE_START_HEAT = 0.5; // each LED starts with up to this
const FIRE_STEP_MS = 40; // the heat moves on this often
const FIRE_KEEP = 0.6; // of its own heat, a step keeps this...
const FIRE_BORROW = 0.15; // ... borrows this much of each neighbour's...
const FIRE_COOL = 0.12; // ... and loses up to this
const FIRE_FLARE = 0.08; // a step flares an LED up
const FIRE_FLARE_MIN = 0.75; // to at least this heat
const FIRE_FLARE_SPREAD = 0.25; // ... plus up to this
const FIRE_HOT = 0.72; // hotter: yellow (white now and then); the display shows '*'
const FIRE_WARM = 0.38; // hotter: red (yellow now and then)
const FIRE_EMBER = 0.25; // hotter: red now and then
const FIRE_HOT_WHITE = 0.2;
const FIRE_WARM_YELLOW = 0.15;
const FIRE_EMBER_RED = 0.3;
const FIRE_GLOW = 0.5; // the display: '^' above this
const FIRE_SMOKE = 0.3; // the display: "'" above this

/** Fire: every LED flickers with its own heat, red when warm, yellow when hot. */
const fire: DemoEffect = {
  id: 'fire',
  name: 'Fire',
  description: 'Every LED has its own heat: it flares up at random, cools down and flickers red and yellow.',
  start({ leds, displays }, random) {
    const heat = Array.from({ length: leds }, () => random() * FIRE_START_HEAT);
    let last = 0;
    return (t) => {
      const steps = Math.max(1, Math.round((t - last) / FIRE_STEP_MS));
      last = t;
      for (let s = 0; s < steps; s++) {
        for (let i = 0; i < leds; i++) {
          const left = heat[i - 1] ?? heat[i] ?? 0;
          const right = heat[i + 1] ?? heat[i] ?? 0;
          const own = heat[i] ?? 0;
          // Cool, borrow a little from the neighbours, and sometimes flare up.
          let next = own * FIRE_KEEP + (left + right) * FIRE_BORROW - random() * FIRE_COOL;
          if (random() < FIRE_FLARE) {
            next = FIRE_FLARE_MIN + random() * FIRE_FLARE_SPREAD;
          }
          heat[i] = Math.max(0, Math.min(1, next));
        }
      }
      const out = heat.map((h) => {
        if (h > FIRE_HOT) {
          return random() < FIRE_HOT_WHITE ? Colour.White : Colour.Yellow;
        }
        if (h > FIRE_WARM) {
          return random() < FIRE_WARM_YELLOW ? Colour.Yellow : Colour.Red;
        }
        return h > FIRE_EMBER && random() < FIRE_EMBER_RED ? Colour.Red : Colour.Off;
      });
      const texts = displays.map((cols) => {
        const row = blankRow(cols);
        for (let c = 0; c < cols; c++) {
          // The hottest LED that lands on this column.
          let h = 0;
          for (let i = 0; i < leds; i++) {
            if (toColumn(i, leds, cols) === c) {
              h = Math.max(h, heat[i] ?? 0);
            }
          }
          row[c] = h > FIRE_HOT ? '*' : h > FIRE_GLOW ? '^' : h > FIRE_SMOKE ? "'" : ' ';
        }
        return row.join('');
      });
      return { leds: out, texts };
    };
  },
};

/** Scanner: one sweep takes about this long, one LED step within these bounds. */
const SCAN_SWEEP_MS = 1400;
const SCAN_MIN_STEP_MS = 25;
const SCAN_MAX_STEP_MS = 90;
/** The tail's LEDs behind the eye, farthest first. */
const SCAN_TAIL = [2, 1] as const;

/** Scanner: a white eye with a red tail sweeping back and forth. */
const scanner: DemoEffect = {
  id: 'scanner',
  name: 'Scanner',
  description: 'A white eye with a red tail sweeping from end to end and back.',
  start({ leds, displays }) {
    const STEP_MS = Math.max(SCAN_MIN_STEP_MS, Math.min(SCAN_MAX_STEP_MS, SCAN_SWEEP_MS / Math.max(1, leds)));
    return (t) => {
      const span = Math.max(1, leds - 1);
      const step = Math.floor(t / STEP_MS) % (2 * span);
      const isForward = step < span;
      const at = Math.min(Math.max(0, leds - 1), isForward ? step : 2 * span - step);
      const back = isForward ? -1 : 1; // the tail trails behind the eye
      const out: LedColour[] = Array.from({ length: leds }, () => Colour.Off);
      for (const k of SCAN_TAIL) {
        const i = at + back * k;
        if (i >= 0 && i < leds) {
          out[i] = Colour.Red;
        }
      }
      if (leds > 0) {
        out[at] = Colour.White;
      }
      const texts = displays.map((cols) => {
        const row = blankRow(cols);
        const col = toColumn(at, leds, cols);
        put(row, col + back, '-');
        put(row, col, '#');
        return row.join('');
      });
      return { leds: out, texts };
    };
  },
};

/** Rainbow: the wheel moves one LED this often, the text one character this often. */
const RAINBOW_STEP_MS = 160;
const RAINBOW_TEXT_STEP_MS = 280;
const RAINBOW_TEXT = 'GEMIFI FEEDBACK DEMO   ';

/** Rainbow: the colour wheel scrolling along the LEDs; the display scrolls a line of text. */
const rainbow: DemoEffect = {
  id: 'rainbow',
  name: 'Rainbow',
  description: 'The colour wheel rolling slowly along every LED, with a line of text scrolling on the display.',
  start({ leds, displays }) {
    return (t) => {
      const shift = Math.floor(t / RAINBOW_STEP_MS);
      const out = Array.from(
        { length: leds },
        (_, i) => WHEEL[(((i - shift) % WHEEL.length) + WHEEL.length) % WHEEL.length] ?? Colour.Red,
      );
      const start = Math.floor(t / RAINBOW_TEXT_STEP_MS);
      const texts = displays.map((cols) =>
        Array.from({ length: cols }, (_, c) => RAINBOW_TEXT[(start + c) % RAINBOW_TEXT.length] ?? ' ').join(''),
      );
      return { leds: out, texts };
    };
  },
};

/** Breathe: one breath takes this long; past this much of a breath the display shows '='. */
const BREATH_MS = 4000;
const BREATH_DEEP = 0.6;

/** Breathe: more and more LEDs light, then fewer, a new colour each breath. */
const breathe: DemoEffect = {
  id: 'breathe',
  name: 'Breathe',
  description: 'A slow breath: LEDs light one by one, then go out again, in a new colour each time.',
  start({ leds, displays }, random) {
    const order = shuffled(leds, random);
    return (t) => {
      const breath = Math.floor(t / BREATH_MS);
      const phase = (1 - Math.cos((2 * Math.PI * (t % BREATH_MS)) / BREATH_MS)) / 2;
      const lit = Math.round(phase * leds);
      const colour = BREATH_COLOURS[breath % BREATH_COLOURS.length] ?? Colour.Blue;
      const out: LedColour[] = Array.from({ length: leds }, () => Colour.Off);
      for (let k = 0; k < lit; k++) {
        out[order[k] ?? 0] = colour;
      }
      const texts = displays.map((cols) => {
        const row = blankRow(cols);
        const width = Math.round(phase * cols);
        const from = Math.floor((cols - width) / 2);
        for (let c = from; c < from + width; c++) {
          put(row, c, phase > BREATH_DEEP ? '=' : '-');
        }
        return row.join('');
      });
      return { leds: out, texts };
    };
  },
};

/** Every effect, noisy ones first. */
export const DEMO_EFFECTS: readonly DemoEffect[] = [fireworks, fire, scanner, rainbow, breathe];

/**
 * A seeded random source (mulberry32), for tests and repeatable runs.
 *
 * @param seed - Any integer.
 * @returns Numbers in 0..1.
 */
export function seededRandom(seed: number): () => number {
  const INCREMENT = 0x6d2b79f5;
  const RANGE = 0x1_0000_0000;
  let a = seed >>> 0;
  return () => {
    a = (a + INCREMENT) >>> 0;
    let x = a;
    x = Math.imul(x ^ (x >>> 15), x | 1);
    x ^= x + Math.imul(x ^ (x >>> 7), x | 61);
    return ((x ^ (x >>> 14)) >>> 0) / RANGE;
  };
}

/** The 0..127 value FB_LED_RGB gets for a colour channel that is on. */
const CHANNEL_ON = 127;

/**
 * @param colour - A demo colour.
 * @returns Its FB_LED_RGB parts.
 */
export function colourToRgb(colour: LedColour): { r: number; g: number; b: number } {
  return {
    r: (colour & 1) === 0 ? 0 : CHANNEL_ON,
    g: (colour & 2) === 0 ? 0 : CHANNEL_ON,
    b: (colour & 4) === 0 ? 0 : CHANNEL_ON,
  };
}

/** CSS colours for the on-page preview, by demo colour. */
export const PREVIEW_CSS: Readonly<Record<number, string>> = Object.freeze({
  [Colour.Off]: 'color-mix(in oklab, currentColor 12%, transparent)',
  [Colour.Red]: '#ef4444',
  [Colour.Green]: '#22c55e',
  [Colour.Yellow]: '#facc15',
  [Colour.Blue]: '#3b82f6',
  [Colour.Magenta]: '#d946ef',
  [Colour.Cyan]: '#22d3ee',
  [Colour.White]: '#f8fafc',
});

/** What changed between two frames, for sending only differences. */
export interface FrameChanges {
  /** LED indexes whose colour changed (or that were never sent). */
  leds: number[];
  /** Display indexes whose text changed (or that were never sent). */
  displays: number[];
}

/**
 * @param sent - What was last sent, or null for nothing yet.
 * @param next - The new frame.
 * @returns What to send.
 */
export function frameChanges(sent: DemoFrame | null, next: DemoFrame): FrameChanges {
  const leds: number[] = [];
  for (const [i, colour] of next.leds.entries()) {
    if (sent === null || sent.leds[i] !== colour) {
      leds.push(i);
    }
  }
  const displays: number[] = [];
  for (const [d, text] of next.texts.entries()) {
    if (sent === null || sent.texts[d] !== text) {
      displays.push(d);
    }
  }
  return { leds, displays };
}

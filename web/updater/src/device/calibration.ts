/**
 * The input tester's calibration pass: each analog input's lowest and highest reading while the
 * user moves every control end to end, turned into `adc_min` / `adc_max` lines for the device
 * YAML (`core/README.md`, `analog:`).
 */

/** The lowest and highest reading seen of each analog input (null: never read). */
export type SeenRange = readonly ({ min: number; max: number } | null)[];

/** A reading is 12 bits (§4.8). */
export const ADC_TOP = 4095;
/** Counts each end is pulled in by, so the control reaches its ends even with a little noise. */
export const CALIBRATION_INSET = 8;
/** Less movement than this between the lowest and highest reading is no calibration. */
export const CALIBRATION_MIN_SPAN = 200;

/**
 * @param seen - The ranges so far.
 * @param values - The latest readings, by index.
 * @returns The ranges with them included.
 */
export function widen(seen: SeenRange, values: readonly (number | null)[]): SeenRange {
  const length = Math.max(seen.length, values.length);
  return Array.from({ length }, (_, i) => {
    const old = seen[i] ?? null;
    const v = values[i];
    if (v === null || v === undefined) {
      return old;
    }
    return old === null ? { min: v, max: v } : { min: Math.min(old.min, v), max: Math.max(old.max, v) };
  });
}

/** One analog input for the snippet. */
export interface CalibrationInput {
  /** The YAML `id:`, or null when the device doesn't say (no GET_COMPONENT). */
  id: string | null;
  /** The display name. */
  name: string;
  /** Whether the YAML's range is reversed (`adc_min` above `adc_max`), so it stays that way. */
  isReversed: boolean;
}

/**
 * @param text - A name.
 * @returns It as a YAML flow scalar: plain when that is safe, else double-quoted.
 */
function yamlScalar(text: string): string {
  return /^[A-Za-z][A-Za-z0-9 _+-]*$/.test(text) ? text : JSON.stringify(text);
}

/**
 * The YAML lines for the inputs that moved far enough.
 *
 * @param inputs - Every analog input, by index.
 * @param seen - The ranges seen.
 * @returns The snippet and the names of the inputs that did not move enough.
 */
export function calibrationSnippet(
  inputs: readonly CalibrationInput[],
  seen: SeenRange,
): { yaml: string; unmoved: string[] } {
  const lines = ['analog:'];
  const unmoved: string[] = [];
  inputs.forEach((input, i) => {
    const range = seen[i] ?? null;
    if (range === null || range.max - range.min < CALIBRATION_MIN_SPAN) {
      unmoved.push(input.name);
      return;
    }
    const low = Math.min(ADC_TOP, range.min + CALIBRATION_INSET);
    const high = Math.max(0, range.max - CALIBRATION_INSET);
    const [adcMin, adcMax] = input.isReversed ? [high, low] : [low, high];
    const key = input.id === null ? `name: ${yamlScalar(input.name)}` : `id: ${input.id}`;
    lines.push(`  - { ${key}, adc_min: ${adcMin}, adc_max: ${adcMax} }`);
  });
  return { yaml: lines.length === 1 ? '' : `${lines.join('\n')}\n`, unmoved };
}

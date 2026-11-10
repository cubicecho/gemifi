export type ProgressBaseProps = {
  /**
   * How much is done, from 0 to `max`. Clamped to that range. Left out (or `null`), the bar is
   * indeterminate: a third of the track sweeping across it, and announced with no value, as
   * Radix's is. Under reduced motion the sweep holds still in the middle, off the start, so it is
   * never read as an amount.
   */
  value?: number | null | undefined;
  /** What `value` is out of. 100 unless given, so a percentage needs nothing. */
  max?: number | undefined;
  /** What is progressing — "Re-embedding progress". The bar's accessible name. */
  label?: string | undefined;
  /**
   * The value in words, read instead of the bare number: "1,204 of 5,880 turns". Without it a
   * screen reader says the percentage, which is right when the number means nothing else.
   */
  valueLabel?: string | undefined;
  /** On the track, the full-width bar the fill is drawn in. A different height is an `h-*` here. */
  className?: string | undefined;
  /** The filled part: `bg-negative` for a context window nearly full. */
  indicatorClassName?: string | undefined;
};

/** What `max` is when nobody gives one, which makes `value` a percentage. */
export const DEFAULT_MAX = 100;
const PERCENT = 100;

/**
 * The value as the bar draws and announces it: `max` made usable, `value` clamped into it, and
 * the share of the track to fill. `now` is `undefined` for an indeterminate bar.
 */
export function progressState(
  value: number | null | undefined,
  max: number,
): { limit: number; now: number | undefined; percent: number } {
  // A zero or negative `max` has no fraction to draw; treat it as nothing done rather than NaN.
  const limit = max > 0 ? max : DEFAULT_MAX;
  const known = typeof value === "number" && Number.isFinite(value);
  const now = known ? Math.min(Math.max(value, 0), limit) : undefined;
  const percent = now === undefined ? 0 : (now / limit) * PERCENT;
  return { limit, now, percent };
}

/**
 * The track. Its `/50` edge is what shows the bar's whole length: 3:1 from the page and a card in
 * every palette, the edge a control has, where the `/10` fill alone was 1.24:1. The fill inside
 * stays `/10`, because a `/50` fill would leave the indicator 2:1 from it in Monokai.
 */
export const progressTrackClass =
  "h-2 w-full overflow-hidden rounded-full border border-foreground/50 bg-foreground/10";

/** The indicator, determinate or sweeping. */
export const progressIndicatorClass = "h-full bg-neutral";

/** The indeterminate segment's share of the track: a third, so it reads as a part moving. */
export const SWEEP_WIDTH = "33.333%";

/** One sweep, from off the start to off the end. */
export const SWEEP_DURATION_MS = 1500;

/**
 * Where the segment is, in its own widths, at the start and the end of a sweep: one width before
 * the track, and past its end. It is a third of the track, so three widths is the end.
 */
export const SWEEP_FROM = -1;
export const SWEEP_TO = 3;

/** Where it rests under reduced motion, in its own widths: the middle third. */
export const SWEEP_REST = 1;

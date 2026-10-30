import type * as React from "react";
import { cn } from "@/lib/utils";

type ViewProps = Omit<React.ComponentPropsWithoutRef<"div">, "className" | "children"> & {
  className?: string | undefined;
};

export type ProgressProps = ViewProps & {
  /**
   * How much is done, from 0 to `max`. Clamped to that range. Left out (or `null`), the bar is
   * indeterminate: drawn empty and announced with no value, as Radix's is — not animated.
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
  /** The filled part: `bg-negative` for a context window nearly full. */
  indicatorClassName?: string | undefined;
};

/** What `max` is when nobody gives one, which makes `value` a percentage. */
const DEFAULT_MAX = 100;
const PERCENT = 100;

/** A horizontal bar showing how much of something is done. */
export function Progress({
  value,
  max = DEFAULT_MAX,
  label,
  valueLabel,
  className,
  indicatorClassName,
  ...props
}: ProgressProps) {
  // A zero or negative `max` has no fraction to draw; treat it as nothing done rather than NaN.
  const limit = max > 0 ? max : DEFAULT_MAX;
  const known = typeof value === "number" && Number.isFinite(value);
  const now = known ? Math.min(Math.max(value, 0), limit) : undefined;
  const percent = now === undefined ? 0 : (now / limit) * PERCENT;

  return (
    <div
      data-slot="progress"
      role="progressbar"
      aria-valuemin={0}
      aria-valuemax={limit}
      {...(now === undefined ? {} : { "aria-valuenow": now })}
      {...(valueLabel ? { "aria-valuetext": valueLabel } : {})}
      {...(props as React.ComponentPropsWithoutRef<"div">)}
      {...(label ? { "aria-label": label } : {})}
      className={cn(
        "cube-rn-view",
        "h-2 w-full overflow-hidden rounded-full bg-foreground/10",
        className,
      )}
    >
      <div
        data-slot="progress-indicator"
        className={cn(
          "cube-rn-view",
          "h-full bg-neutral",
          "transition-[width]",
          indicatorClassName,
        )}
        style={{ width: `${percent}%` }}
      />
    </div>
  );
}

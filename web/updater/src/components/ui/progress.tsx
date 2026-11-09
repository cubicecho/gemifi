import type * as React from "react";
import { useEffect, useRef } from "react";
import {
  DEFAULT_MAX,
  type ProgressBaseProps,
  progressIndicatorClass,
  progressState,
  progressTrackClass,
  SWEEP_DURATION_MS,
  SWEEP_FROM,
  SWEEP_REST,
  SWEEP_TO,
  SWEEP_WIDTH,
} from "@/components/ui/progress-base";
import { cn } from "@/lib/utils";
import { useReducedMotion } from "@/components/ui/reduced-motion";

export type ProgressProps = Omit<React.ComponentPropsWithoutRef<"div">, "className" | "children"> &
  ProgressBaseProps;

const PERCENT = 100;

/** A translate of the segment by this many of its own widths. */
const shift = (widths: number) => `translateX(${widths * PERCENT}%)`;

/** A third of the track, sweeping from before its start to past its end. */
function Sweep({ className }: { className?: string | undefined }) {
  const reducedMotion = useReducedMotion();
  const segment = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const element = segment.current;
    // `animate` is missing from a test DOM without the Web Animations API; the segment rests.
    if (reducedMotion || !element || typeof element.animate !== "function") {
      return;
    }
    const sweep = element.animate(
      [{ transform: shift(SWEEP_FROM) }, { transform: shift(SWEEP_TO) }],
      {
        duration: SWEEP_DURATION_MS,
        easing: "ease-in-out",
        iterations: Number.POSITIVE_INFINITY,
      },
    );
    return () => sweep.cancel();
  }, [reducedMotion]);

  return (
    <div
      ref={segment}
      data-slot="progress-indicator"
      className={cn(progressIndicatorClass, className)}
      style={{ width: SWEEP_WIDTH, transform: shift(SWEEP_REST) }}
    />
  );
}

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
  const { limit, now, percent } = progressState(value, max);
  // Radix's three states, so a shadcn call site's `data-[state=complete]:` still matches.
  const state = now === undefined ? "indeterminate" : now === limit ? "complete" : "loading";

  return (
    <div
      data-slot="progress"
      data-state={state}
      role="progressbar"
      aria-valuemin={0}
      aria-valuemax={limit}
      {...(now === undefined ? {} : { "aria-valuenow": now })}
      {...(valueLabel ? { "aria-valuetext": valueLabel } : {})}
      {...props}
      {...(label ? { "aria-label": label } : {})}
      className={cn(progressTrackClass, className)}
    >
      {now === undefined ? (
        <Sweep className={indicatorClassName} />
      ) : (
        <div
          data-slot="progress-indicator"
          className={cn(
            progressIndicatorClass,
            "transition-[width] motion-reduce:transition-none",
            indicatorClassName,
          )}
          style={{ width: `${percent}%` }}
        />
      )}
    </div>
  );
}

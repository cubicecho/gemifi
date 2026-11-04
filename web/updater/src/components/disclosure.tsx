import type { ReactNode } from "react";
import * as React from "react";
import { cn, type SlotNode } from "@/lib/utils";
import { ChevronRight } from "@/components/ui/icons";

type DisclosureProps = {
  /**
   * What the button says: "Completed (3)", "Raw output". It is the button's accessible name. A
   * string is drawn as the title; an element is placed as it is and styles itself.
   */
  title: ReactNode;
  /** One line under the title, inside the button, drawn whether open or shut. */
  description?: ReactNode | undefined;
  /**
   * The header's far end, **outside** the button: a copy button, a count, a clear-all. A control
   * nested in a button is invalid HTML and, in practice, a click that also toggles the section.
   */
  actionSlot?: SlotNode | undefined;
  /** What opening shows. Not mounted while shut, so a long list behind it costs nothing. */
  contentSlot?: SlotNode | undefined;
  /**
   * Whether it is open, when the caller holds that — a deep link, a "show the failure" button
   * elsewhere, a title that reads "Hide" once open. Leave it out and the disclosure holds its own.
   */
  open?: boolean | undefined;
  /** Told on every toggle, controlled or not. Given alone, it listens without taking over. */
  onOpenChange?: ((open: boolean) => void) | undefined;
  /** Where an uncontrolled disclosure starts. Shut by default, the way `<details>` is. */
  defaultOpen?: boolean | undefined;
  className?: string | undefined;
  headerClassName?: string | undefined;
  titleClassName?: string | undefined;
  contentClassName?: string | undefined;
};

/**
 * A string gets a `Text` around it; an element is placed as it is, because a `View` inside a
 * `Text` lays out inline on device and loses most of its own styling.
 */
function asText(node: ReactNode, className: string, testID: string) {
  return typeof node === "string" || typeof node === "number" ? (
    <span data-slot={testID} className={cn("cube-rn-text", className)}>
      {node}
    </span>
  ) : (
    node
  );
}

/**
 * A titled part of a screen whose body shows and hides: "Show completed (3)" under a list. The
 * look is a muted `text-sm` line with the chevron before it; `titleClassName` makes the title the
 * foreground where the disclosure is a section's heading.
 *
 * The whole header is one button carrying `aria-expanded`, and the chevron turns off the same
 * boolean. `actionSlot` sits outside that button, because a button in a button is invalid. Open
 * is uncontrolled with `defaultOpen` or controlled with `open`, and `onOpenChange` is heard
 * either way.
 */
export function Disclosure({
  title,
  description,
  actionSlot,
  contentSlot,
  open: openProp,
  onOpenChange,
  defaultOpen = false,
  className,
  headerClassName,
  titleClassName,
  contentClassName,
}: DisclosureProps) {
  const [ownOpen, setOwnOpen] = React.useState(defaultOpen);
  const open = openProp ?? ownOpen;
  const contentId = React.useId();
  const shown = open && contentSlot !== undefined && contentSlot !== null && contentSlot !== false;

  const toggle = () => {
    if (openProp === undefined) {
      setOwnOpen(!open);
    }
    onOpenChange?.(!open);
  };

  return (
    <div data-slot="disclosure" className={cn("cube-rn-view", "min-w-0 gap-2", className)}>
      <div
        data-slot="disclosure-header"
        className={cn("cube-rn-view", "min-w-0 flex-row items-start gap-2", headerClassName)}
      >
        <button
          type="button"
          data-slot="disclosure-trigger"
          aria-expanded={open}
          // Web only: React Native has no `aria-controls`, and pointing it at an id that is not in
          // the document is a broken reference rather than a hint — the body unmounts when shut.
          {...(shown ? { "aria-controls": contentId } : {})}
          onClick={toggle}
          className={cn(
            "cube-rn-view cube-rn-pressable",
            "min-w-0 flex-1 flex-row items-start gap-2 rounded-sm",
            "text-left focus-visible:bg-hover focus-visible:outline-none",
          )}
        >
          <ChevronRight
            aria-hidden
            className={cn(
              "mt-0.5 size-4 shrink-0 text-foreground/60",
              "transition-transform",
              open && "rotate-90",
            )}
          />
          <div className="cube-rn-view min-w-0 flex-1 gap-0.5">
            {asText(
              title,
              cn("font-semibold text-foreground/60 text-sm", titleClassName),
              "disclosure-title",
            )}
            {description
              ? asText(description, "text-foreground/60 text-xs", "disclosure-description")
              : null}
          </div>
        </button>
        {actionSlot ? (
          <div
            data-slot="disclosure-action"
            className="cube-rn-view shrink-0 flex-row items-center gap-1"
          >
            {actionSlot}
          </div>
        ) : null}
      </div>

      {shown ? (
        <div
          data-slot="disclosure-content"
          id={contentId}
          // Indented to the title (the 16px chevron and its 8px gap), so the body reads as the
          // title's and not as the next thing on the page.
          className={cn("cube-rn-view", "min-w-0 gap-2 pl-6", contentClassName)}
        >
          {contentSlot}
        </div>
      ) : null}
    </div>
  );
}

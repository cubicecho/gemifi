import type { ReactNode } from "react";
import { useCallback, useEffect, useRef, useState } from "react";
import { ConfirmDialog } from "@/components/ui/confirm-dialog";

/**
 * A router's navigation blocker, as far as the guard reads it.
 *
 * Structural, so no router is imported here: TanStack Router's `useBlocker({ withResolver: true })`
 * answers with `status`, React Router's `useBlocker` with `state`, and both say `"blocked"` and
 * carry `proceed` and `reset`. Either is passed as it comes.
 */
export type NavigationBlocker = {
  status?: string | undefined;
  state?: string | undefined;
  proceed?: (() => void) | undefined;
  reset?: (() => void) | undefined;
};

type UnsavedChangesGuardOptions = {
  /** Whether there is anything to lose. A function is asked at the moment of leaving. */
  hasUnsavedChanges: boolean | (() => boolean);
  /**
   * The router's blocker, when the app has a router. While it is blocked the question is asked;
   * *Discard* calls its `proceed` and *Keep editing* its `reset`. Without one, only `leave` and
   * closing the tab are guarded.
   */
  blocker?: NavigationBlocker | undefined;
};

/** What `useUnsavedChangesGuard` returns: the question's state and its two answers. */
export type UnsavedChangesGuard = {
  /**
   * Runs `go` now if nothing would be lost, and after *Discard* if something would. For the
   * page's own Close and Cancel buttons, and for a native navigator's "before remove" event.
   */
  leave: (go: () => void) => void;
  /** Whether the question is being asked. */
  asking: boolean;
  /** *Discard*: the held navigation, or the held `go`, goes ahead. */
  discard: () => void;
  /** *Keep editing*: the held navigation is dropped. */
  stay: () => void;
};

/** What a web page's window answers to here; the device has neither. */
type UnloadTarget = {
  addEventListener: (type: "beforeunload", listener: (event: UnloadEvent) => void) => void;
  removeEventListener: (type: "beforeunload", listener: (event: UnloadEvent) => void) => void;
};
type UnloadEvent = { preventDefault: () => void; returnValue?: unknown };

/**
 * The state behind "leave with unsaved edits?", for a page.
 *
 * A dialog has `DialogLayout`'s `hasUnsavedChanges`. A page has three ways out and no shell that
 * owns them: a router navigation, its own Close button, and closing the tab. Each app wired the
 * router's blocker to a dialog, added a second piece of state for the Close button and merged the
 * two into one `open`. This is that, once, with no router in it: the blocker is handed in as
 * whatever the app's router returned.
 *
 * Closing the tab is covered on the web by a `beforeunload` listener, held only while there are
 * changes — a page that always has one loses the browser's back/forward cache. The browser words
 * that question itself. A device has no tab to close; its back gesture is the navigator's
 * "before remove" event, which calls `leave`.
 */
export function useUnsavedChangesGuard({
  hasUnsavedChanges,
  blocker,
}: UnsavedChangesGuardOptions): UnsavedChangesGuard {
  // The `go` a Close button is waiting on. In state, not a ref, because holding one is what
  // opens the question.
  const [held, setHeld] = useState<(() => void) | undefined>(undefined);
  const isDirty = useRef(hasUnsavedChanges);
  isDirty.current = hasUnsavedChanges;

  const dirtyNow = typeof hasUnsavedChanges === "function" ? undefined : hasUnsavedChanges;

  useEffect(() => {
    // A function is asked when the tab closes; a boolean that is false needs no listener at all.
    if (dirtyNow === false) {
      return;
    }
    const target = globalThis as unknown as UnloadTarget;
    const ask = (event: UnloadEvent) => {
      const dirty = typeof isDirty.current === "function" ? isDirty.current() : isDirty.current;
      if (dirty === false) {
        return;
      }
      event.preventDefault();
      // What Chrome reads; `preventDefault` alone is the standard and is not enough there.
      event.returnValue = "";
    };
    target.addEventListener("beforeunload", ask);
    return () => target.removeEventListener("beforeunload", ask);
  }, [dirtyNow]);

  const leave = useCallback((go: () => void) => {
    const dirty = typeof isDirty.current === "function" ? isDirty.current() : isDirty.current;
    if (dirty) {
      // The updater form would call `go` as a function; wrap it so it is stored.
      setHeld(() => go);
    } else {
      go();
    }
  }, []);

  const blocked = blocker?.status === "blocked" || blocker?.state === "blocked";

  const discard = () => {
    setHeld(undefined);
    held?.();
    if (blocked) {
      blocker?.proceed?.();
    }
  };

  const stay = () => {
    setHeld(undefined);
    if (blocked) {
      blocker?.reset?.();
    }
  };

  return { leave, asking: held !== undefined || blocked, discard, stay };
}

type UnsavedChangesDialogProps = {
  /** What `useUnsavedChangesGuard` returned. */
  guard: UnsavedChangesGuard;
  /** The question. `DialogLayout`'s wording by default, so a page and a dialog ask alike. */
  discardTitle?: ReactNode | undefined;
  discardDescription?: ReactNode | undefined;
  /** The destructive button. */
  discardLabel?: ReactNode | undefined;
  /** The other button, which is also what Escape answers. */
  stayLabel?: ReactNode | undefined;
};

/**
 * The question itself: a `ConfirmDialog` opened and answered by the guard. Mount it once on the
 * page, anywhere.
 *
 * ```tsx
 * const blocker = useBlocker({ shouldBlockFn: () => dirty, withResolver: true });
 * const guard = useUnsavedChangesGuard({ hasUnsavedChanges: dirty, blocker });
 *
 * <Button variant="outline" onPress={() => guard.leave(onClose)} content="Close" />
 * <UnsavedChangesDialog guard={guard} />
 * ```
 */
export function UnsavedChangesDialog({
  guard,
  discardTitle = "Discard your changes?",
  discardDescription = "What you have typed here will not be saved.",
  discardLabel = "Discard",
  stayLabel = "Keep editing",
}: UnsavedChangesDialogProps) {
  return (
    <ConfirmDialog
      open={guard.asking}
      onOpenChange={(open) => {
        if (open === false) {
          guard.stay();
        }
      }}
      title={discardTitle}
      description={discardDescription}
      confirmLabel={discardLabel}
      cancelLabel={stayLabel}
      onConfirm={guard.discard}
    />
  );
}

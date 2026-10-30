import { createContext, type ReactNode, useLayoutEffect, useRef, useState } from "react";
import { cn, type SlotNode } from "@/lib/utils";

/** Monospaced text inline in a sentence. */
export function Code({
  className,
  children,
}: {
  className?: string | undefined;
  children: ReactNode;
}) {
  return (
    <span
      className={cn(
        "cube-rn-text",
        "rounded-sm bg-foreground/10 px-1 py-0.5 font-mono text-xs text-foreground",
        className,
      )}
    >
      {children}
    </span>
  );
}

/** The heights a block may be capped at: ten, fifteen and twenty-four rem. */
const MAX_HEIGHTS = {
  sm: "max-h-40",
  md: "max-h-60",
  lg: "max-h-96",
} as const;

/**
 * The title of the `CodeBlock` around whatever reads it. `CopyButton` does: inside a titled block
 * its default name is "Copy .mcp.json" rather than a bare "Copy", which on a page of snippets
 * names nothing.
 */
export const CodeBlockTitleContext = createContext<string | undefined>(undefined);

/** The block's type, which is `Code`'s: the two are one family and should not drift apart. */
const TEXT = "font-mono text-xs text-foreground";

/**
 * One line, with an ellipsis: `truncate` on the web, `line-clamp-1` on device, which is what
 * becomes `numberOfLines` there.
 */
const ONE_LINE = "truncate";

export type CodeBlockProps = {
  /**
   * The text, drawn as written: its newlines and its indentation are kept. A string rather than
   * a node, because whitespace is the one thing a block of code cannot have rearranged, and a
   * node would let a caller pass markup that is not preformatted at all.
   */
  content: string;
  /**
   * The block's far end, at the top: a `CopyButton`. It sits beside the text rather than over
   * it, so the room it needs is the block's to reserve — the hand-written version floated the
   * button over the text and remembered a `pr-12` to keep the first line out from under it.
   */
  actionSlot?: SlotNode;
  /**
   * What a line too long for the block does. Off, the default, it runs on and the block scrolls
   * sideways, which is right for code and config: a wrapped line of JSON reads as two. On, it
   * wraps, breaking inside a word if it has to, which is right for output, a log, or one long
   * value such as a URL.
   */
  wrap?: boolean | undefined;
  /**
   * The tallest the block may get before it scrolls inside itself. Left out, it is as tall as
   * its text. A named height rather than a class, so the blocks in an app are one of three
   * heights instead of a `max-h-*` each.
   */
  maxHeight?: keyof typeof MAX_HEIGHTS | undefined;
  /**
   * What the snippet is: a file name, a tool, a shell — ".mcp.json", "Claude Code", "curl". Drawn
   * in a header row of its own, with the `actionSlot` beside it rather than beside the text, and
   * it names a `CopyButton` inside the block "Copy {title}" unless that button has a `label`.
   */
  title?: string | undefined;
  /** The snippet's language, drawn muted after the `title` — "json", "bash". Only with a title. */
  language?: string | undefined;
  /**
   * Keep the end of a growing `content` in view — a live log, a run's output. Meaningful with
   * `maxHeight`, since only a capped block scrolls. It follows while the reader is at the end, lets
   * go once they scroll up, and shows "Jump to latest" until they are back. The text is never a
   * live region: a log read aloud line by line drowns out everything else.
   */
  follow?: boolean | undefined;
  /** The block's root: its width, its margin. */
  className?: string | undefined;
};

/** How close to the end, in pixels, still counts as at it: a fractional `scrollTop` is common. */
const END_SLACK = 4;

/** The part of the web's scrolling `<div>` that following reads and moves. */
type WebScroller = {
  scrollTop: number;
  scrollHeight: number;
  clientHeight: number;
  scrollTo: (options: { top: number; behavior: "smooth" | "instant" }) => void;
};

/** The part of a DOM scroll event following reads. */
type WebScrollEvent = { currentTarget: Omit<WebScroller, "scrollTo"> };

/**
 * Moves a scroller to its end. Statements, for the reason {@link CodeBlockText} is: the compiler
 * keeps the web arm and drops the `ScrollView` call under it.
 */
function toEnd(scroller: unknown, animated: boolean) {
  const box = scroller as WebScroller;
  box.scrollTo({ top: box.scrollHeight, behavior: animated ? "smooth" : "instant" });
  return;
}

/** What {@link CodeBlockText} needs to follow: where its scroller is and whether it is at the end. */
type Following = {
  scroller: { current: unknown };
  /** Called on every scroll with whether the reader is now at the end. */
  onAtEnd: (atEnd: boolean) => void;
  /** Called on device when the text grows, which is when a follower moves to the new end. */
  onGrow: () => void;
};

type CodeBlockTextProps = Pick<CodeBlockProps, "content" | "wrap" | "maxHeight"> & {
  following?: Following | undefined;
};

/**
 * The scrolling text. The one part the platforms draw with different elements.
 *
 * On the web it is a `<pre>` in one box that scrolls both ways. On device `overflow` scrolls
 * nothing and a `Text` always wraps, so the height is a `ScrollView` and a line that must not
 * wrap needs a second, horizontal one inside it.
 *
 * Written as statements rather than a ternary because the two arms are different elements, and
 * the compiler refuses an element chosen at runtime — it folds `Platform.OS === "web"` to `true`,
 * keeps the first arm, and drops the `ScrollView`s below it as unreachable.
 */
function CodeBlockText({ content, wrap, maxHeight, following }: CodeBlockTextProps) {
  const height = maxHeight ? MAX_HEIGHTS[maxHeight] : undefined;
  return (
    <div
      data-slot="code-block-content"
      // A region that scrolls has to be reachable by keyboard, or only a mouse wheel can read
      // the end of the line (axe `scrollable-region-focusable`). A block that wraps and has no
      // cap cannot scroll, so it takes no tab stop — which is what keeps a one-line value
      // beside its copy button from being two stops.
      tabIndex={wrap && !maxHeight ? undefined : 0}
      // A view's own props have no scroll event (on device a view does not scroll), so the
      // listener and the ref go on as a spread, untyped. The element under it is a `<div>`.
      {...(following
        ? {
            ref: (node: unknown) => {
              following.scroller.current = node;
            },
            "aria-live": "off" as const,
            onScroll: ({ currentTarget: box }: WebScrollEvent) =>
              following.onAtEnd(box.scrollHeight - box.scrollTop - box.clientHeight <= END_SLACK),
          }
        : {})}
      className={cn("cube-rn-view", "min-w-0 flex-1 overflow-auto p-3", height)}
    >
      <pre
        className={cn(
          "cube-rn-text",
          TEXT,
          // `self-start` lets an unwrapped line be as wide as it is, so the box scrolls to the
          // end of it with the padding still there.
          wrap ? "whitespace-pre-wrap wrap-anywhere" : "self-start whitespace-pre",
        )}
      >
        {content}
      </pre>
    </div>
  );
}

/**
 * A block of preformatted text: a config file, a command, a payload, a log.
 *
 * - `content` is a string and is drawn as written. There is no syntax highlighting.
 * - `actionSlot` is the far end, top-aligned, in a column of its own — the text never runs under
 *   it.
 * - `wrap` chooses between wrapping a long line and scrolling sideways (the default).
 * - `maxHeight` caps the block, which then scrolls inside itself.
 * - `title` (and `language`) label the block in a header row, which then holds the action.
 * - `follow` keeps a growing block at its end while the reader is there.
 *
 * A value to copy — an endpoint, a token, a command — is this with `wrap` and a `CopyButton`
 * as its `actionSlot`, not a second component: one line of text is a short block.
 */
export function CodeBlock({
  content,
  actionSlot,
  wrap = false,
  maxHeight,
  title,
  language,
  follow = false,
  className,
}: CodeBlockProps) {
  const scroller = useRef<unknown>(null);
  // Whether the reader is at the end, which is whether the block follows. It starts there.
  const [atEnd, setAtEnd] = useState(true);

  // On the web new text is laid out by the time this runs, so the end is where it now is. On
  // device the content size is not known yet; `onContentSizeChange` does it there.
  // biome-ignore lint/correctness/useExhaustiveDependencies: new `content` is when to follow.
  useLayoutEffect(() => {
    if (follow && atEnd && scroller.current) {
      toEnd(scroller.current, false);
    }
  }, [content, follow, atEnd]);

  const following: Following | undefined = follow
    ? {
        scroller,
        onAtEnd: setAtEnd,
        onGrow: () => {
          if (atEnd && scroller.current) {
            toEnd(scroller.current, false);
          }
        },
      }
    : undefined;

  const action = actionSlot ? (
    // `p-0.5` and not the text's `p-3`: an icon button is as tall as a line of text and its
    // padding together, so a one-line block stays one line tall with the button in it.
    <div data-slot="code-block-action" className="cube-rn-view shrink-0 p-0.5">
      {actionSlot}
    </div>
  ) : null;
  const text = (
    <CodeBlockText content={content} wrap={wrap} maxHeight={maxHeight} following={following} />
  );
  const jump =
    follow && atEnd === false ? (
      <button
        type="button"
        data-slot="code-block-jump"
        onClick={() => {
          setAtEnd(true);
          if (scroller.current) {
            toEnd(scroller.current, true);
          }
        }}
        className="cube-rn-view cube-rn-pressable absolute right-2 bottom-2 rounded-md border border-foreground/10 bg-background px-2 py-1 shadow-sm"
      >
        <span className="cube-rn-text text-xs text-foreground">Jump to latest</span>
      </button>
    ) : null;

  if (title === undefined) {
    return (
      <div
        data-slot="code-block"
        className={cn(
          // The tint is the boundary; a border on top of it only darkened the edge.
          "cube-rn-view",
          "flex-row items-start rounded-md bg-foreground/10",
          follow && "relative",
          className,
        )}
      >
        {text}
        {action}
        {jump}
      </div>
    );
  }

  return (
    <CodeBlockTitleContext.Provider value={title}>
      <div
        data-slot="code-block"
        className={cn("cube-rn-view", "rounded-md bg-foreground/10", className)}
      >
        <div
          data-slot="code-block-header"
          // As tall as the `icon-sm` button it may hold and that button's `p-0.5`, so a titled block
          // with no action has the same header.
          className="cube-rn-view min-h-10 flex-row items-center gap-2 border-b border-foreground/10 pl-3"
        >
          <span
            className={cn("cube-rn-text", ONE_LINE, "shrink text-xs font-semibold text-foreground")}
          >
            {title}
          </span>
          {language === undefined ? null : (
            <span className="cube-rn-text shrink-0 text-xs text-foreground/60">{language}</span>
          )}
          <div className="cube-rn-view flex-1" />
          {action}
        </div>
        {/* The jump sits over the text, not over the header. */}
        <div className={cn("cube-rn-view", "flex-row", follow && "relative")}>
          {text}
          {jump}
        </div>
      </div>
    </CodeBlockTitleContext.Provider>
  );
}

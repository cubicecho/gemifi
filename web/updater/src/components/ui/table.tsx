"use client";

import * as React from "react";

import { cn } from "@/lib/utils";

/**
 * Whether a `TableHead` sits in the header row group, so it can default its `scope` to the
 * direction it actually labels. Upstream sets no `scope` and leaves the browser to infer one,
 * which the header row gets right and a row header in the body — the first cell of each row,
 * as a `th` — is left to each screen reader's heuristics. Saying it costs one attribute.
 */
const InTableHeader = React.createContext(false);

/**
 * Upstream's container is `overflow-x-auto` and nothing else, which makes a table wider than its
 * column a region only a mouse wheel can move: axe reports it as `scrollable-region-focusable`,
 * and a keyboard user cannot read the columns past the edge. The container takes a tab stop —
 * the fix that rule asks for, and the one `HeaderContentFooter`'s scrolling body makes — but only
 * while it actually overflows, so a table that fits is not a stray stop in every page's tab order.
 */
function useOverflowsX() {
  const ref = React.useRef<HTMLDivElement>(null);
  const [overflows, setOverflows] = React.useState(false);
  React.useEffect(() => {
    const node = ref.current;
    if (!node || typeof ResizeObserver === "undefined") {
      return;
    }
    const measure = () => setOverflows(node.scrollWidth > node.clientWidth);
    const observer = new ResizeObserver(measure);
    observer.observe(node);
    // The container keeps its width when a column grows, so the table is watched as well.
    if (node.firstElementChild) {
      observer.observe(node.firstElementChild);
    }
    measure();
    return () => observer.disconnect();
  }, []);
  return [ref, overflows] as const;
}

/** A table inside a container that scrolls sideways when it is too wide. */
function Table({ className, ...props }: React.ComponentProps<"table">) {
  const [ref, overflows] = useOverflowsX();
  return (
    <div
      ref={ref}
      data-slot="table-container"
      tabIndex={overflows ? 0 : undefined}
      className="relative w-full overflow-x-auto"
    >
      <table
        data-slot="table"
        // `border-collapse` here rather than trusting Tailwind's preflight to set it: without it
        // a row's border does not paint, and an app that skips preflight drew no row rules.
        // `tabular-nums` so a column of figures lines up by digit.
        className={cn("w-full caption-bottom border-collapse text-sm tabular-nums", className)}
        {...props}
      />
    </div>
  );
}

// Every border below names `border-foreground/10` beside its width. Upstream leans on the app's base
// layer to colour a bare `border-b`, and the cubeui stylesheet sets no such rule, so without it
// the rules between rows are `currentColor` — black lines in a light theme.
/** The table's header row group. */
function TableHeader({ className, ...props }: React.ComponentProps<"thead">) {
  return (
    <InTableHeader.Provider value={true}>
      <thead
        data-slot="table-header"
        className={cn("[&_tr]:border-foreground/10 [&_tr]:border-b", className)}
        {...props}
      />
    </InTableHeader.Provider>
  );
}

/** The table's body row group. */
function TableBody({ className, ...props }: React.ComponentProps<"tbody">) {
  return (
    <tbody
      data-slot="table-body"
      className={cn("[&_tr:last-child]:border-0", className)}
      {...props}
    />
  );
}

/** The table's footer row group, for totals. */
function TableFooter({ className, ...props }: React.ComponentProps<"tfoot">) {
  return (
    <tfoot
      data-slot="table-footer"
      className={cn(
        "border-foreground/10 border-t bg-foreground/10 font-semibold [&>tr]:last:border-b-0",
        className,
      )}
      {...props}
    />
  );
}

/** One row. */
function TableRow({ className, ...props }: React.ComponentProps<"tr">) {
  return (
    <tr
      data-slot="table-row"
      className={cn(
        "border-foreground/10 border-b transition-colors hover:bg-hover has-aria-expanded:bg-foreground/10 data-[state=selected]:bg-active/40",
        className,
      )}
      {...props}
    />
  );
}

/** A header cell, scoped to its column in the header and to its row in the body. */
function TableHead({ className, scope, ...props }: React.ComponentProps<"th">) {
  const inHeader = React.useContext(InTableHeader);
  return (
    <th
      data-slot="table-head"
      scope={scope ?? (inHeader ? "col" : "row")}
      className={cn(
        // A label quieter than the data under it: weight says it is a header, the grey that it is
        // not the content. It was the loudest text in the table.
        "h-10 whitespace-nowrap px-2 text-left align-middle font-semibold text-foreground/60 [&:has([role=checkbox])]:pr-0 [&>[role=checkbox]]:translate-y-[2px]",
        className,
      )}
      {...props}
    />
  );
}

/** A data cell. */
function TableCell({ className, ...props }: React.ComponentProps<"td">) {
  return (
    <td
      data-slot="table-cell"
      className={cn(
        "whitespace-nowrap p-2 align-middle [&:has([role=checkbox])]:pr-0 [&>[role=checkbox]]:translate-y-[2px]",
        className,
      )}
      {...props}
    />
  );
}

/** The table's caption, which is also its accessible name. */
function TableCaption({ className, ...props }: React.ComponentProps<"caption">) {
  return (
    <caption
      data-slot="table-caption"
      className={cn("mt-4 text-foreground/60 text-sm", className)}
      {...props}
    />
  );
}

export { Table, TableBody, TableCaption, TableCell, TableFooter, TableHead, TableHeader, TableRow };

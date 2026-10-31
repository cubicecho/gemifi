/** Text edits the YAML editor makes: indenting at the caret and finding a line. Framework-free. */

/** What Tab inserts: YAML is indented with spaces, two to a level. */
export const INDENT = '  ';

/** A range of characters in the text. */
export interface TextRange {
  start: number;
  end: number;
}

/**
 * @param text - The whole file.
 * @param line - A 1-based line number.
 * @returns Where that line starts and ends, without its newline; past the end, the last line.
 */
export function lineRange(text: string, line: number): TextRange {
  let start = 0;
  for (let current = 1; current < line; current += 1) {
    const next = text.indexOf('\n', start);
    if (next === -1) {
      break;
    }
    start = next + 1;
  }
  const newline = text.indexOf('\n', start);
  return { start, end: newline === -1 ? text.length : newline };
}

/**
 * Replaces the selection with one indent, as a code editor's Tab does.
 *
 * @param text - The whole file.
 * @param selection - What is selected; an empty range is the caret.
 * @returns The new text and where the caret goes.
 */
export function insertIndent(text: string, selection: TextRange): { text: string; caret: number } {
  return {
    text: text.slice(0, selection.start) + INDENT + text.slice(selection.end),
    caret: selection.start + INDENT.length,
  };
}

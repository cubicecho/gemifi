// The key table on a component's docs page, from src/_data/component_keys.json: the keys a component
// declares as data (components/keys.py; `gemifi_build.py keys` writes the file). Its README marks where
// the table goes with `<!-- keys -->`; GitHub shows nothing there.

import fs from "node:fs";
import path from "node:path";
import { slugify } from "./repo-docs.js";

export const KEYS_FILE = path.resolve(import.meta.dirname, "../src/_data/component_keys.json");
export const KEYS_MARKER = "<!-- keys -->";

/** component name -> its sections ({ section, list, platform?, keys: [rows] }). */
export function componentKeys() {
  return JSON.parse(fs.readFileSync(KEYS_FILE, "utf8"));
}

const cell = (s) => String(s).replace(/\|/g, "\\|").replace(/\n/g, " ");

/** A `see` as a Markdown link from the README at `file`: a heading of its own, or `path.md "Heading"`. */
function seeLink(see, file) {
  const m = see.match(/^([\w./-]+\.md)(?: "([^"]+)")?$/);
  if (!m) return `[${see}](#${slugify(see)})`;
  const rel = path.posix.relative(path.posix.dirname(file), m[1]);
  return `[${see.replace(m[1], `\`${m[1]}\``)}](${rel}${m[2] ? `#${slugify(m[2])}` : ""})`;
}

function rows(keys, file, prefix = "") {
  return keys.flatMap((k) => {
    const name = `${prefix}${k.key}`;
    let what = k.doc;
    if (k.param) what += ` *Can be changed on the device* (\`${k.param}\`).`;
    if (k.see) what += ` See ${seeLink(k.see, file)}.`;
    const dflt = k.required ? "**required**" : (k.default ?? ""); // empty: the CSS shows a dash, or drops it on a phone
    const type = k.type.replace(/^(integer|time) /, "$1 "); // "integer 0..7" on one line
    const own = `| \`${name}\` | ${cell(type)} | ${cell(dflt)} | ${cell(what)} |`;
    return [own, ...(k.keys ? rows(k.keys, file, `${name}.`) : [])];
  });
}

function heading(s) {
  if (s.platform) return `\`${s.section}:\` items with \`platform: ${s.platform}\``;
  if (s.list === "either") return `\`${s.section}:\`, or each item of it as a list`;
  return s.list ? `Each \`${s.section}:\` item` : `\`${s.section}:\``;
}

/** The Markdown of a component's key tables (README at `file`): `## Keys`, a table per section. */
export function keysMarkdown(sections, file) {
  const out = ["## Keys", ""];
  for (const s of sections) {
    if (sections.length > 1) out.push(`### ${heading(s)}`, "");
    else out.push(`${heading(s)}:`, "");
    out.push('<div class="keys-table">', "");
    out.push("| Key | Type | Default | What it does |", "|---|---|---|---|", ...rows(s.keys, file), "");
    out.push("</div>", "");
  }
  return out.join("\n");
}

/** A README's text with its key tables in place of the marker. Throws when the marker and the data disagree. */
export function withKeys(text, file, all = componentKeys()) {
  const m = file.match(/^components\/([^/]+)\/README\.md$/);
  const sections = m && all[m[1]];
  const marker = new RegExp(`^${KEYS_MARKER}$`, "m");    // on a line of its own (not in a mention of it)
  const marked = marker.test(text);
  if (sections && !marked) throw new Error(`${file}: its keys are data; put ${KEYS_MARKER} where the table goes`);
  if (!sections && marked) throw new Error(`${file}: ${KEYS_MARKER} but no keys in ${path.basename(KEYS_FILE)}`);
  return marked ? text.replace(marker, () => keysMarkdown(sections, file)) : text;
}

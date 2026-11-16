// The repo's own Markdown (component READMEs, core/README.md, platform notes)
// rendered as site pages, so the docs live next to the code and the site only
// collects them. Used by src/_data/docs.js; the tests in test/ use it too.
//
// A doc's links are rewritten for the site: a link to another doc goes to its
// page (anchors kept, since headings get GitHub's ids), and a link to any other
// file in the repo goes to GitHub.

import fs from "node:fs";
import path from "node:path";
import MarkdownIt from "markdown-it";
import { highlightBlocks } from "./highlight.js";

export const REPO = path.resolve(import.meta.dirname, "../..");

/** GitHub's heading id: lower case, no punctuation but `-` and `_`, spaces to `-`. */
export function slugify(text) {
  return text
    .trim()
    .toLowerCase()
    .replace(/[^\p{L}\p{N}\s_-]/gu, "")
    .replace(/\s/g, "-");
}

/** The site path of a doc: `/docs/` + its folder, + its name unless it is a README. */
export function docUrl(file) {
  const dir = path.posix.dirname(file);
  const base = path.posix.basename(file, ".md");
  const parts = dir === "." ? [] : [dir];
  if (base !== "README") parts.push(base);
  return `/docs/${parts.join("/")}/`.replace(/\/+/g, "/");
}

/** A repo file's text. */
export function readRepo(file) {
  return fs.readFileSync(path.join(REPO, file), "utf8");
}

/** Every `components/<name>/README.md`, sorted by name. */
export function componentFiles() {
  const dir = path.join(REPO, "components");
  return fs
    .readdirSync(dir)
    .filter((name) => fs.existsSync(path.join(dir, name, "README.md")))
    .sort()
    .map((name) => `components/${name}/README.md`);
}

/** A component's group: its own name for a base, "controls", or the base its README links first. */
export function componentGroup(name, markdown) {
  const opening = markdown.replace(/^# .*\n+/, "").split("\n\n")[0];
  if (/^The base component/.test(opening)) return name;
  if (/^A control\b/.test(opening)) return "controls";
  const base = opening.match(/\]\(\.\.\/([a-z0-9_]+)\/README\.md/);
  return base ? base[1] : "other";
}

/**
 * One line on what a component is, from its first sentence with the asides dropped: a control's
 * README says it after "A control (...):", the others' builder module docstrings start with it.
 */
export function componentSummary(name) {
  const readme = readRepo(`components/${name}/README.md`).replace(/^# .*\n+/, "").split("\n\n")[0];
  const init = path.join(REPO, "components", name, "__init__.py");
  const doc = fs.existsSync(init) ? fs.readFileSync(init, "utf8").match(/^\s*(?:#.*\n\s*)*"""([\s\S]*?)"""/) : null;
  const control = /^A control\b/.test(readme);
  let text = control || !doc ? readme : doc[1];
  text = text.replace(/\s+/g, " ").replace(/\[([^\]]*)\]\([^)]*\)/g, "$1").trim();
  // Drop asides: parentheses, nested ones too.
  let prev;
  do {
    prev = text;
    text = text.replace(/ ?\([^()]*\)/g, "");
  } while (text !== prev);
  text = text.replace(new RegExp(`^${name}: `), "").replace(/^A control: /, "");
  text = text.split(/\. /)[0].replace(/\.$/, "");
  text = text.split(/, (?:a |an |the |two )?(?:hub type|platform|control|YAML keys)\b|, ESPHome style|: `/)[0];
  text = text.replace(/`([^`]*)`/g, "$1").replace(/:$/, "").trim();
  return text.charAt(0).toUpperCase() + text.slice(1);
}

/**
 * Markdown for the site. `env` is `{ file, pages, blob, tree, prefix }`: the doc's repo
 * path, the repo paths that have a page (path -> site path), and where the rest goes.
 */
export function markdown() {
  // Bare URLs become links, as on GitHub; only with a scheme, or `gemifi_build.py` would be a link to Paraguay.
  const md = new MarkdownIt({ html: true, linkify: true });
  md.linkify.set({ fuzzyLink: false, fuzzyEmail: false });

  // GitHub's heading ids (with -1, -2 for repeats), and each h2/h3 for the page's contents list.
  md.core.ruler.push("heading_ids", (state) => {
    const seen = new Map();
    const toc = [];
    state.tokens.forEach((tok, i) => {
      if (tok.type !== "heading_open") return;
      const inline = state.tokens[i + 1];
      const text = inline.children.filter((c) => c.type === "text" || c.type === "code_inline").map((c) => c.content).join("");
      let id = slugify(text);
      const n = seen.get(id) ?? 0;
      seen.set(id, n + 1);
      if (n) id = `${id}-${n}`;
      tok.attrSet("id", id);
      if (tok.tag === "h2" || tok.tag === "h3") toc.push({ level: Number(tok.tag[1]), id, text });
    });
    state.env.toc = toc;
  });

  const rewrite = (href, env) => resolveLink(href, env);
  md.core.ruler.push("repo_links", (state) => {
    for (const tok of state.tokens) {
      for (const child of tok.children ?? []) {
        if (child.type === "link_open") child.attrSet("href", rewrite(child.attrGet("href"), state.env));
        if (child.type === "image") child.attrSet("src", rewrite(child.attrGet("src"), state.env, true));
      }
    }
  });

  // Wide tables scroll on their own instead of the page.
  md.renderer.rules.table_open = () => '<div class="table-scroll"><table>\n';
  md.renderer.rules.table_close = () => "</table></div>\n";
  return highlightBlocks(md);
}

/** Where a link in a doc goes on the site. */
export function resolveLink(href, env, raw = false) {
  if (!href || /^[a-z][a-z0-9+.-]*:/i.test(href) || href.startsWith("#") || href.startsWith("/")) return href;
  const [target, frag] = href.split("#");
  const hash = frag !== undefined ? `#${frag}` : "";
  let file = path.posix.normalize(path.posix.join(path.posix.dirname(env.file), target));
  if (file.startsWith("..")) return href;
  file = file.replace(/\/$/, "");
  const page = env.pages.get(file) ?? env.pages.get(`${file}/README.md`);
  if (page) return `${env.prefix}${page.slice(1)}${hash}`;
  const full = path.join(REPO, file);
  const isDir = fs.existsSync(full) && fs.statSync(full).isDirectory();
  if (raw) return `${env.blob}${file}?raw=true`;
  return `${isDir ? env.tree : env.blob}${file}${hash}`;
}

/** A doc page: its Markdown rendered with the links resolved, its title and contents list. */
export function renderDoc(md, file, env, { url, title } = {}) {
  // A licence comment (<!-- SPDX ... -->) may come before the title.
  let text = readRepo(file).replace(/^<!--[\s\S]*?-->\s*/, "");
  const h1 = text.match(/^# (.*)\n/);
  if (h1) text = text.slice(h1[0].length);
  const pageEnv = { ...env, file };
  const html = md.render(text, pageEnv);
  return {
    file,
    url: url ?? docUrl(file),
    title: title ?? (h1 ? h1[1].replace(/`/g, "") : path.posix.basename(path.posix.dirname(file))),
    html,
    toc: pageEnv.toc,
  };
}

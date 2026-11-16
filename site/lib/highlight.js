// Code blocks for every Markdown page (the repo docs in repo-docs.js and the site's own .md pages): coloured
// with highlight.js when the build runs, nothing in the browser, and split into a span per line so a long
// line wraps with a hanging indent under its own indentation instead of scrolling out of sight (site.css
// `.code .line`).

import hljs from "highlight.js/lib/core";
import bash from "highlight.js/lib/languages/bash";
import c from "highlight.js/lib/languages/c";
import ini from "highlight.js/lib/languages/ini";
import java from "highlight.js/lib/languages/java";
import javascript from "highlight.js/lib/languages/javascript";
import json from "highlight.js/lib/languages/json";
import python from "highlight.js/lib/languages/python";
import typescript from "highlight.js/lib/languages/typescript";
import yaml from "highlight.js/lib/languages/yaml";

for (const [name, lang] of Object.entries({ bash, c, ini, java, javascript, json, python, typescript, yaml })) {
  hljs.registerLanguage(name, lang);
}
hljs.registerAliases(["sh", "shell", "console"], { languageName: "bash" });
hljs.registerAliases(["h", "cpp"], { languageName: "c" });
hljs.registerAliases(["yml"], { languageName: "yaml" });
hljs.registerAliases(["js", "mjs"], { languageName: "javascript" });
hljs.registerAliases(["ts"], { languageName: "typescript" });
hljs.registerAliases(["py"], { languageName: "python" });
hljs.registerAliases(["toml"], { languageName: "ini" });

const escape = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");

/**
 * Highlighted HTML split into lines: a span that runs over a line break is closed at the end of the line and
 * opened again on the next, so each line stands alone.
 */
function splitLines(html) {
  const open = [];
  return html.split("\n").map((line) => {
    const text = open.join("") + line;
    for (const [tag] of line.matchAll(/<span[^>]*>|<\/span>/g)) {
      if (tag === "</span>") open.pop();
      else open.push(tag);
    }
    return text + "</span>".repeat(open.length);
  });
}

/** A fenced block as HTML: `<pre class="code">`, a `.line` span per line, each with its indent as `--indent`. */
export function codeBlock(code, info = "") {
  const lang = info.trim().split(/\s+/)[0].toLowerCase();
  const body = code.replace(/\n$/, "");
  const known = lang && hljs.getLanguage(lang);
  const html = known ? hljs.highlight(body, { language: lang, ignoreIllegals: true }).value : escape(body);
  const lines = splitLines(html).map((line, i) => {
    const indent = body.split("\n")[i].match(/^ */)[0].length;
    // The newline stays inside the (block) line, so a copied block keeps its line breaks.
    return `<span class="line" style="--indent:${indent}">${line}\n</span>`;
  });
  const cls = known ? ` class="language-${lang}"` : "";
  return `<pre class="code"><code${cls}>${lines.join("")}</code></pre>\n`;
}

/** Use codeBlock for a markdown-it instance's fenced and indented code blocks. */
export function highlightBlocks(md) {
  md.renderer.rules.fence = (tokens, i) => codeBlock(tokens[i].content, tokens[i].info);
  md.renderer.rules.code_block = (tokens, i) => codeBlock(tokens[i].content);
  return md;
}

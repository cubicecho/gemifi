// Checks of the built site (`npm test` builds it into _site first): every page
// the docs tree promises exists, every link inside the site lands on a page
// and an id that exist, and every link into the repo names a file (and, for
// Markdown, a heading) that is there. Plus the pure helpers in lib/.

import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import site from "../src/_data/site.js";
import tested from "../src/_data/tested.js";
import { buildDocs, GROUPS } from "../lib/docs-tree.js";
import { REPO, componentFiles, componentSummary, docUrl, readRepo, resolveLink, slugify } from "../lib/repo-docs.js";
import { componentKeys, withKeys } from "../lib/keys-table.js";

const OUT = path.resolve(import.meta.dirname, "../_site");
const PREFIX = site.pathPrefix;

/** Every built HTML file, as paths inside _site. */
function htmlFiles(dir = OUT) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) return htmlFiles(p);
    return e.name.endsWith(".html") ? [p] : [];
  });
}

const ids = new Map();
function idsOf(file) {
  if (!ids.has(file)) ids.set(file, new Set([...fs.readFileSync(file, "utf8").matchAll(/\sid="([^"]+)"/g)].map((m) => m[1])));
  return ids.get(file);
}

/** The built file a site path is served from, or null. */
function servedFile(sitePath) {
  const rel = decodeURI(sitePath.slice(PREFIX.length));
  const file = path.join(OUT, rel);
  if (rel === "" || rel.endsWith("/")) return fs.existsSync(path.join(file, "index.html")) ? path.join(file, "index.html") : null;
  return fs.existsSync(file) ? file : null;
}

const repoHeadings = new Map();
/** A repo Markdown file's GitHub heading ids. */
function headingIds(file) {
  if (!repoHeadings.has(file)) {
    const seen = new Map();
    const out = new Set();
    let fence = false;
    for (const line of readRepo(file).split("\n")) {
      if (line.startsWith("```")) fence = !fence;
      const m = !fence && line.match(/^#{1,6} (.*)/);
      if (!m) continue;
      const id = slugify(m[1].replace(/`/g, ""));
      const n = seen.get(id) ?? 0;
      seen.set(id, n + 1);
      out.add(n ? `${id}-${n}` : id);
    }
    for (const m of readRepo(file).matchAll(/<a (?:id|name)="([^"]+)"/g)) out.add(m[1]);
    repoHeadings.set(file, out);
  }
  return repoHeadings.get(file);
}

test("the site was built", () => {
  assert.ok(fs.existsSync(path.join(OUT, "index.html")), "run `npm run build` (npm test does) first");
});

test("the homepage links to the docs, the devices and the platforms", () => {
  const html = fs.readFileSync(path.join(OUT, "index.html"), "utf8");
  for (const p of ["docs/", "devices/", "docs/platforms/"]) assert.ok(html.includes(`href="${PREFIX}${p}"`), p);
});

test("the docs menu opens the section that holds the page, and the search has its index", () => {
  const html = fs.readFileSync(path.join(OUT, "docs/components/encoders/index.html"), "utf8");
  const open = [...html.matchAll(/<details class="docs-nav-section"( open)?>\s*<summary[^>]*>([^<]+)</g)]
    .filter((m) => m[1])
    .map((m) => m[2]);
  assert.deepEqual(open, ["Overview", "Controls"]);
  assert.ok(html.includes("data-pagefind-body"), "the page is indexed");
  assert.ok(!fs.readFileSync(path.join(OUT, "docs/index.html"), "utf8").includes("data-pagefind-body"), "the docs home is not");
  for (const f of ["pagefind-ui.js", "pagefind-ui.css", "pagefind-entry.json"]) assert.ok(fs.existsSync(path.join(OUT, "pagefind", f)), f);
});

test("a link to a docs page says the page's name, not its repo path", () => {
  const encoders = fs.readFileSync(path.join(OUT, "docs/components/encoders/index.html"), "utf8");
  assert.ok(encoders.includes(`<a href="${PREFIX}docs/core/#controls">Device YAML › Controls</a>`));
  const bad = [];
  for (const file of htmlFiles()) {
    const html = fs.readFileSync(file, "utf8");
    for (const [, href, text] of html.matchAll(/<a href="([^"]*)">([^<]*\.md\b[^<]*)<\/a>/g)) {
      if (href.startsWith(`${PREFIX}docs/`)) bad.push(`${path.relative(OUT, file)}: ${text}`);
    }
  }
  assert.deepEqual(bad, []);
});

test("every page names its own address, and the sitemap lists every page", () => {
  const sitemap = fs.readFileSync(path.join(OUT, "sitemap.xml"), "utf8");
  assert.ok(sitemap.startsWith("<?xml"), "sitemap.xml starts with its XML declaration");
  const listed = new Set([...sitemap.matchAll(/<loc>([^<]+)<\/loc>/g)].map((m) => m[1]));
  const moved = (html) => html.includes('http-equiv="refresh"');
  for (const file of htmlFiles()) {
    const html = fs.readFileSync(file, "utf8");
    const rel = path.relative(OUT, file);
    if (rel === "404.html" || moved(html)) {
      assert.ok(html.includes('name="robots" content="noindex"'), `${rel}: noindex`);
      continue;
    }
    const here = `${site.origin}${PREFIX}${rel.replace(/index\.html$/, "")}`;
    assert.ok(html.includes(`<link rel="canonical" href="${here}" />`), `${rel}: canonical`);
    assert.ok(html.includes(`<meta property="og:url" content="${here}" />`), `${rel}: og:url`);
    assert.ok(listed.has(here), `${rel}: not in sitemap.xml`);
    listed.delete(here);
  }
  assert.deepEqual([...listed], [], "sitemap.xml lists pages that aren't built");
});

test("the pages that moved into the docs send old links, and their sections, to pages and ids that exist", () => {
  for (const old of ["components/", "platforms/"]) {
    const html = fs.readFileSync(servedFile(PREFIX + old), "utf8");
    const to = html.match(/http-equiv="refresh" content="0; url=([^"]+)"/)?.[1];
    assert.ok(to && servedFile(to), `${old}: refresh to ${to}`);
    const hashes = JSON.parse(html.match(/const to = (\{.*\})\[/)[1]);
    assert.ok(Object.keys(hashes).length, `${old}: no section links`);
    for (const [hash, target] of Object.entries(hashes)) {
      const [p, frag] = target.split("#");
      const file = servedFile(PREFIX + p.slice(1));
      assert.ok(file, `${old}#${hash}: no page ${p}`);
      if (frag) assert.ok(idsOf(file).has(frag), `${old}#${hash}: no id ${frag} on ${p}`);
    }
  }
});

test("every component README has a docs page in a known group", () => {
  const docs = buildDocs();
  for (const file of componentFiles()) {
    const name = file.split("/")[1];
    assert.ok(servedFile(`${PREFIX}docs/components/${name}/`), `${name}: no page`);
    const page = docs.pages.find((p) => p.file === file);
    assert.notEqual(page.group, "Other", `${name}: its README's opening line names no base (see lib/docs-tree.js GROUPS)`);
    assert.ok(componentSummary(name).length > 10, `${name}: no summary from its __init__.py docstring`);
  }
  assert.deepEqual(
    docs.groups.map((g) => g.key),
    GROUPS.map((g) => g.key).filter((k) => docs.groups.some((g) => g.key === k)),
  );
});

test("a component whose keys are data has a key table on its page, from component_keys.json", () => {
  const all = componentKeys();
  assert.ok(all.encoders && all.tm1637, Object.keys(all).join(", "));
  for (const [name, sections] of Object.entries(all)) {
    const html = fs.readFileSync(servedFile(`${PREFIX}docs/components/${name}/`), "utf8");
    assert.ok(idsOf(servedFile(`${PREFIX}docs/components/${name}/`)).has("keys"), `${name}: no Keys heading`);
    const keysAt = html.indexOf('id="keys"');
    for (const s of sections) {
      for (const k of s.keys) assert.ok(html.includes(`<td><code>${k.key}</code></td>`, keysAt), `${name}: no row for ${k.key}`);
    }
    // the table sits where the README's marker is: after the YAML example, before the next heading
    assert.ok(keysAt > html.indexOf("<pre"), `${name}: the key table comes before the example`);
  }
  const enc = fs.readFileSync(servedFile(`${PREFIX}docs/components/encoders/`), "utf8");
  assert.match(enc, /<td><code>acceleration\.slow<\/code><\/td>/);
  assert.match(enc, /<em>Can be changed on the device\.<\/em>/);
  assert.throws(() => withKeys("# x\n\n<!-- keys -->\n", "components/nope/README.md", all), /no keys/);
  assert.throws(() => withKeys("# x\n", "components/encoders/README.md", all), /put <!-- keys -->/);
});

test("the base components are in their own section, out of their groups", () => {
  const docs = buildDocs();
  const bases = docs.base.items.map((i) => i.title);
  assert.ok(bases.includes("hub") && bases.includes("display"), bases.join(", "));
  for (const i of docs.base.items) {
    assert.equal(i.of.key, i.title, `${i.title}: the base of another group`);
    for (const g of docs.groups) assert.ok(!g.items.some((x) => x.url === i.url), `${i.title} is also in ${g.title}`);
  }
  for (const g of docs.groups) assert.ok(g.items.length, `${g.title}: empty`);
});

test("every page in the docs tree is built", () => {
  const docs = buildDocs();
  const urls = [
    ...docs.core.map((c) => c.url),
    ...[docs.base, ...docs.groups].flatMap((g) => g.items.map((i) => i.url)),
    ...docs.platforms.flatMap((p) => [p.url, ...p.children.map((c) => c.url)]),
    "/docs/",
    "/docs/devices/",
  ];
  for (const u of urls) assert.ok(servedFile(PREFIX + u.slice(1)), u);
});

test("every device YAML says what has been tested on it", () => {
  const yamls = fs.readdirSync(path.join(REPO, "devices")).filter((f) => f.endsWith(".yaml"));
  const entries = tested();
  assert.deepEqual(entries.map((t) => t.file).sort(), yamls.sort(), "devices/*.yaml and site/src/_data/tested.js differ");
  const pages = fs.readdirSync(path.resolve(import.meta.dirname, "../src/devices")).map((f) => f.replace(/\.md$/, ""));
  for (const t of entries) {
    assert.ok(["works", "in-progress"].includes(t.status), `${t.file}: status ${t.status}`);
    assert.equal(t.status === "works", Boolean(t.checked), `${t.file}: a "works" entry needs \`checked\`, and only it`);
    if (t.device) assert.ok(pages.includes(t.device), `${t.file}: no device page ${t.device}`);
    assert.ok(t.platform && t.name, `${t.file}: no platform or name in the YAML`);
  }
});

test("the tested-devices page counts the checked YAMLs", () => {
  const html = fs.readFileSync(path.join(OUT, "docs/devices/index.html"), "utf8");
  const works = tested().filter((t) => t.status === "works").length;
  assert.match(html, new RegExp(`<strong>${works} of ${tested().length}</strong>`));
});

test("every link inside the site lands on a page and an id", () => {
  const bad = [];
  for (const file of htmlFiles()) {
    const html = fs.readFileSync(file, "utf8");
    const self = `${PREFIX}${path.relative(OUT, file).replace(/index\.html$/, "")}`;
    for (let [, href] of html.matchAll(/\s(?:href|src)="([^"]*)"/g)) {
      const where = `${path.relative(OUT, file)}: ${href}`;
      if (href.startsWith("#")) {
        if (href.length > 1 && !idsOf(file).has(decodeURIComponent(href.slice(1)))) bad.push(`${where} (no such id)`);
        continue;
      }
      if (/^[a-z][a-z0-9+.-]*:/i.test(href)) continue;
      // A relative link (the device pages' front matter has some): from this page's path.
      if (!href.startsWith("/")) href = new URL(href, `http://site${self}`).pathname + (href.includes("#") ? `#${href.split("#")[1]}` : "");
      if (!href.startsWith(PREFIX)) {
        bad.push(`${where} (no ${PREFIX} prefix)`);
        continue;
      }
      const [p, frag] = href.split("#");
      const target = servedFile(p);
      if (!target) bad.push(`${where} (no such page)`);
      else if (frag && target.endsWith(".html") && !idsOf(target).has(decodeURIComponent(frag))) bad.push(`${where} (no such id)`);
    }
  }
  assert.deepEqual(bad, []);
});

test("every link into the repo names a file and heading that are there", () => {
  const bad = [];
  for (const file of htmlFiles()) {
    const html = fs.readFileSync(file, "utf8");
    for (const [, href] of html.matchAll(/\shref="([^"]*)"/g)) {
      const base = [site.blob, site.tree].find((b) => href.startsWith(b));
      if (!base) continue;
      const [repoPath, frag] = decodeURI(href.slice(base.length)).split("?")[0].split("#");
      const full = path.join(REPO, repoPath);
      const where = `${path.relative(OUT, file)}: ${href}`;
      if (!fs.existsSync(full)) bad.push(`${where} (no such file)`);
      else if (frag && repoPath.endsWith(".md") && !headingIds(repoPath).has(frag)) bad.push(`${where} (no such heading)`);
    }
  }
  assert.deepEqual(bad, []);
});

test("slugify gives GitHub's heading ids", () => {
  assert.equal(slugify("LED looks (idle and active)"), "led-looks-idle-and-active");
  assert.equal(slugify("DMA channels (stm32f1)"), "dma-channels-stm32f1");
  assert.equal(slugify("9. A complete session, step by step"), "9-a-complete-session-step-by-step");
  assert.equal(slugify("Plain feedback (F1, every DAW)"), "plain-feedback-f1-every-daw");
});

test("docUrl and resolveLink", () => {
  assert.equal(docUrl("components/leds/README.md"), "/docs/components/leds/");
  assert.equal(docUrl("core/performance.md"), "/docs/core/performance/");
  const env = {
    file: "components/leds/README.md",
    pages: new Map([["components/modes/README.md", "/docs/components/modes/"]]),
    blob: "B/",
    tree: "T/",
    prefix: "/p/",
  };
  assert.equal(resolveLink("../modes/README.md#cycles", env), "/p/docs/components/modes/#cycles");
  assert.equal(resolveLink("../modes/", env), "/p/docs/components/modes/");
  assert.equal(resolveLink("leds.c", env), "B/components/leds/leds.c");
  assert.equal(resolveLink("../../core/", env), "T/core");
  assert.equal(resolveLink("#full-colour-leds", env), "#full-colour-leds");
  assert.equal(resolveLink("https://esphome.io/", env), "https://esphome.io/");
});

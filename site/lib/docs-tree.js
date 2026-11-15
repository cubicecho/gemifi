// The docs section (/docs/, the `docs` data in src/_data/docs.js): the repo's Markdown collected into one tree, as
// ESPHome's docs are. Nothing is copied: each build reads
//
// - core/README.md and core/performance.md (the device YAML and the costs),
// - every components/<name>/README.md, grouped by the base it is on (its
//   README's opening line says which) and summed up by its builder module's
//   docstring,
// - the Markdown each platform in platforms.js lists under `docs`.
//
// A new component folder with a README shows up on its own. The devices part
// of the tree comes from the devices collection (src/devices/*.md) and the
// tested-devices page (tested.js), in the layout.

import site from "../src/_data/site.js";
import platforms from "../src/_data/platforms.js";
import {
  componentFiles,
  componentGroup,
  componentSummary,
  docUrl,
  markdown,
  readRepo,
  renderDoc,
  resolveLink,
} from "./repo-docs.js";

/** The component groups in tree order. A group's base component (same key) is listed under BASE instead. */
export const GROUPS = [
  { key: "controls", title: "Controls", intro: "What the player plays. Each is built when the device has it." },
  { key: "i2c", title: "Buses", intro: "The buses other components sit on." },
  {
    key: "hub",
    title: "Hubs",
    intro: "Chips and circuits that give pins: shift registers, multiplexers, I/O expanders, matrices.",
  },
  { key: "led_strip", title: "LED strips", intro: "Chains of LEDs that show any colour." },
  { key: "led_driver", title: "LED drivers", intro: "Chips that light LEDs and digits from a RAM of bits." },
  { key: "display", title: "Displays", intro: "Screens, each on a platform, and the text shown on them." },
  { key: "other", title: "Other", intro: "" },
];

/** The base components, out of their groups: what each group's components are built on. */
export const BASE = {
  key: "base",
  title: "Base components",
  intro: "The base of each group: the YAML list its components go in, and what they share.",
};

const CORE = [
  { file: "core/README.md", url: "/docs/core/", title: "Device YAML and the core", label: "Device YAML" },
  { file: "core/performance.md", title: "Component performance", label: "Performance" },
];

let cache;

/** Forget the tree, so the next build reads the READMEs again (eleventy.config.js, before each build). */
export function resetDocs() {
  cache = undefined;
}

/** The whole docs tree and every page in it, built once per build. */
export function buildDocs() {
  if (cache) return cache;

  // Every page first, so links between docs can go to their pages.
  const specs = [];
  for (const c of CORE) specs.push({ ...c, url: c.url ?? docUrl(c.file), section: "core" });
  const components = componentFiles().map((file) => {
    const name = file.split("/")[1];
    const spec = { file, url: `/docs/components/${name}/`, title: name, label: name, section: "components", name };
    specs.push(spec);
    return spec;
  });
  const platformItems = platforms.map((p) => {
    const docs = p.docs.filter(([f]) => f.endsWith(".md"));
    const notes = docs.find(([f]) => f.startsWith("platform/"));
    const page = {
      file: notes?.[0],
      url: `/docs/platforms/${p.id}/`,
      title: p.name,
      label: p.name,
      section: "platforms",
      platform: p,
    };
    specs.push(page);
    const children = docs
      .filter((d) => d !== notes)
      .map(([file, label]) => {
        const existing = specs.find((s) => s.file === file);
        if (existing) return { label, url: existing.url };
        const spec = { file, url: docUrl(file), label, section: "platforms" };
        specs.push(spec);
        return { label, url: spec.url };
      });
    return { title: p.name, url: page.url, status: p.status, keys: p.keys, children };
  });

  const pages = new Map(specs.filter((s) => s.file).map((s) => [s.file, s.url]));
  const env = { pages, blob: site.blob, tree: site.tree, prefix: site.pathPrefix };
  const md = markdown();

  const rendered = specs.map((s) => {
    const doc = s.file ? renderDoc(md, s.file, env, { url: s.url, title: s.title }) : { html: "", toc: [], url: s.url };
    return {
      ...s,
      ...doc,
      title: s.title ?? doc.title,
      source: s.file ? `${site.blob}${s.file}` : null,
    };
  });

  // Components into their groups, with a line each for the docs home. A group's base component goes in the
  // base group instead, so a group lists only what is built on it (and one that is only its base drops out).
  const groups = GROUPS.map((g) => ({ ...g, items: [] }));
  const base = { ...BASE, items: [] };
  for (const c of components) {
    const page = rendered.find((r) => r.url === c.url);
    const key = componentGroup(c.name, readRepo(c.file));
    const group = groups.find((g) => g.key === key) ?? groups.find((g) => g.key === "other");
    page.summary = componentSummary(c.name);
    page.base = c.name === group.key;
    const home = page.base ? base : group;
    page.group = home.title;
    page.tone = home.key;
    home.items.push({ title: c.name, url: c.url, summary: page.summary, of: page.base ? group : undefined });
  }
  const shown = groups.filter((g) => g.items.length);
  for (const g of [base, ...shown]) g.items.sort((a, b) => a.title.localeCompare(b.title));
  // A base names its group, linked on the docs home when the group is shown.
  for (const i of base.items) i.of = { key: i.of.key, title: i.of.title, shown: shown.includes(i.of) };

  cache = {
    core: CORE.map((c) => ({ title: c.label, url: specs.find((s) => s.file === c.file).url })),
    base,
    groups: shown,
    platforms: platformItems,
    pages: rendered,
    /** A repo path (with an optional #anchor) as a site link: its doc page if it has one, else GitHub. */
    link: (href) => resolveLink(href, { ...env, file: "README.md" }),
  };
  return cache;
}


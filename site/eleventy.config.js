// Eleventy config for the gemifi site.
//
// Deployed to https://cubicecho.github.io/gemifi/, so everything is served
// under the /gemifi/ path prefix. Use the `| url` filter on every internal
// link and asset — a bare "/css/cubesite.css" 404s on Pages and works in dev,
// which is the worst way for it to break.

import path from "node:path";
import * as pagefind from "pagefind";

import site from "./src/_data/site.js";
import { buildDocs, resetDocs } from "./lib/docs-tree.js";
import { highlightBlocks } from "./lib/highlight.js";
import { addPager } from "./lib/pager.js";

export default function (eleventyConfig) {
  eleventyConfig.on("eleventy.before", resetDocs);
  // The site's own .md pages colour their code blocks as the repo docs do (lib/highlight.js).
  eleventyConfig.amendLibrary("md", highlightBlocks);

  // The docs search (the box over the docs menu): Pagefind indexes the built pages after every build, dev
  // server included, into _site/pagefind/. Only what's inside data-pagefind-body (the docs layout's page)
  // is indexed.
  eleventyConfig.on("eleventy.after", async ({ dir }) => {
    const { index } = await pagefind.createIndex();
    const added = await index.addDirectory({ path: dir.output });
    const written = await index.writeFiles({ outputPath: path.join(dir.output, "pagefind") });
    await pagefind.close();
    const errors = [...added.errors, ...written.errors];
    if (errors.length) throw new Error(`pagefind: ${errors.join("; ")}`);
  });
  // A repo path (with an optional #anchor) as a link: its /docs/ page if it has one, else GitHub.
  // `{{ "core/README.md#presets" | repoLink }}`, already with the path prefix.
  eleventyConfig.addFilter("repoLink", (repoPath) => buildDocs().link(repoPath));

  // The docs (src/_data/docs.js) are the repo's own READMEs: rebuild when they change.
  for (const dir of ["../components/", "../core/", "../platform/", "../bootloader/", "../firmware/", "../tools/", "../devices/", "../protocol/", "../hosts/", "../web/updater/README.md"]) {
    eleventyConfig.addWatchTarget(dir);
  }

  eleventyConfig.addPassthroughCopy({ "src/css": "css" });
  eleventyConfig.addPassthroughCopy({ "src/assets": "assets" });
  // The device YAML's JSON schema (tools/builder/gemifi_build.py schema), for editors:
  // `# yaml-language-server: $schema=https://cubicecho.github.io/gemifi/schema/device.json`.
  eleventyConfig.addPassthroughCopy({ "src/schema": "schema" });
  // Pages runs Jekyll over the artifact unless told not to; a leading-underscore
  // directory (like _includes output, if any) would vanish without this.
  eleventyConfig.addPassthroughCopy({ "src/nojekyll": ".nojekyll" });

  eleventyConfig.addShortcode("year", () => `${new Date().getFullYear()}`);
  // Previous/next at the foot of each docs page, from the docs menu's own order (lib/pager.js).
  eleventyConfig.addTransform("pager", addPager);

  // Whether a docs menu section holds the page at `url` (its items, or their children): the section is open then.
  eleventyConfig.addFilter("holds", (items, url) =>
    items.some((i) => i.url === url || (i.children ?? []).some((c) => c.url === url)),
  );

  // The devices whose status is `key` (statuses.js): the homepage's counts.
  eleventyConfig.addFilter("withStatus", (devices, key) => devices.filter((d) => d.data.status === key));

  // One markdown file per device in src/devices/, ordered by `order:`.
  eleventyConfig.addCollection("devices", (api) =>
    api
      .getFilteredByGlob("src/devices/*.md")
      .sort((a, b) => (a.data.order ?? 99) - (b.data.order ?? 99)),
  );

  eleventyConfig.setServerOptions({ port: 3000 });

  return {
    dir: {
      input: "src",
      output: "_site",
      includes: "_includes",
      data: "_data",
    },
    pathPrefix: site.pathPrefix,
    htmlTemplateEngine: "njk",
    markdownTemplateEngine: "njk",
    templateFormats: ["njk", "md"],
  };
}

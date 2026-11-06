// Eleventy config for the gemifi site.
//
// Deployed to https://cubicecho.github.io/gemifi/, so everything is served
// under the /gemifi/ path prefix. Use the `| url` filter on every internal
// link and asset — a bare "/css/cubesite.css" 404s on Pages and works in dev,
// which is the worst way for it to break.

export default function (eleventyConfig) {
  eleventyConfig.addPassthroughCopy({ "src/css": "css" });
  eleventyConfig.addPassthroughCopy({ "src/assets": "assets" });
  // The device YAML's JSON schema (tools/builder/gemifi_build.py schema), for editors:
  // `# yaml-language-server: $schema=https://cubicecho.github.io/gemifi/schema/device.json`.
  eleventyConfig.addPassthroughCopy({ "src/schema": "schema" });
  // Pages runs Jekyll over the artifact unless told not to; a leading-underscore
  // directory (like _includes output, if any) would vanish without this.
  eleventyConfig.addPassthroughCopy({ "src/nojekyll": ".nojekyll" });

  eleventyConfig.addShortcode("year", () => `${new Date().getFullYear()}`);

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
    pathPrefix: "/gemifi/",
    htmlTemplateEngine: "njk",
    markdownTemplateEngine: "njk",
    templateFormats: ["njk", "md"],
  };
}

// Every device page (src/devices/*.md): the device layout, inside the docs (its maker above the title, its
// summary under it, in the Devices tone).
export default {
  layout: "device.njk",
  permalink: "/devices/{{ page.fileSlug }}/",
  tone: "devices",
  eleventyComputed: {
    crumb: (data) => data.maker,
    lede: (data) => data.summary,
  },
};

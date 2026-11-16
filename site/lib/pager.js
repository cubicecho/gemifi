// Previous and next links at the foot of a docs page, in the docs menu's order. The menu (docs-nav.njk) is
// the one list of pages: this reads its links from the built page, so the two can't disagree.

const MENU = /<nav class="docs-nav"[^>]*>([\s\S]*?)<\/nav>/;
const LINK = /<a href="([^"]+)"( aria-current="page")?>([^<]*)<\/a>/g;
export const MARKER = "<!-- pager -->";

/** The menu's links in order, each { href, title, current }; a page the menu names twice counts once. */
export function menuLinks(html) {
  const menu = html.match(MENU);
  if (!menu) return [];
  const seen = new Set();
  const links = [];
  for (const [, href, current, title] of menu[1].matchAll(LINK)) {
    if (seen.has(href)) continue;
    seen.add(href);
    links.push({ href, title: title.trim(), current: Boolean(current) });
  }
  return links;
}

/** The pager's HTML for this page: its menu neighbours, or "" when the menu doesn't hold it. */
export function pagerHtml(html) {
  const links = menuLinks(html);
  const at = links.findIndex((l) => l.current);
  if (at < 0) return "";
  const side = (l, rel, label) =>
    l ? `<a class="pager-${rel}" href="${l.href}" rel="${rel}"><span>${label}</span>${l.title}</a>` : "<span></span>";
  return `<nav class="pager" aria-label="Previous and next pages" data-pagefind-ignore>${side(links[at - 1], "prev", "Previous")}${side(links[at + 1], "next", "Next")}</nav>`;
}

/** An Eleventy transform: a page with the marker gets its pager there. */
export function addPager(content) {
  if (!this.page.outputPath?.endsWith(".html") || !content.includes(MARKER)) return content;
  return content.replace(MARKER, pagerHtml(content));
}

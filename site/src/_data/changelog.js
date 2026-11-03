// The repo's CHANGELOG.md (written by semantic-release, see .releaserc.json) for the
// /changelog/ page. Its title and intro are dropped (the page has its own), and every
// version heading becomes an h2: semantic-release writes `#` for a minor or major
// release and `##` for a patch, which would put patches under the release before them.
import { readFileSync } from "node:fs";

export default function () {
  let text;
  try {
    text = readFileSync(new URL("../../../CHANGELOG.md", import.meta.url), "utf8");
  } catch {
    return { body: "", latest: null };
  }
  const lines = text.split("\n");
  const first = lines.findIndex((l) => /^#{1,2} \[?\d+\.\d+\.\d+/.test(l));
  if (first < 0) {
    return { body: "", latest: null };
  }
  const body = lines
    .slice(first)
    .map((l) => l.replace(/^#{1,2} (\[?\d+\.\d+\.\d+)/, "## $1"))
    .join("\n");
  const latest = body.match(/^## \[?(\d+\.\d+\.\d+[^\]\s]*)/)?.[1] ?? null;
  return { body, latest };
}

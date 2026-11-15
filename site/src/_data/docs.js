// The docs tree (/docs/): built by lib/docs-tree.js from the repo's READMEs.
import { buildDocs } from "../../lib/docs-tree.js";

export default function () {
  return buildDocs();
}

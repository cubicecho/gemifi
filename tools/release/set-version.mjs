#!/usr/bin/env node
// Writes the release version everywhere gemifi shows it. semantic-release runs this
// (.releaserc.json, @semantic-release/exec) after it has bumped the root package.json,
// and commits the files it touches.
//
//   node tools/release/set-version.mjs 1.2.3
//
// - tools/builder/gemifi_device.py   VERSION: the builder's, which min_version: is checked against
//                                    and /api/info answers with (the builder UI shows it)
// - web/updater/package.json     and its lock file's root entries
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const version = process.argv[2];
if (!/^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/.test(version ?? "")) {
  console.error("usage: set-version.mjs <x.y.z>");
  process.exit(2);
}
const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

function edit(rel, change) {
  const path = join(root, rel);
  const before = readFileSync(path, "utf8");
  const after = change(before);
  if (after === before && !before.includes(version)) {
    throw new Error(`${rel}: no version found to set`);
  }
  writeFileSync(path, after);
  console.log(`${rel}: ${version}`);
}

edit("tools/builder/gemifi_device.py", (s) => s.replace(/^VERSION = "[^"]*"/m, `VERSION = "${version}"`));

const json = (s, set) => {
  const o = JSON.parse(s);
  set(o);
  return `${JSON.stringify(o, null, 2)}\n`;
};
edit("web/updater/package.json", (s) => json(s, (o) => { o.version = version; }));
edit("web/updater/package-lock.json", (s) =>
  json(s, (o) => {
    o.version = version;
    if (o.packages?.[""]) o.packages[""].version = version;
  }),
);

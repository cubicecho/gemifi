"""Device YAML reuse, ESPHome style (core/README.md "Packages and substitutions").

It runs on the loaded tree, before validation, so none of it reaches the firmware but the hash:

- `!include file.yaml`, or `!include { file: file.yaml, vars: { name: value } }`: that file's
  content in place; `vars` are substitutions for that file only.
- `packages:` (a map or a list of local files, `preset:<name>`, `!include`s or inline maps): merged
  under the device file. Maps merge key by key; lists of components merge by id; the device file wins.
- `- id: !extend <id>` / `- id: !remove <id>` in a component list: change or drop one component a
  package brought in. `key: !remove` drops a whole key a package set.
- `substitutions:` and `${name}` in string values; the CLI's `-s NAME VALUE` overrides them.
- `min_version:` in any of the files: the builder must be at least that version.
- config_hash(): the resolved config's hash, which the device reports as param config.hash.

    res = resolve(data, path)       # data: load_yaml()'s tree; res.data, res.files, res.diags

Lines keep their file (gemifi_device.Line), so a problem inside a package or an included file is
reported at that file's line.
"""
import difflib
import hashlib
import json
import os
import re

import gemifi_device as gd
from gemifi_device import Diag, LDict, LList, Tagged, YamlLoadError

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PRESET_DIR = os.path.join(REPO, "devices")
PRESET_PREFIX = "preset:"
PRESET_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
SUB_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")
SUB_NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
VERSION_RE = re.compile(r"(\d+)\.(\d+)\.(\d+)")
MAX_DEPTH = 16          # nested packages / includes
INCLUDE_KEYS = ["file", "vars"]


class Resolved:
    """resolve()'s result: data (the merged tree, None after an error that stops it), files (the
    other files read, in order) and diags."""

    def __init__(self, data, files, diags):
        self.data, self.files, self.diags = data, files, diags


def version_tuple(text):
    m = VERSION_RE.match(str(text).strip())
    return tuple(int(x) for x in m.groups()) if m else None


def scalar_value(text):
    """A -s VALUE as YAML would read it in substitutions: (`64` an int, `yes` a bool, `abc` text)."""
    tag = gd.yaml.resolver.Resolver().resolve(gd.yaml.ScalarNode, text, (True, False))
    try:
        return gd.yaml.SafeLoader("").construct_object(gd.yaml.ScalarNode(tag, text))
    except gd.yaml.YAMLError:
        return text


def _copy_dict(d, skip=()):
    out = LDict()
    out.line = d.line
    for k in d:
        if k in skip:
            continue
        out[k] = d[k]
        out.key_lines[k] = d.line_of(k)
        if k in d.raw:
            out.raw[k] = d.raw[k]
    return out


def _known(names):
    return ", ".join(sorted(names)) or "none"


class _R:
    def __init__(self, path, preset_dir):
        self.main = os.path.abspath(path)
        self.preset_dir = preset_dir or PRESET_DIR
        self.diags, self.files = [], []
        self.stack = [self.main]

    def err(self, line, msg, path=""):
        self.diags.append(Diag("error", path, line, msg))

    # -- files --

    def min_version(self, d):
        """Pop and check a file's min_version:. False when this builder is too old for the file."""
        if not isinstance(d, dict) or "min_version" not in d:
            return True
        raw, line = d.raw_of("min_version"), d.line_of("min_version")
        del d["min_version"]
        need = version_tuple(raw) if VERSION_RE.fullmatch(str(raw).strip()) else None
        if need is None:
            hint = " (quote it: min_version: \"0.34.0\")" if not isinstance(raw, str) else ""
            self.err(line, f"must be a version like 0.34.0 (got {raw!r}){hint}", "min_version")
            return True
        have = version_tuple(gd.VERSION) or (0, 0, 0)
        if have < need:
            self.err(line, f"needs gemifi builder {'.'.join(map(str, need))} or newer; this one is "
                           f"{gd.VERSION}: update gemifi (git pull, or the newer Docker image)", "min_version")
            return False
        return True

    def presets(self):
        try:
            return sorted(f[:-5] for f in os.listdir(self.preset_dir) if f.endswith(".yaml"))
        except OSError:
            return []

    def ref_path(self, ref, line, base_file, what):
        """A package or !include file name -> an absolute path, or None after an error.
        Relative names count from the file that names them; preset:<name> is devices/<name>.yaml."""
        if not isinstance(ref, str) or not ref.strip():
            self.err(line, f"{what} needs a file name (got {ref!r})")
            return None
        ref = ref.strip()
        if "://" in ref:
            self.err(line, f"{what} '{ref}': remote packages aren't supported yet; use a local file "
                           f"or {PRESET_PREFIX}<name>")
            return None
        if ref.startswith(PRESET_PREFIX):
            name = re.sub(r"\.ya?ml$", "", ref[len(PRESET_PREFIX):].strip())
            p = os.path.join(self.preset_dir, name + ".yaml")
            if not PRESET_NAME_RE.fullmatch(name) or not os.path.isfile(p):
                known = self.presets()
                close = difflib.get_close_matches(name, known, 1)
                hint = f"; did you mean '{close[0]}'?" if close else ""
                self.err(line, f"{what}: no preset '{name}' (presets: {_known(known)}{hint})")
                return None
            return os.path.abspath(p)
        return os.path.normpath(os.path.join(os.path.dirname(base_file), os.path.expanduser(ref)))

    def load_file(self, ref, line, base_file, what):
        """Read, parse and expand one file. Returns its tree, or None after an error."""
        p = self.ref_path(ref, line, base_file, what)
        if p is None:
            return None
        if p in self.stack:
            chain = " -> ".join(os.path.basename(f) for f in self.stack[self.stack.index(p):] + [p])
            self.err(line, f"{what} '{ref}' includes itself ({chain})")
            return None
        if len(self.stack) > MAX_DEPTH:
            self.err(line, f"{what} '{ref}': packages and includes nest more than {MAX_DEPTH} deep")
            return None
        try:
            with open(p, encoding="utf-8") as f:
                text = f.read()
        except OSError as e:
            self.err(line, f"{what} '{ref}': can't read {p}: {e.strerror}")
            return None
        except UnicodeDecodeError:
            self.err(line, f"{what} '{ref}': {p} is not UTF-8 text")
            return None
        if p not in self.files:
            self.files.append(p)
        try:
            data = gd.load_yaml(text, file=p)
        except YamlLoadError as e:
            self.diags.append(Diag("error", "", e.line, e.msg, src=p))
            return None
        self.stack.append(p)
        try:
            if not self.min_version(data):
                return None
            return self.expand(data, p)
        finally:
            self.stack.pop()

    # -- !include --

    def expand(self, x, file):
        """x with its !include values replaced by the files' content."""
        if isinstance(x, Tagged) and x.tag == "include":
            return self.include(x, file)
        if isinstance(x, dict):
            for k in list(x):
                if isinstance(x[k], Tagged) and x[k].tag == "include":
                    x.raw.pop(k, None)
                x[k] = self.expand(x[k], file)
        elif isinstance(x, list):
            for i in range(len(x)):
                if isinstance(x[i], Tagged) and x[i].tag == "include" and i < len(x.item_raw):
                    x.item_raw[i] = None
                x[i] = self.expand(x[i], file)
        return x

    def include(self, t, file):
        spec, vars_ = t.value, None
        if isinstance(spec, dict):
            for k in spec:
                if k not in INCLUDE_KEYS:
                    self.err(spec.line_of(k), f"!include: unknown key '{k}' (known: {', '.join(INCLUDE_KEYS)})")
            if "vars" in spec:
                vars_ = self.sub_table(spec["vars"], "vars", spec.line_of("vars"))
            spec = spec.get("file")
        data = self.load_file(spec, t.line, file, "!include")
        if data is not None and vars_:
            data = self.substitute(data, vars_, keep_unknown=True)
        return data

    # -- packages --

    def packages(self, data, file):
        """data with its packages: merged under it."""
        if not isinstance(data, dict) or "packages" not in data:
            return data
        pk, pline = data["packages"], data.line_of("packages")
        del data["packages"]
        if isinstance(pk, dict):
            items = [(f"packages.{k}", pk[k], pk.line_of(k)) for k in pk]
        elif isinstance(pk, list):
            items = [(f"packages[{i}]", pk[i], pk.line_of(i)) for i in range(len(pk))]
        else:
            self.err(pline, "must be a mapping of name: package (a file, preset:<name>, !include or a "
                            "mapping) or a list of them", "packages")
            return data
        base = None
        for p, val, line in items:
            if isinstance(val, str):
                val = self.load_file(val, line, file, "package")
                if val is None:
                    continue
            if not isinstance(val, dict):
                if val is not None or not self.diags:     # None: an !include that failed, already said
                    self.err(line, f"a package must be a mapping, a part of a device file (got "
                                   f"{gd.type_name(val)})", p)
                continue
            src = getattr(val.line, "file", None) or file     # relative names in it count from there
            self.stack.append(src)
            try:
                val = self.packages(val, src)
            finally:
                self.stack.pop()
            base = val if base is None else self.merge(base, val, "")
        return data if base is None else self.merge(base, data, "")

    def merge(self, base, over, path):
        """over merged onto base: maps key by key, component lists by id; over wins."""
        if isinstance(base, dict) and isinstance(over, dict):
            out = _copy_dict(base)
            out.line = over.line
            for k in over:
                v, line, p = over[k], over.line_of(k), f"{path}.{k}" if path else str(k)
                if isinstance(v, Tagged) and v.tag == "remove":
                    if v.value:
                        self.err(line, f"`{k}: !remove` drops the whole key: it takes no id (for one "
                                       f"component of a list, use `- id: !remove {v.value}`)", p)
                    elif k in out:
                        del out[k]
                    else:
                        self.err(line, f"nothing to remove: no package sets {p}", p)
                    continue
                out[k] = self.merge(out[k], v, p) if k in out else v
                out.key_lines[k] = line
                if k in over.raw:
                    out.raw[k] = over.raw[k]
                else:
                    out.raw.pop(k, None)
            return out
        if isinstance(base, list) and isinstance(over, list):
            return self.merge_list(base, over, path)
        return over

    @staticmethod
    def item_key(x):
        """What a list item is matched by: its id (or the id its name gives), an !extend/!remove."""
        if not isinstance(x, dict):
            return None
        i = x.get("id")
        if isinstance(i, (str, Tagged)):
            return i
        n = x.get("name")
        return gd.derive_id(n) if "id" not in x and isinstance(n, str) else None

    def merge_list(self, base, over, path):
        """By id when every item has one (or over has !extend / !remove); else over replaces base."""
        bk = [self.item_key(x) for x in base]
        ok = [self.item_key(x) for x in over]
        if not (all(k is not None for k in bk + ok) or any(isinstance(k, Tagged) for k in ok)):
            return over
        items = [[base[i], base.line_of(i), base.item_raw[i] if i < len(base.item_raw) else None]
                 for i in range(len(base))]
        index = {k: i for i, k in enumerate(bk) if k is not None}
        for j, x in enumerate(over):
            k, line = ok[j], over.line_of(j)
            if isinstance(k, Tagged):
                if k.tag not in ("extend", "remove"):
                    self.err(k.line, f"!{k.tag} can't be an id", f"{path}[{j}].id")
                    continue
                target = k.value
                i = index.get(target)
                if i is None or items[i] is None:
                    known = [t for t, n in index.items() if items[n] is not None and isinstance(t, str)]
                    close = difflib.get_close_matches(str(target), known, 1)
                    hint = f"; did you mean '{close[0]}'?" if close else ""
                    self.err(k.line, f"!{k.tag} {target}: no item with id '{target}' in {path} from a "
                                     f"package (ids: {_known(known)}{hint})", f"{path}[{j}].id")
                    continue
                if k.tag == "remove":
                    if len(x) > 1:
                        self.err(k.line, "!remove drops the whole item: leave out its other keys",
                                 f"{path}[{j}]")
                    items[i] = None
                else:
                    items[i][0] = self.merge(items[i][0], _copy_dict(x, skip=("id",)), f"{path}[{i}]")
                continue
            if k is not None and k in index and items[index[k]] is not None:
                items[index[k]] = [x, line, None]      # the same id: this file's item replaces it
            else:
                if k is not None:
                    index[k] = len(items)
                items.append([x, line, None])
        out = LList()
        out.line = over.line
        for it in items:
            if it is not None:
                out.append(it[0])
                out.item_lines.append(it[1])
                out.item_raw.append(it[2])
        return out

    # -- substitutions --

    def sub_table(self, m, path, line):
        """A substitutions: or vars: mapping -> {name: (value, text)}."""
        if m is None:
            return {}
        if not isinstance(m, dict):
            self.err(line, f"must be a mapping of name: value (got {gd.type_name(m)})", path)
            return {}
        out = {}
        for k in m:
            p, kl, v = f"{path}.{k}", m.line_of(k), m[k]
            if not isinstance(k, str) or not SUB_NAME_RE.fullmatch(k):
                self.err(kl, f"'{k}' can't be a substitution name (letters, digits and _; not a digit "
                             "first)", p)
            elif isinstance(v, (dict, list, Tagged)):
                self.err(kl, f"must be a plain value: text or a number (got {gd.type_name(v)})", p)
            else:
                out[k] = (v, "" if v is None else m.raw_of(k), kl)
        return out

    def sub_str(self, s, table, line, keep_unknown, path):
        """${name} in s -> (value, text). A string that is just ${name} takes the value's type."""
        m = SUB_RE.fullmatch(s)
        if m and m.group(1) in table:
            v, text, _l = table[m.group(1)]
            return v, text

        def rep(m):
            name = m.group(1)
            if name in table:
                return table[name][1]
            if not keep_unknown:
                close = difflib.get_close_matches(name, list(table), 1)
                hint = f"; did you mean '{close[0]}'?" if close else ""
                self.err(line, f"unknown substitution ${{{name}}} (substitutions: {_known(table)}{hint})", path)
            return m.group(0)

        out = SUB_RE.sub(rep, s)
        return out, out

    def substitute(self, x, table, keep_unknown=False, path=""):
        """${name} replaced throughout x's string values (and !extend / !remove ids)."""
        if isinstance(x, Tagged):
            if isinstance(x.value, str) and "${" in x.value:
                x.value = str(self.sub_str(x.value, table, x.line, keep_unknown, path)[0])
            elif isinstance(x.value, (dict, list)):
                self.substitute(x.value, table, keep_unknown, path)
        elif isinstance(x, dict):
            for k in x:
                v, p = x[k], f"{path}.{k}" if path else str(k)
                if isinstance(v, str) and "${" in v:
                    x[k], x.raw[k] = self.sub_str(v, table, x.line_of(k), keep_unknown, p)
                else:
                    self.substitute(v, table, keep_unknown, p)
        elif isinstance(x, list):
            for i, v in enumerate(x):
                p = f"{path}[{i}]"
                if isinstance(v, str) and "${" in v:
                    x[i], text = self.sub_str(v, table, x.line_of(i), keep_unknown, p)
                    while len(x.item_raw) <= i:
                        x.item_raw.append(None)
                    x.item_raw[i] = text
                else:
                    self.substitute(v, table, keep_unknown, p)
        return x

    def resolve_table(self, table):
        """Substitutions that use other substitutions (`b: ${a}-x`) get those values first."""
        for _ in range(len(table) + 1):
            changed = False
            for k, (v, text, line) in list(table.items()):
                if isinstance(v, str) and SUB_RE.search(v):
                    others = {n: e for n, e in table.items() if n != k}
                    nv, nt = self.sub_str(v, others, line, True, f"substitutions.{k}")
                    if (nv, nt) != (v, text):
                        table[k] = (nv, nt, line)
                        changed = True
            if not changed:
                break
        for k, (v, _t, line) in table.items():
            used = SUB_RE.findall(v) if isinstance(v, str) else []
            if k in used or any(u in table for u in used):
                self.err(line, f"${{{k}}} and the substitutions it uses refer to each other in a loop",
                         f"substitutions.{k}")
            for u in used:
                if u not in table:
                    self.err(line, f"unknown substitution ${{{u}}} (substitutions: {_known(table)})",
                             f"substitutions.{k}")
        return table

    def leftovers(self, x, path=""):
        """Tags nothing used up: !extend / !remove outside a package merge."""
        if isinstance(x, Tagged):
            self.err(x.line, f"!{x.tag} {x.value or ''}: there is nothing from a package to {x.tag} here. "
                             f"!extend and !remove go on a list item's id (`- id: !{x.tag} some_id`) and "
                             "change what a package brought in", path)
        elif isinstance(x, dict):
            for k in x:
                self.leftovers(x[k], f"{path}.{k}" if path else str(k))
        elif isinstance(x, list):
            for i, v in enumerate(x):
                self.leftovers(v, f"{path}[{i}]")


def resolve(data, path, subs=None, preset_dir=None):
    """Packages, !include, !extend / !remove, substitutions and min_version on a loaded device file.
    subs: {name: text} from the command line; they win over substitutions:. -> Resolved."""
    r = _R(path, preset_dir)
    if not isinstance(data, dict):
        return Resolved(data, r.files, r.diags)
    if not r.min_version(data):
        return Resolved(None, r.files, r.diags)
    data = r.expand(data, r.main)
    data = r.packages(data, r.main)
    if r.diags:
        return Resolved(None, r.files, r.diags)
    table = {}
    if "substitutions" in data:
        table = r.sub_table(data["substitutions"], "substitutions", data.line_of("substitutions"))
        del data["substitutions"]
    for k, text in (subs or {}).items():
        if not SUB_NAME_RE.fullmatch(k):
            r.err(0, f"-s {k}: '{k}' can't be a substitution name (letters, digits and _)")
        else:
            table[k] = (scalar_value(text), text, table[k][2] if k in table else 0)
    r.resolve_table(table)
    r.substitute(data, table)
    r.leftovers(data)
    return Resolved(None if r.diags else data, r.files, r.diags)


def resolve_file(path, subs=None, preset_dir=None):
    """resolve() for a file on disk. A file that can't be read or parsed gives data None."""
    try:
        with open(path, encoding="utf-8") as f:
            data = gd.load_yaml(f.read())
    except (OSError, UnicodeDecodeError) as e:
        return Resolved(None, [], [Diag("error", "", 0, f"can't read: {getattr(e, 'strerror', e)}")])
    except YamlLoadError as e:
        return Resolved(None, [], [Diag("error", "", e.line, e.msg)])
    return resolve(data, path, subs, preset_dir)


# ---------------------------------------------------------------------------------------------
# config hash

def canonical(data):
    """The resolved config as one line of JSON: maps in file order, lists in order, and every scalar
    as "<type>:<text as written>" (`int:0x40`, `str:Sustain`, `bool:true`, `NoneType:`), so two files
    hash the same exactly when they read the same. Comments, layout, substitutions: and packages:
    (already applied) don't count."""
    def scalar(v, text):
        return f"{type(v).__name__}:{v if isinstance(v, str) else text}"

    def plain(x):
        if isinstance(x, dict):
            return {str(k): plain(v) if isinstance(v, (dict, list)) else scalar(v, x.raw_of(k)) for k, v in x.items()}
        if isinstance(x, list):
            raw = x.raw_of if isinstance(x, LList) else (lambda i: str(x[i]))
            return [plain(v) if isinstance(v, (dict, list)) else scalar(v, raw(i)) for i, v in enumerate(x)]
        return scalar(x, str(x))

    return json.dumps(plain(data), ensure_ascii=False, separators=(",", ":"))


def config_hash(data):
    """The first 31 bits of the canonical config's SHA-256 (a positive s32: param config.hash)."""
    digest = hashlib.sha256(canonical(data).encode("utf-8")).digest()
    return int.from_bytes(digest[:4], "big") & 0x7FFFFFFF

"""Device YAML reuse (gemifi_yaml.py): packages, !include, !extend / !remove, substitutions,
min_version, the config hash, and the JSON schema (gemifi_schema.py)."""
import glob
import io
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

TESTS = os.path.dirname(os.path.abspath(__file__))
BUILDER = os.path.dirname(TESTS)
REPO = os.path.dirname(os.path.dirname(BUILDER))
sys.path.insert(0, BUILDER)
import gemifi_build as gb  # noqa: E402
import gemifi_device as gd  # noqa: E402
import gemifi_schema as gs  # noqa: E402
import gemifi_yaml as gy  # noqa: E402

BASE = """\
gemifi: 2
name: base
board_id: 0x3F01
platform: nuvoton-nuc1xx
chip: NUC120LC1BN
usb: { vid: 0x1209, pid: 0x0002 }
substitutions:
  cc_a: 1
  label: Base
buttons:
  - { id: a, pin: PA1, action: cc_momentary, cc: "${cc_a}" }
  - { id: b, pin: PA2, action: cc_momentary, cc: 2 }
  - { name: Shift, pin: PA3, action: cc_momentary, cc: 3 }
"""

LEDS = """\
leds:
  - id: led_${n}
    pin: PB${n}
"""


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestPackages(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.write("base.yaml", BASE)
        self.write("leds.yaml", LEDS)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write(self, name, text):
        p = os.path.join(self.tmp, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
        return p

    def load(self, text, name="main.yaml", **kw):
        return gd.load_device(self.write(name, text), **kw)

    def ok(self, text, **kw):
        dev, diags = self.load(text, **kw)
        self.assertIsNotNone(dev, "\n".join(map(str, diags)))
        return dev

    def errs(self, text, **kw):
        dev, diags = self.load(text, **kw)
        self.assertIsNone(dev)
        return gd.errors(diags)

    def buttons(self, dev):
        return [(b.id, b.action_name, b.number) for b in dev.buttons]

    def test_package_merge(self):
        dev = self.ok("packages: { base: base.yaml }\nname: mine\nfriendly_name: ${label} plus\n")
        self.assertEqual((dev.node_name, dev.name), ("mine", "Base plus"))     # main wins; package subs
        self.assertEqual(self.buttons(dev), [("a", "cc_momentary", 1), ("b", "cc_momentary", 2),
                                             ("shift", "cc_momentary", 3)])
        self.assertEqual(dev.sources, [os.path.join(self.tmp, "base.yaml")])

    def test_list_merge_by_id(self):
        dev = self.ok("""\
packages: [base.yaml, !include { file: leds.yaml, vars: { n: 4 } }]
substitutions: { cc_a: 11 }
buttons:
  - id: !extend a
    led: led_4
  - id: !remove b
  - id: shift
    pin: PA5
    action: note
    note: 61
  - { id: c, pin: PA6, action: cc_momentary, cc: "${cc_a}" }
""")
        # !extend merges into a (and its ${cc_a} takes the main file's value); !remove drops b; the same
        # id (derived from the package's name: Shift) replaces the item in place; c is added
        self.assertEqual(self.buttons(dev), [("a", "cc_momentary", 11), ("shift", "note", 61),
                                             ("c", "cc_momentary", 11)])
        self.assertEqual(dev.buttons[0].led, 0)
        self.assertEqual([x.id for x in dev.leds], ["led_4"])

    def test_list_without_ids_is_replaced(self):
        self.write("m.yaml", BASE + "boot_combo: [a, b]\n")
        dev = self.ok("packages: { base: m.yaml }\nboot_combo: [shift]\n")
        self.assertEqual(dev.boot_combo, [2])

    def test_remove_key(self):
        self.write("m.yaml", BASE + "scan_interval: 250us\n")
        self.assertEqual(self.ok("packages: { base: m.yaml }\nscan_interval: !remove\n").scan_us, 125)
        e = self.errs("packages: { base: base.yaml }\nkey_matrix: !remove\n")
        self.assertIn("nothing to remove", e[0].msg)

    def test_extend_errors(self):
        e = self.errs("packages: { base: base.yaml }\nbuttons:\n  - id: !extend aa\n    cc: 5\n")
        self.assertRegex(e[0].msg, r"!extend aa: no item with id 'aa' in buttons.*did you mean 'a'")
        self.assertEqual((e[0].line, e[0].src), (3, None))
        e = self.errs(BASE.replace("- { id: b", "- { id: !remove b"))       # no package to remove from
        self.assertIn("nothing from a package to remove", e[0].msg)

    def test_errors_point_into_the_package(self):
        self.write("bad.yaml", "buttons:\n  - { id: x, pin: PA9, action: cc_momentary, cc: 300 }\n")
        dev, diags = self.load("packages: { base: base.yaml, more: bad.yaml }\n")
        self.assertIsNone(dev)
        e = gd.errors(diags)[0]
        self.assertEqual((e.src, e.line), (os.path.join(self.tmp, "bad.yaml"), 2))
        self.assertIn("300 is out of range", e.msg)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            gb.check(os.path.join(self.tmp, "main.yaml"))
        self.assertIn("bad.yaml:2: error: buttons[3].cc: 300 is out of range", err.getvalue())
        self.assertEqual(e.as_dict()["file"], os.path.join(self.tmp, "bad.yaml"))

    def test_yaml_error_in_include(self):
        self.write("broken.yaml", "leds: [\n")
        e = self.errs(BASE + "leds: !include broken.yaml\n")
        self.assertEqual(e[0].src, os.path.join(self.tmp, "broken.yaml"))
        self.assertIn("YAML syntax", e[0].msg)

    def test_include_cycle_and_missing(self):
        self.write("loop.yaml", "packages: [main.yaml]\n")
        e = self.errs("packages: [loop.yaml]\n")
        self.assertIn("includes itself (main.yaml -> loop.yaml -> main.yaml)", e[0].msg)
        e = self.errs("packages: [nope.yaml]\n")
        self.assertIn("can't read", e[0].msg)
        e = self.errs("packages: [github://cubicecho/gemifi/devices/x.yaml]\n")
        self.assertIn("remote packages aren't supported yet", e[0].msg)
        e = self.errs("leds: !nope x\n")
        self.assertIn("unknown tag !nope", e[0].msg)

    def test_nested_packages_relative_to_their_file(self):
        self.write("sub/inner.yaml", "packages: [../base.yaml]\nfriendly_name: Inner\n")
        dev = self.ok("packages: [sub/inner.yaml]\n")
        self.assertEqual((dev.name, len(dev.buttons)), ("Inner", 3))

    def test_preset_package(self):
        self.write("presets/kbd.yaml", BASE)
        dev = self.ok("packages: { base: preset:kbd }\nname: x\n", preset_dir=os.path.join(self.tmp, "presets"))
        self.assertEqual(len(dev.buttons), 3)
        e = self.errs("packages: { base: preset:kdb }\n", preset_dir=os.path.join(self.tmp, "presets"))
        self.assertIn("no preset 'kdb' (presets: kbd; did you mean 'kbd'?)", e[0].msg)

    def test_substitutions(self):
        dev = self.ok(BASE + "friendly_name: ${label} ${label}\n")
        self.assertEqual(dev.name, "Base Base")
        self.assertEqual(self.buttons(dev)[0], ("a", "cc_momentary", 1))     # "${cc_a}" alone: an int
        dev = self.ok(BASE + "friendly_name: ${label}\n", subs={"cc_a": "7", "label": "CLI"})
        self.assertEqual((dev.name, dev.buttons[0].number), ("CLI", 7))
        e = self.errs(BASE + "friendly_name: ${lable}\n")
        self.assertEqual(e[0].line, 14)
        self.assertIn("unknown substitution ${lable} (substitutions: cc_a, label; did you mean 'label'?)", e[0].msg)
        dev = self.ok(BASE.replace("label: Base", "label: ${who}-${cc_a}\n  who: me") + "friendly_name: ${label}\n")
        self.assertEqual(dev.name, "me-1")                                    # substitutions use others
        e = self.errs(BASE.replace("label: Base", "label: ${label}x") + "friendly_name: ${label}\n")
        self.assertIn("refer to each other in a loop", e[0].msg)

    def test_yaml_words_stay_refused(self):
        """A substituted `no` is the boolean it reads as, and the id check still says why."""
        e = self.errs(BASE.replace("label: Base", "label: Base\n  bid: no").replace("id: b,", "id: ${bid},"))
        self.assertIn("YAML", e[0].msg)

    def test_min_version(self):
        self.assertIsNotNone(self.ok(BASE + "min_version: 0.0.1\n"))
        e = self.errs(BASE + "min_version: 99.0.0\n")
        self.assertEqual(len(e), 1)
        self.assertEqual(e[0].msg, f"needs gemifi builder 99.0.0 or newer; this one is {gd.VERSION}: update "
                                   "gemifi (git pull, or the newer Docker image)")
        self.write("new.yaml", "min_version: 99.1.0\n")
        e = self.errs(BASE + "packages: [new.yaml]\n")
        self.assertEqual((e[0].src, e[0].line), (os.path.join(self.tmp, "new.yaml"), 1))
        self.assertIn("must be a version", self.errs(BASE + "min_version: 1.2\n")[0].msg)

    def test_config_hash(self):
        a = self.ok(BASE).config_hash
        self.assertTrue(0 <= a <= 0x7FFFFFFF)
        # comments, layout and how the file is split don't count; what it says does
        self.assertEqual(self.ok("# hi\n" + BASE.replace("{ vid: 0x1209, pid: 0x0002 }",
                                                         "\n  vid: 0x1209\n  pid: 0x0002")).config_hash, a)
        self.assertEqual(self.ok("packages: [base.yaml]\n").config_hash, a)
        self.assertNotEqual(self.ok(BASE.replace("cc: 2", "cc: 3")).config_hash, a)
        self.assertNotEqual(self.ok(BASE, subs={"cc_a": "2"}).config_hash, a)
        self.assertEqual(gy.canonical(gd.load_yaml("a: 0x10\nb: [x, 1.0]\nc: yes\n")),
                         '{"a":"int:0x10","b":["str:x","float:1.0"],"c":"bool:yes"}')
        h = gd.generate(self.ok(BASE), "x.yaml")["gm_device_config.h"]
        self.assertIn(f"#define GM_CONFIG_HASH 0x{a:08X}", h)

    def test_presets_unchanged(self):
        """Every preset resolves to itself: none uses the new keys, so validation is as before."""
        for p in glob.glob(os.path.join(REPO, "devices", "*.yaml")):
            with open(p, encoding="utf-8") as f:
                data = gd.load_yaml(f.read())
            res = gy.resolve(data, p)
            self.assertEqual((res.diags, res.files), ([], []), p)

    def test_new_device_uses_the_preset(self):
        p = gb.new_device("my-keys", "keystation-mini32-mk3", self.tmp)
        with open(p, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("packages:\n  base: preset:keystation-mini32-mk3\nname: my-keys\n", text)
        self.assertLess(len(text.splitlines()), 15)
        dev, diags = gd.load_device(p)
        self.assertIsNotNone(dev, diags)
        ref, _ = gd.load_device(os.path.join(REPO, "devices", "keystation-mini32-mk3.yaml"))
        self.assertEqual((dev.node_name, len(dev.buttons)), ("my-keys", len(ref.buttons)))
        self.assertEqual(gb.peek_info(p), ("nuvoton-nuc1xx", ref.description, ref.name, "my-keys"))
        c = gb.new_device("copy", "keystation-mini32-mk3", self.tmp, copy=True)
        with open(c, encoding="utf-8") as f:
            self.assertIn("key_matrix:", f.read())

    def test_source_hash_follows_packages(self):
        main = self.write("main.yaml", "packages: [base.yaml]\n")
        h = gb.source_hash(main)
        self.write("base.yaml", BASE.replace("cc: 2", "cc: 4"))
        self.assertNotEqual(gb.source_hash(main), h)
        plain = self.write("plain.yaml", BASE)
        with open(plain, "rb") as f:
            import hashlib
            self.assertEqual(gb.source_hash(plain), hashlib.sha256(f.read()).hexdigest())   # as before

    def test_readme_example(self):
        """core/README.md "Packages and substitutions": the example builds on the Keystation preset."""
        with open(os.path.join(REPO, "core", "README.md"), encoding="utf-8") as f:
            doc = f.read()
        text = re.search(r"### Packages and substitutions.*?```yaml\n(.*?)```", doc, re.S).group(1)
        self.write("pads.yaml", "description: with pads\n")
        self.write("leds.yaml", "leds:\n  - id: led_${first}\n    pin: PC${first}\n")
        text = text.replace("min_version: 0.34.0", f"min_version: {gd.VERSION}")
        dev = self.ok(text)
        ids = [b.id for b in dev.buttons]
        self.assertIn("extra", ids)
        self.assertNotIn("oct_up", ids)
        self.assertEqual((dev.node_name, dev.buttons[0].number, dev.scan_us), ("my-keys", 20, 125))
        self.assertIn("led_4", [x.id for x in dev.leds])

    def test_cli_substitution(self):
        p = self.write("main.yaml", BASE + "friendly_name: ${label}\n")
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(io.StringIO()):
            rc = gb.main(["generate", p, "-o", os.path.join(self.tmp, "cfg"), "-s", "label", "From CLI"])
        self.assertEqual(rc, 0)
        with open(os.path.join(self.tmp, "cfg", "gm_device_config.h"), encoding="utf-8") as f:
            self.assertIn('#define GM_DEVICE_NAME "From CLI"', f.read())


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestDashboardPackages(unittest.TestCase):
    """The dashboard: a new device is a short package file; the list has config hashes; a problem in a
    package names that file."""

    def setUp(self):
        import dashboard
        from tests.test_builder import raw_req, start_server
        self.raw_req = raw_req
        self.tmp = tempfile.mkdtemp()
        self.presets = os.path.join(self.tmp, "presets")
        self.mine = os.path.join(self.tmp, "mine")
        os.makedirs(self.presets)
        os.makedirs(self.mine)
        with open(os.path.join(self.presets, "base.yaml"), "w", encoding="utf-8") as f:
            f.write(BASE)
        self.roots = dashboard.Roots(self.mine, os.path.join(self.tmp, "build"), preset_dir=self.presets,
                                     readonly_presets=True)
        self.srv, self.port = start_server(self.roots)

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        shutil.rmtree(self.tmp)

    def req(self, path, body=None):
        st, _h, data = self.raw_req(self.port, "GET" if body is None else "POST", path, body, {})
        return st, json.loads(data)

    def test_listing_and_validate(self):
        with open(os.path.join(self.mine, "keys.yaml"), "w", encoding="utf-8") as f:
            f.write("packages: [preset:base]\nname: keys\n")
        _, j = self.req("/api/devices")
        devs = {d["id"]: d for d in j["devices"]}
        keys = devs["mine/keys.yaml"]
        self.assertEqual((keys["platform"], keys["node_name"]), ("nuvoton-nuc1xx", "keys"))   # from the package
        dev, _ = gd.load_device(os.path.join(self.mine, "keys.yaml"), preset_dir=self.presets)
        self.assertEqual(keys["config_hash"], dev.config_hash)
        self.assertIsNone(keys["built_config_hash"])
        out = os.path.join(self.tmp, "build", "mine", "keys")
        os.makedirs(out)
        with open(os.path.join(out, gb.CONFIG_HASH_FILE), "w") as f:
            f.write("0000002a\n")
        _, j = self.req("/api/devices")
        self.assertEqual({d["id"]: d for d in j["devices"]}["mine/keys.yaml"]["built_config_hash"], 42)
        st, j = self.req("/api/validate", {"id": "mine/keys.yaml",
                                           "text": "packages: [preset:base]\nbuttons:\n  - id: !extend a\n"
                                                   "    cc: 300\n"})
        self.assertEqual(j["diags"][0]["line"], 4)
        self.assertNotIn("file", j["diags"][0])                     # in the file itself
        st, j = self.req("/api/validate", {"id": "mine/keys.yaml", "text": "packages: [preset:base]\n"
                                                                          "substitutions: { cc_a: 300 }\n"})
        self.assertEqual((j["diags"][0]["file"], j["diags"][0]["line"]), ("presets/base.yaml", 11))
        self.assertFalse(j["ok"])
        st, j = self.req("/api/validate", {"id": "mine/keys.yaml", "text": "packages: [preset:base]\nname: keys\n"})
        self.assertEqual(j["config_hash"], dev.config_hash)


# ---------------------------------------------------------------------------------------------
# the JSON schema

def check_schema(schema, x, root=None, path="$"):
    """The JSON schema subset gemifi_schema uses -> a list of problems (enough for these tests)."""
    root = root or schema
    if "$ref" in schema:
        node = root
        for part in schema["$ref"].lstrip("#/").split("/"):
            node = node[part]
        return check_schema(node, x, root, path)
    out = []
    if "anyOf" in schema:
        results = [check_schema(s, x, root, path) for s in schema["anyOf"]]
        if all(results):
            # the problems of the branch of the right type, when there is one
            near = [r for r in results if not (len(r) == 1 and r[0].startswith(f"{path}: not "))]
            out += near[0] if len(near) == 1 else [f"{path}: matches none of anyOf"]
        return out
    t = schema.get("type")
    if t is not None:
        names = t if isinstance(t, list) else [t]
        kinds = {"object": dict, "array": list, "string": str, "boolean": bool, "null": type(None)}
        good = any((isinstance(x, (int, float)) and not isinstance(x, bool)) if n == "number" else
                   (isinstance(x, int) and not isinstance(x, bool)) if n == "integer" else isinstance(x, kinds[n])
                   for n in names)
        if not good:
            return [f"{path}: not {t}"]
    if "enum" in schema and x not in schema["enum"]:
        out.append(f"{path}: {x!r} not in enum")
    if "const" in schema and x != schema["const"]:
        out.append(f"{path}: not {schema['const']!r}")
    if isinstance(x, str) and "pattern" in schema and not re.search(schema["pattern"], x):
        out.append(f"{path}: {x!r} doesn't match {schema['pattern']}")
    if isinstance(x, (int, float)) and not isinstance(x, bool):
        if "minimum" in schema and x < schema["minimum"] or "maximum" in schema and x > schema["maximum"]:
            out.append(f"{path}: {x} out of range")
    if isinstance(x, dict):
        props = schema.get("properties", {})
        for k, v in x.items():
            if k in props:
                out += check_schema(props[k], v, root, f"{path}.{k}")
            elif schema.get("additionalProperties") is False:
                out.append(f"{path}: unknown key {k}")
            elif isinstance(schema.get("additionalProperties"), dict):
                out += check_schema(schema["additionalProperties"], v, root, f"{path}.{k}")
    if isinstance(x, list) and "items" in schema:
        for i, v in enumerate(x):
            out += check_schema(schema["items"], v, root, f"{path}[{i}]")
    return out


def plain(x):
    """A loaded tree as JSON would see it (yaml-language-server reads YAML 1.2: 0x10 is an int too)."""
    if isinstance(x, dict):
        return {str(k): plain(v) for k, v in x.items()}
    if isinstance(x, list):
        return [plain(v) for v in x]
    if isinstance(x, gd.Tagged):
        return x.value if isinstance(x.value, str) else plain(x.value)
    return x


def device_files():
    refs = os.path.join(REPO, "web", "updater", "src", "builder", "yaml-reference.json")
    with open(refs, encoding="utf-8") as f:
        examples = [e["yaml"] for e in json.load(f)["entries"]]
    files = sorted(glob.glob(os.path.join(REPO, "devices", "*.yaml")) +
                   glob.glob(os.path.join(TESTS, "fixtures", "*.yaml")))
    return files, examples


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestSchema(unittest.TestCase):
    def test_checked_in_schema_is_current(self):
        with open(os.path.join(REPO, gs.SCHEMA_FILE), encoding="utf-8") as f:
            self.assertEqual(f.read(), gs.schema_text(),
                             f"run `python3 tools/builder/gemifi_build.py schema -o {gs.SCHEMA_FILE}`")

    def test_devices_match_the_schema(self):
        schema = gs.device_schema()
        files, examples = device_files()
        for p in files:
            with open(p, encoding="utf-8") as f:
                text = f.read()
            self.assertEqual(check_schema(schema, plain(gd.load_yaml(text))), [], p)
            if "devices" + os.sep in p:
                self.assertTrue(text.startswith(f"# yaml-language-server: $schema={gs.SCHEMA_URL}\n"), p)
        for y in examples:
            self.assertEqual(check_schema(schema, plain(gd.load_yaml(y))), [], y)

    def test_schema_catches_mistakes(self):
        schema = gs.device_schema()
        bad = gd.load_yaml("gemifi: 2\nbuttons:\n  - { id: a, pin: PA1, acton: note }\nplatform: avr\n")
        problems = check_schema(schema, plain(bad))
        self.assertIn("$.buttons[0]: unknown key acton", problems)
        self.assertTrue(any(p.startswith("$.platform") for p in problems), problems)
        self.assertEqual(check_schema(schema, plain(gd.load_yaml("buttons: [{ cc: '${x}' }]\n"))), [])

    def test_keys_are_the_validators(self):
        """Every mapping the validator checks has exactly the schema's keys there (fixed sections) or
        a subset of them (maps keyed by component id), on every device file and example."""
        schema = gs.device_schema()
        seen = {}
        orig = gd._V.mapping

        def spy(self_, v, path, line, keys, renamed=None):
            seen.setdefault(re.sub(r"\[\d+\]", "[]", path), set(keys))
            return orig(self_, v, path, line, keys, renamed)

        files, examples = device_files()
        gd._V.mapping = spy
        try:
            for p in files:
                gd.load_device(p)
            for y in examples:
                gd.load_device("x.yaml", "gemifi: 2\nname: x\nboard_id: 0x3F01\nplatform: stm32f1\n"
                               "chip: STM32F103RC\nusb: { vid: 0x1209, pid: 0x0002 }\n" + y)
        finally:
            gd._V.mapping = orig
        self.assertIn("buttons[]", seen)

        def at(path):
            node, dynamic = schema, False
            for part in filter(None, re.split(r"\.|(\[\])", path)):
                node = self.resolve(schema, node, part != "[]")
                if part == "[]":
                    node = node["items"]
                elif part in node.get("properties", {}):
                    node = node["properties"][part]
                else:
                    node, dynamic = node["additionalProperties"], True
            return self.resolve(schema, node, True), dynamic

        for path, keys in seen.items():
            node, dynamic = at(path)
            props = set(node.get("properties", {}))
            if path == "":
                props -= set(gs.REUSE_KEYS)      # used up by gemifi_yaml.py before validation
            if dynamic:
                self.assertLessEqual(keys, props, path)
            else:
                self.assertEqual(keys, props, path)

    @staticmethod
    def resolve(schema, node, want_object):
        """Follow $ref, and into the object (or array) branch of an anyOf."""
        while True:
            if "$ref" in node:
                node = schema["definitions"][node["$ref"].split("/")[-1]]
            elif "anyOf" in node:
                want = "object" if want_object else "array"
                kinds = [s for s in node["anyOf"] if "$ref" in s or s.get("type") in ("object", "array")
                         or "anyOf" in s]
                node = next((s for s in kinds if s.get("type") == want), kinds[0])
            else:
                return node


if __name__ == "__main__":
    unittest.main()

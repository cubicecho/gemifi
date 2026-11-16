"""External components, ESPHome style (core/README.md "External components"): components from outside
gemifi's components/ folder, which add platforms to the existing bases (display platforms, hub types,
LED strips, LED drivers) for the device that names them:

    external_components:
      - source: ../my_components            # a folder of component folders (or a repo with a components/ one)
        components: [st7567]                # only these; leave out for all of them
      - source: { type: local, path: ../more }

    reg = registry(v, data, file)           # a components.Registry, or None after an error (in v.diags)

A component is a folder like gemifi's own (components/<name>/: __init__.py, C sources and headers, README),
whose __init__.py declares GEMIFI_API (components.api.API_VERSION), lists its HubType / ScreenType instances
in HUB_TYPES / DISPLAY_PLATFORMS, and optionally DEPENDENCIES and PLATFORMS (the targets it builds on). One
with the name of one of gemifi's own platform components replaces it (with a warning), keeping its GM_HUB_* /
GM_SCREEN_* numbers; the hub types core/src/vpin.c handles by number (HubType.core_paths) and the base and
control components can't be replaced. The builder numbers an external component's other types after the
built-in ones (GM_HUB_EXT_<KEY>, GM_SCREEN_EXT_<PLATFORM>: gm_components.h).

Git sources (github://owner/repo@ref) are #169's next step; for now a source is a local folder.
"""
import hashlib
import importlib.util
import os
import re
import sys
import threading
import traceback

import components
from components import api
from components.display import BUILTIN_TYPES as SCREEN_BUILTIN_TYPES, ScreenType
from components.hub import BUILTIN_TYPES as HUB_BUILTIN_TYPES, HubType

ITEM_KEYS = ["source", "components", "refresh"]
SOURCE_KEYS = ["type", "path", "url", "ref"]
NAME_RE = re.compile(r"[a-z][a-z0-9_]*")
HUB_TYPE_LIMIT = 32             # core/src/vpin.c GM_HUB_TYPES: a 32-bit mask, bit = GM_HUB_* number
# What makes a component a control or a base, not a platform: refused in an external one.
NOT_PLATFORM = {"in_device": "a control (in_device)", "REQUIRED": "built on every device (REQUIRED)",
                "KEY_SECTIONS": "a control's key tables (KEY_SECTIONS)"}
GIT_LATER = "git sources aren't supported yet (#169): use a local folder, `source: { type: local, path: ... }`"

_lock = threading.Lock()
_registries = {}                # (names, folders, digests) -> Registry: loaded once while the files stay the same


def files_digest(folder):
    """A hash of a component folder's files (names and bytes; __pycache__ and dotfiles left out)."""
    h = hashlib.sha256()
    for root, dirs, files in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if d != "__pycache__" and not d.startswith("."))
        for f in sorted(files):
            if f.startswith(".") or f.endswith(".pyc"):
                continue
            p = os.path.join(root, f)
            h.update(os.path.relpath(p, folder).replace(os.sep, "/").encode() + b"\0")
            with open(p, "rb") as fh:
                h.update(fh.read())
            h.update(b"\0")
    return h.hexdigest()


def component_dirs(folder):
    """The component folders (with an __init__.py) in a source folder, by name."""
    return {d: os.path.join(folder, d) for d in sorted(os.listdir(folder))
            if os.path.isfile(os.path.join(folder, d, "__init__.py"))}


class _Load:
    def __init__(self, v, file):
        self.v, self.base = v, os.path.dirname(os.path.abspath(file)) if file else os.getcwd()
        self.ok = True

    def err(self, path, line, msg):
        self.v.err(path, line, msg)
        self.ok = False

    def source_folder(self, item, p):
        """An item's source: -> its folder of components, or None after an error."""
        src, line = item.get("source"), item.line_of("source") or item.line
        if src is None:
            self.err(f"{p}.source", item.line, "is required: a local folder (`source: ../my_components`)")
            return None
        if isinstance(src, dict):
            if self.v.mapping(src, f"{p}.source", line, SOURCE_KEYS) is None:
                self.ok = False
                return None
            kind = src.get("type", "local")
            if kind == "git":
                self.err(f"{p}.source.type", src.line_of("type"), GIT_LATER)
                return None
            if kind != "local":
                self.err(f"{p}.source.type", src.line_of("type"), f"unknown source type '{kind}' (local)")
                return None
            for k in ("url", "ref"):
                if k in src:
                    self.err(f"{p}.source.{k}", src.line_of(k), f"{k}: is for git sources; {GIT_LATER}")
            path, line = src.get("path"), src.line_of("path") or src.line
            if not isinstance(path, str) or not path.strip():
                self.err(f"{p}.source.path", line, "is required: the folder of components")
                return None
        elif isinstance(src, str) and src.strip():
            if "://" in src:
                self.err(f"{p}.source", line, f"'{src}': {GIT_LATER}")
                return None
            path = src
        else:
            self.err(f"{p}.source", line, f"must be a folder or a mapping {{ type: local, path: ... }} (got {src!r})")
            return None
        # relative to the file that names it (a package's, an !include's, or the device file)
        base = os.path.dirname(os.path.abspath(line.file)) if getattr(line, "file", None) else self.base
        folder = os.path.normpath(os.path.join(base, os.path.expanduser(path.strip())))
        if not os.path.isdir(folder):
            self.err(f"{p}.source", line, f"'{path}': no such folder ({folder})")
            return None
        sub = os.path.join(folder, "components")
        if not component_dirs(folder) and os.path.isdir(sub):
            folder = sub            # a repo with its components in components/, as gemifi's own
        return folder

    def wanted(self, item, p, folder):
        """The components an item loads: {name: folder}, or None after an error."""
        have = component_dirs(folder)
        if "components" not in item:
            if not have:
                self.err(f"{p}.source", item.line_of("source") or item.line,
                         f"{folder} has no components (folders with an __init__.py)")
                return None
            return have
        names = self.v.seq(item["components"], f"{p}.components", item.line_of("components"))
        out = {}
        for i, n in enumerate(names):
            line = names.line_of(i) if hasattr(names, "line_of") else item.line_of("components")
            if not isinstance(n, str) or n not in have:
                known = ", ".join(have) or "none"
                self.err(f"{p}.components[{i}]", line, f"'{n}' isn't a component in {folder} (there: {known})")
                continue
            out[n] = have[n]
        return out if out or not names else None


def registry(v, data, file):
    """The components.Registry for a device file's external_components: (gemifi's own if it has none), or
    None after an error. Problems go to v (the validator) at their file and line."""
    ext = data.get("external_components")       # gemifi_device.EXTERNAL_KEY
    if ext is None:
        return components.builtin()
    ld = _Load(v, file)
    items = v.seq(ext, "external_components", data.line_of("external_components"))
    chosen = {}                 # name -> (folder, item path, line)
    for i, item in enumerate(items):
        p = f"external_components[{i}]"
        if v.mapping(item, p, getattr(item, "line", data.line_of("external_components")), ITEM_KEYS) is None:
            ld.ok = False
            continue
        if "refresh" in item:
            ld.err(f"{p}.refresh", item.line_of("refresh"), f"refresh: is for git sources; {GIT_LATER}")
        folder = ld.source_folder(item, p)
        if folder is None:
            continue
        for name, d in (ld.wanted(item, p, folder) or {}).items():
            if name in chosen:
                ld.err(p, item.line, f"component '{name}' is in two sources ({chosen[name][0]} and {d})")
                continue
            chosen[name] = (d, p, item.line)
    if not ld.ok:
        return None
    if not chosen:
        return components.builtin()
    digests = {name: files_digest(d) for name, (d, _, _) in chosen.items()}
    key = tuple((name, chosen[name][0], digests[name]) for name in sorted(chosen))
    with _lock:
        reg = _registries.get(key)
        if reg is None:
            reg = _build(ld, chosen, key)
            if reg is None:
                return None
            _registries[key] = reg
    # what a build loaded can't change between devices, but its warnings are each device's
    for path, line, msg in reg.ext_warnings(chosen):
        v.warn(path, line, msg)
    return reg


def _import(name, folder, key):
    """Load components/<name>/__init__.py from folder as a package of its own (relative imports work)."""
    tag = hashlib.sha256(repr(key).encode()).hexdigest()[:12]
    modname = f"_gemifi_ext_{tag}_{name}"
    spec = importlib.util.spec_from_file_location(modname, os.path.join(folder, "__init__.py"),
                                                  submodule_search_locations=[folder])
    mod = importlib.util.module_from_spec(spec)
    sys.modules[modname] = mod
    try:
        spec.loader.exec_module(mod)
    except BaseException:
        del sys.modules[modname]
        raise
    return mod


def _where(exc, folder):
    """file:line in the component where exc came from (the last frame in its folder), or ''."""
    frames = [f for f in traceback.extract_tb(exc.__traceback__) if os.path.abspath(f.filename).startswith(folder)]
    return f" ({frames[-1].filename}:{frames[-1].lineno})" if frames else ""


class _Registry(components.Registry):
    """A Registry with external components, and the warnings their loading gave (replacements)."""

    def __init__(self, external, warnings):
        super().__init__(external)
        self._warnings = warnings     # name -> message

    def ext_warnings(self, chosen):
        return [(p, line, self._warnings[name]) for name, (_, p, line) in chosen.items() if name in self._warnings]


def _build(ld, chosen, key):
    """Import and check the chosen components, number their types -> a Registry, or None after an error."""
    builtin = components.builtin()
    own = builtin.modules()
    mods, warnings = {}, {}
    for name, (folder, p, line) in chosen.items():
        try:
            mod = _import(name, folder, key)
        except SyntaxError as e:
            ld.err(p, line, f"component '{name}' doesn't load: {e.msg} ({e.filename}:{e.lineno})")
            continue
        except Exception as e:     # noqa: BLE001 (a component's own error, reported where it is)
            ld.err(p, line, f"component '{name}' doesn't load: {type(e).__name__}: {e}{_where(e, folder)}")
            continue
        if _check(ld, name, mod, own, p, line):
            mods[name] = (mod, folder)
            if name in own:
                warnings[name] = (f"component '{name}' from {folder} replaces gemifi's own "
                                  f"(components/{name})")
    if not ld.ok:
        return None
    names = set(own) | set(mods)
    for name, (mod, _) in mods.items():
        for dep in getattr(mod, "DEPENDENCIES", ()):
            if dep not in names:
                ld.err(chosen[name][1], chosen[name][2], f"component '{name}' depends on '{dep}', which isn't a "
                                                         "component (gemifi's own or external)")
    if not ld.ok or not _number(ld, mods, own, chosen):
        return None
    return _Registry(mods, warnings)


def _check(ld, name, mod, own, p, line):
    """An external component's declarations: its API version, that it only adds platforms, its types."""
    ok = True

    def err(msg):
        nonlocal ok
        ld.err(p, line, f"component '{name}': {msg}")
        ok = False
    want = getattr(mod, "GEMIFI_API", None)
    if want is None:
        err(f"declares no GEMIFI_API (the component API it was written for; this builder has {api.API_VERSION})")
    elif want != api.API_VERSION:
        err(f"was written for component API {want}; this builder has {api.API_VERSION}")
    for attr, what in NOT_PLATFORM.items():
        if hasattr(mod, attr):
            err(f"is {what}: an external component can only add hub types (HUB_TYPES) and display "
                "platforms (DISPLAY_PLATFORMS) to the existing bases")
    hubs, screens = getattr(mod, "HUB_TYPES", ()), getattr(mod, "DISPLAY_PLATFORMS", ())
    if not hubs and not screens:
        err("lists no HUB_TYPES or DISPLAY_PLATFORMS: an external component adds hub types or display platforms")
    for t in hubs:
        if not isinstance(t, HubType):
            err(f"HUB_TYPES holds {t!r}, not a HubType (components.hub)")
        elif not isinstance(t.key, str) or not NAME_RE.fullmatch(t.key):
            err(f"hub type key {t.key!r} must be lower_snake_case")
        elif not t.c_ops:
            err(f"hub type '{t.key}' has no c_ops (its gm_hub_ops_t, in the component's C)")
    for t in screens:
        if not isinstance(t, ScreenType):
            err(f"DISPLAY_PLATFORMS holds {t!r}, not a ScreenType (components.display)")
        elif not isinstance(t.platform, str) or not NAME_RE.fullmatch(t.platform):
            err(f"display platform {t.platform!r} must be lower_snake_case")
        elif not (t.c_ops and t.c_config and t.c_header):
            err(f"display platform '{t.platform}' needs c_header, c_config and c_ops")
    plats = getattr(mod, "PLATFORMS", None)
    if plats is not None:
        import gemifi_device as gd
        bad = [x for x in (plats if isinstance(plats, (list, tuple)) else [plats]) if x not in gd.PLATFORMS]
        if bad or not isinstance(plats, (list, tuple)):
            err(f"PLATFORMS must list targets ({', '.join(gd.PLATFORMS)}); got {plats!r}")
    if name in own:
        why = _not_replaceable(name, own)
        if why:
            err(f"can't replace gemifi's own components/{name}: {why}")
    return ok


def _not_replaceable(name, own):
    """Why gemifi's own component name can't be replaced, or None."""
    m = own[name]
    if not getattr(m, "HUB_TYPES", ()) and not getattr(m, "DISPLAY_PLATFORMS", ()):
        return "it isn't a platform (hub types or display platforms) but a base or a control"
    if any(name in getattr(o, "DEPENDENCIES", ()) for o in own.values()):
        return "other components build on it (a base)"
    core = [t.key for t in getattr(m, "HUB_TYPES", ()) if t.core_paths]
    if core:
        return (f"core/src/vpin.c handles {', '.join(core)} by number, with code of its own beyond its "
                "gm_hub_ops_t, which a replacement can't give")
    return None


def _number(ld, mods, own, chosen):
    """GM_HUB_* / GM_SCREEN_* names and numbers for the external types: a replacement's types keep the
    built-in ones of the same key; the others are numbered after the built-ins. Checks that no key or
    platform name clashes with another component's, or with a top-level key."""
    import gemifi_device as gd
    builtin_hubs = {t.key: t for m in own.values() for t in getattr(m, "HUB_TYPES", ())}
    builtin_screens = {t.platform: t for m in own.values() for t in getattr(m, "DISPLAY_PLATFORMS", ())}
    replaced = {n for n in mods if n in own}
    owner_hub = {t.key: n for n, m in own.items() if n not in replaced for t in getattr(m, "HUB_TYPES", ())}
    owner_screen = {t.platform: n for n, m in own.items() if n not in replaced
                    for t in getattr(m, "DISPLAY_PLATFORMS", ())}
    domains = {t.domain for t in builtin_hubs.values() if t.domain}
    taken = set(gd.TOP_KEYS_BEFORE_HUBS + gd.TOP_KEYS_AFTER_HUBS) | {gd.EXTERNAL_KEY}
    next_hub, next_screen = HUB_BUILTIN_TYPES, SCREEN_BUILTIN_TYPES
    seen_hub, seen_screen = {}, {}
    for name in sorted(mods):
        mod, _ = mods[name]
        p, line = chosen[name][1], chosen[name][2]
        for t in mod.HUB_TYPES if hasattr(mod, "HUB_TYPES") else ():
            clash = owner_hub.get(t.key) or seen_hub.get(t.key)
            if clash:
                ld.err(p, line, f"component '{name}': hub type '{t.key}' is also component '{clash}''s")
                continue
            if t.domain and t.domain not in domains:
                ld.err(p, line, f"component '{name}': hub type '{t.key}' has domain '{t.domain}', which isn't a "
                                f"base's list ({', '.join(sorted(domains))})")
                continue
            if not t.domain and (t.key in taken or (t.pin_key or t.key) in taken):
                ld.err(p, line, f"component '{name}': hub type '{t.key}' would be a top-level list, but that "
                                "key is taken")
                continue
            seen_hub[t.key] = name
            old = builtin_hubs.get(t.key) if name in replaced else None
            if old is not None:
                t.enum, t.num = old.enum, old.num
            else:
                t.enum, t.num = f"GM_HUB_EXT_{t.key.upper()}", next_hub
                next_hub += 1
        for t in mod.DISPLAY_PLATFORMS if hasattr(mod, "DISPLAY_PLATFORMS") else ():
            clash = owner_screen.get(t.platform) or seen_screen.get(t.platform)
            if clash:
                ld.err(p, line, f"component '{name}': display platform '{t.platform}' is also component "
                                f"'{clash}''s")
                continue
            seen_screen[t.platform] = name
            old = builtin_screens.get(t.platform) if name in replaced else None
            if old is not None:
                t.enum, t.num = old.enum, old.num
            else:
                t.enum, t.num = f"GM_SCREEN_EXT_{t.platform.upper()}", next_screen
                next_screen += 1
    if next_hub > HUB_TYPE_LIMIT:
        ld.err("external_components", chosen[min(chosen)][2],
               f"{next_hub - HUB_BUILTIN_TYPES} external hub types: at most {HUB_TYPE_LIMIT - HUB_BUILTIN_TYPES}")
    return ld.ok


def check_used(v, dev, reg, line=0):
    """After validation: each external component the device builds supports its target (PLATFORMS)."""
    if not reg.external:
        return
    hub_keys = sorted({h.kind.key for h in dev.hubs})
    used = reg.used(hub_keys, sorted({x.type for x in dev.screens}), reg.controls(dev))
    for name in used:
        if name not in reg.external:
            continue
        plats = getattr(reg.mods[name], "PLATFORMS", None)
        if plats is not None and dev.platform.name not in plats:
            v.err("external_components", line, f"component '{name}' ({reg.dirs[name]}) doesn't build on "
                                            f"{dev.platform.name} (its PLATFORMS: {', '.join(plats)})")


def config_digest(reg):
    """What the external components add to the config hash: their names and files' hashes, or b''."""
    if not reg.external:
        return b""
    return "\n".join(f"{n}:{files_digest(reg.dirs[n])}" for n in sorted(reg.external)).encode()


def data_digest(data, file):
    """config_digest() for a resolved device file without validating it (the dashboard's listing, the
    stale check): b'' when it has no external components or they don't load."""
    if not isinstance(data, dict) or "external_components" not in data:
        return b""
    import gemifi_device as gd
    reg = registry(gd._V(file), data, file)
    return config_digest(reg) if reg is not None else b""

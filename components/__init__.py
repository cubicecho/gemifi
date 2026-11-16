"""gemifi components, one folder each, as in ESPHome: components/<name>/ holds a component's builder
part (its __init__.py: schema, validation, generated C), its C sources, its README and tests. A base
component (hub, display, led_strip, led_driver) defines what the components on it (sn74hc595, ssd1306,
ws2812, tm1628 ...) implement (core/README.md "Components"). A control component (pads, ...) is built
when the device has that control (its in_device(dev)), or on every device when it is REQUIRED (leds: the
status LED and the lamp test need it).

A Registry is the set of components one device can use: gemifi's own, and an external component
(external_components:, core/README.md "External components") in place of or beside them. The functions
below (names(), modules(), hub_types() ...) answer for the current registry, which is builtin() unless
the builder is loading a device with external components (using()). The current one is a context
variable, so each dashboard thread sees its own device's.
"""
import contextlib
import contextvars
import importlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))


class Registry:
    """The components a device can use, by name: their modules and folders. external: {name: (module,
    folder)}, which win over gemifi's own of the same name."""

    def __init__(self, external=None):
        own = sorted(d for d in os.listdir(HERE) if os.path.isfile(os.path.join(HERE, d, "__init__.py")))
        self.dirs = {name: os.path.join(HERE, name) for name in own}
        self.mods = {name: importlib.import_module(f"{__name__}.{name}") for name in own}
        self.external = set()
        for name, (mod, folder) in (external or {}).items():
            self.dirs[name], self.mods[name] = folder, mod
            self.external.add(name)
        self.mods = dict(sorted(self.mods.items()))
        self.cache = {}         # what users build from it once (gemifi_device.py: its hub tables)

    def names(self):
        return list(self.mods)

    def modules(self):
        return dict(self.mods)

    def hub_types(self):
        return [t for m in self.mods.values() for t in getattr(m, "HUB_TYPES", ())]

    def display_platforms(self):
        return [t for m in self.mods.values() for t in getattr(m, "DISPLAY_PLATFORMS", ())]

    def controls(self, dev):
        return sorted(name for name, m in self.mods.items() if hasattr(m, "in_device") and m.in_device(dev))

    def used(self, hub_keys, screen_platforms=(), control_names=()):
        mods = self.mods
        owner = {t.key: name for name, m in mods.items() for t in getattr(m, "HUB_TYPES", ())}
        screen_owner = {t.platform: name for name, m in mods.items() for t in getattr(m, "DISPLAY_PLATFORMS", ())}
        want = [name for name, m in mods.items() if getattr(m, "REQUIRED", False)]
        want += [owner[k] for k in hub_keys if k in owner]
        want += [screen_owner[k] for k in screen_platforms if k in screen_owner]
        want += list(control_names)
        out = set()
        while want:
            name = want.pop()
            if name not in out:
                out.add(name)
                want += getattr(mods[name], "DEPENDENCIES", ())
        return sorted(out)

    def folder(self, name):
        """A component's folder: components/<name> from the repo root for gemifi's own, else absolute."""
        return f"components/{name}" if name not in self.external else self.dirs[name]

    def sources(self, name):
        return sorted(f"{self.folder(name)}/{f}" for f in os.listdir(self.dirs[name]) if f.endswith(".c"))

    def has_headers(self, name):
        return any(f.endswith(".h") for f in os.listdir(self.dirs[name]))


_builtin = []
_current = contextvars.ContextVar("gemifi_components", default=None)


def builtin():
    """gemifi's own components (made on first use: they import this package)."""
    if not _builtin:
        _builtin.append(Registry())
    return _builtin[0]


def current():
    """The registry of the device being loaded (builtin() outside using())."""
    return _current.get() or builtin()


@contextlib.contextmanager
def using(registry):
    """Make registry the current one for the code inside (this thread and context only)."""
    token = _current.set(registry)
    try:
        yield registry
    finally:
        _current.reset(token)


def names():
    """The components (folders with an __init__.py), sorted."""
    return current().names()


def modules():
    """Each component's module, by name."""
    return current().modules()


def hub_types():
    """The HubType instances every component lists in its HUB_TYPES."""
    return current().hub_types()


def display_platforms():
    """The ScreenType instances every component lists in its DISPLAY_PLATFORMS."""
    return current().display_platforms()


def controls(dev):
    """The control components (pads, ...) a device has: those whose in_device(dev) says so."""
    return current().controls(dev)


def used(hub_keys, screen_platforms=(), control_names=()):
    """The components a device builds, sorted: the REQUIRED ones, those whose HUB_TYPES (YAML keys) and
    DISPLAY_PLATFORMS (platform: names) it uses, the control components it has (control_names, from
    controls()), and what those name in DEPENDENCIES."""
    return current().used(hub_keys, screen_platforms, control_names)


def sources(name):
    """A component's C sources: paths from the repo root for gemifi's own, absolute for an external one."""
    return current().sources(name)


def has_headers(name):
    """Whether a component has C headers (its folder is then an include dir)."""
    return current().has_headers(name)


def folder(name):
    """A component's folder (its include dir): components/<name> from the repo root, absolute if external."""
    return current().folder(name)

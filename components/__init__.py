"""gemifi components, one folder each, as in ESPHome: components/<name>/ holds a component's builder
part (its __init__.py: schema, validation, generated C) and, as they move here, its C sources, docs and
tests. A base component (hub, display) defines what the components on it (sn74hc595, ssd1306 ...)
implement.
"""
import importlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def names():
    """The component folders (those with an __init__.py), sorted."""
    return sorted(d for d in os.listdir(HERE) if os.path.isfile(os.path.join(HERE, d, "__init__.py")))


def modules():
    """Each component's module, by name."""
    return {name: importlib.import_module(f"{__name__}.{name}") for name in names()}


def hub_types():
    """The HubType instances every component lists in its HUB_TYPES."""
    return [t for m in modules().values() for t in getattr(m, "HUB_TYPES", ())]


def display_platforms():
    """The ScreenType instances every component lists in its DISPLAY_PLATFORMS."""
    return [t for m in modules().values() for t in getattr(m, "DISPLAY_PLATFORMS", ())]


def used(hub_keys, screen_platforms=()):
    """The components a device builds, sorted: the REQUIRED ones, those whose HUB_TYPES (YAML keys) and
    DISPLAY_PLATFORMS (platform: names) it uses, and what those name in DEPENDENCIES."""
    mods = modules()
    owner = {t.key: name for name, m in mods.items() for t in getattr(m, "HUB_TYPES", ())}
    screen_owner = {t.platform: name for name, m in mods.items() for t in getattr(m, "DISPLAY_PLATFORMS", ())}
    want = [name for name, m in mods.items() if getattr(m, "REQUIRED", False)]
    want += [owner[k] for k in hub_keys if k in owner]
    want += [screen_owner[k] for k in screen_platforms if k in screen_owner]
    out = set()
    while want:
        name = want.pop()
        if name not in out:
            out.add(name)
            want += getattr(mods[name], "DEPENDENCIES", ())
    return sorted(out)


def sources(name):
    """A component's C sources, as paths from the repo root."""
    return sorted(f"components/{name}/{f}" for f in os.listdir(os.path.join(HERE, name)) if f.endswith(".c"))


def has_headers(name):
    """Whether a component has C headers (its folder is then an include dir)."""
    return any(f.endswith(".h") for f in os.listdir(os.path.join(HERE, name)))

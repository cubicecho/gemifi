"""gemifi components, one folder each, as in ESPHome: components/<name>/ holds a component's builder
part (its __init__.py: schema, validation, generated C) and, as they move here, its C sources, docs and
tests. A base component (hub) defines what its platform components (sn74hc595, ssd1306 ...) implement.
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

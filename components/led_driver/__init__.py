"""led_driver: the base component of LED drivers, chips that light LEDs and segment digits from a RAM of a
bit per output, ESPHome style: `led_driver: [{ id: digits, platform: max7219, ... }]`, pins
`{ led_driver: digits, number: n }`. Each platform is a hub type in a component of its own
(components/tm1628, max7219, tm1637, ht16k33) on LedDriver: outputs in the hub RAM, written from the
main loop within the send budget. The C side is led_driver.h (the bit pins most of them share);
README.md.
"""
from components.api import MIGRATE_HINT
from components.hub import HubType


class LedDriver(HubType):
    """An LED driver platform: outputs only, a bit each in the hub RAM, sent from the main loop."""
    domain = "led_driver"
    roles = frozenset({"out"})
    ram = True

    def moved_text(self):
        return (f"moved: an LED driver is `led_driver: [{{ id: digits, platform: {self.key}, ... }}]` now, "
                "ESPHome style, and its pins `{ led_driver: digits, number: n }`" + MIGRATE_HINT)

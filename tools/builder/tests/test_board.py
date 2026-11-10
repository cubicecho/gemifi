"""Board profiles (tools/builder/gemifi_board.py, issue #116): the profiles, what they generate, and how the
builder uses them (board:, reserved pins, the storage budget)."""
import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

TESTS = os.path.dirname(os.path.abspath(__file__))
BUILDER = os.path.dirname(TESTS)
REPO = os.path.dirname(os.path.dirname(BUILDER))
sys.path.insert(0, BUILDER)
import gemifi_board as gbd  # noqa: E402
import gemifi_device as gd  # noqa: E402

ARM_GCC = shutil.which("arm-none-eabi-gcc")


def raw_profile(platform, board):
    with open(gbd.profile_path(platform, board), encoding="utf-8") as f:
        return json.load(f)


class TestProfiles(unittest.TestCase):
    def test_every_profile_loads(self):
        for plat in gbd.PLATFORMS:
            names = gbd.boards(plat)
            self.assertEqual(names[0], gbd.DEFAULT_BOARD[plat])
            for b in names:
                p = gbd.load(plat, b)
                self.assertTrue(os.path.exists(os.path.join(gbd.profile_dir(plat), p["header"])), b)

    def test_code61_is_the_vendor_layout(self):
        """The values every Code 61 build used before profiles (its images must not change)."""
        p = gbd.load("stm32f1", "m-audio-code61")
        fl = p["flash"]
        self.assertEqual(p["board_id"], 2)
        self.assertEqual((p["pll_mul"], p["pll_hse_div2"]), (9, 0))
        self.assertEqual((fl["page"], fl["app_base"], fl["app_end"], fl["image_max"]),
                         (1024, 0x08002400, 0x0801B000, 102398))
        self.assertEqual((fl["config_base"], fl["config_pages"], fl["config_min"], fl["config_max"]),
                         (0x0801EC00, 3, 0x0801B400, 0x0801F800))
        self.assertEqual((fl["flag_page"], p["ram"]["size"], p["ram"]["mailbox"]), (0x0801FC00, 0x5000, 0x20004FF0))
        h = gbd.gen_header(p, 2)
        for line in ("#define BOARD_USB_PULLUP_PIN 8", "#define BOARD_RECOVERY_DRIVE_MASK 0x00F8u",
                     "#define BOARD_RECOVERY_PULL_MASK 0xFF00u", "#define BOARD_RECOVERY_MASK 0x0300u",
                     "#define BOARD_POWER_LATCH_PIN 15", "#define BOARD_SWJ_NOJTAG 1"):
            self.assertIn(line, h)
        self.assertNotIn("BOARD_UPDATE_LED", h)
        self.assertIn("ORIGIN = 0x08002400, LENGTH = 0x18C00", gbd.gen_ld(p, "app"))
        self.assertIn("ORIGIN = 0x08000000, LENGTH = 0x2400", gbd.gen_ld(p, "boot"))
        self.assertIn("--hdr-offset 0x130 --base 0x8002400 --max-size 102398", gbd.gen_mk(p, 2, "/x"))

    def test_keystation(self):
        p = gbd.load("nuvoton-nuc1xx", "keystation-mini32-mk3")
        self.assertEqual((p["pllcon"], p["flash"]["config_pages"]), (0xC22E, 7))
        self.assertIn("ORIGIN = 0x20000000, LENGTH = 0xFF0", gbd.gen_ld(p, "app"))
        self.assertIn("ORIGIN = 0x00000000, LENGTH = 0x1000", gbd.gen_ld(p, "boot"))

    def test_generic_c8(self):
        p = gbd.load("stm32f1", "generic-stm32f103c8")
        fl = p["flash"]
        self.assertIsNone(p["board_id"])
        self.assertEqual((fl["app_end"], fl["config_base"], fl["flag_page"]), (0x0800F000, 0x0800F000, 0x0800FC00))
        h = gbd.gen_header(p, 0x3F03)
        self.assertIn("#define BOARD_RECOVERY_NONE 1", h)
        self.assertIn("#define BOARD_UPDATE_LED_PIN 13", h)
        self.assertNotIn("BOARD_USB_PULLUP_PIN", h)
        self.assertNotIn("BOARD_POWER", h)
        self.assertEqual(p["reserved"], {})
        with self.assertRaisesRegex(gbd.ProfileError, "no board_id of its own"):
            gbd.board_id_for(p)
        with self.assertRaisesRegex(gbd.ProfileError, "DIY"):
            gbd.board_id_for(p, 0x0005)
        with self.assertRaisesRegex(gbd.ProfileError, "doesn't match"):
            gbd.board_id_for(gbd.load("stm32f1", "m-audio-code61"), 0x3F03)

    def test_pll_from_the_crystal(self):
        self.assertEqual(gbd.stm32f1_pll(8_000_000), (9, 0))
        self.assertEqual(gbd.stm32f1_pll(12_000_000), (6, 0))
        self.assertEqual(gbd.stm32f1_pll(16_000_000), (9, 1))
        self.assertEqual(gbd.stm32f1_pll(6_000_000), (12, 0))
        for bad in (10_000_000, 25_000_000, 3_000_000, 7_372_800):
            with self.assertRaises(gbd.ProfileError):
                gbd.stm32f1_pll(bad)

    def check_raw(self, raw, platform="stm32f1"):
        return gbd.resolve(raw, platform, "t")

    def test_profile_checks(self):
        base = raw_profile("stm32f1", "generic-stm32f103c8")
        cases = [
            (("flash", "page"), 2048, "boot_size"),                # 0x2400 isn't a 2 KB page boundary
            (("flash", "config_base"), "0x0800F200", "page boundary"),
            (("flash", "config_pages"), 1, "2 to 8"),
            (("ram", "mailbox"), "0x20004000", "top 16 bytes"),
            (("crystal_hz",), 10_000_000, "72 MHz"),
            (("usb_pullup",), {"pin": "PZ1"}, "not a pin"),
            (("flash", "base"), "0x08001000", "0x08000000"),
            (("bogus",), 1, "unknown key"),
        ]
        for path, value, msg in cases:
            raw = copy.deepcopy(base)
            d = raw
            for k in path[:-1]:
                d = d[k]
            d[path[-1]] = value
            with self.subTest(path=path), self.assertRaisesRegex(gbd.ProfileError, msg):
                self.check_raw(raw)
        raw = copy.deepcopy(base)
        raw["flash"].update(page=2048, boot_size="0x2800")           # a 2 KB-page part: page-aligned boot
        raw["flash"]["size"] = "0x40000"
        p = self.check_raw(raw)
        self.assertEqual((p["flash"]["app_base"], p["flash"]["flag_page"]), (0x08002800, 0x0803F800))
        with self.assertRaisesRegex(gbd.ProfileError, "platform"):
            gbd.resolve(base, "nuvoton-nuc1xx", "t")
        with self.assertRaisesRegex(gbd.ProfileError, "no board profile"):
            gbd.load("stm32f1", "nope")

    def test_recovery_on_a_plain_gpio(self):
        raw = raw_profile("stm32f1", "generic-stm32f103c8")
        raw["recovery"] = {"what": "the BOOT button", "pins": ["PA0"], "active": 1}
        p = self.check_raw(raw)
        h = gbd.gen_header(p, 0x3F09)
        self.assertIn("#define BOARD_RECOVERY_PORT 0", h)
        self.assertIn("#define BOARD_RECOVERY_MASK 0x0001u", h)
        self.assertIn("#define BOARD_RECOVERY_ACTIVE 1", h)
        self.assertNotIn("BOARD_RECOVERY_DRIVE", h)
        self.assertNotIn("BOARD_RECOVERY_NONE", h)
        self.assertEqual([lvl for lvl, _ in gbd.pin_check(p, 0, "out")], ["warning"])
        self.assertEqual(gbd.pin_check(p, 0, "in"), [])     # reading the button later is fine
        raw["recovery"]["pins"] = ["PA0", "PB1"]
        with self.assertRaisesRegex(gbd.ProfileError, "one port"):
            self.check_raw(raw)

    @unittest.skipUnless(ARM_GCC, "needs arm-none-eabi-gcc")
    def test_board_variants_compile(self):
        """The bootloader, recovery.c and the app's soft-power code for boards with and without each
        board pin."""
        base = raw_profile("stm32f1", "generic-stm32f103c8")
        plain = copy.deepcopy(base)
        plain["recovery"] = {"pins": ["PA0"], "active": 1}
        plain["usb_pullup"] = {"pin": "PA10", "active_low": True}
        plain["crystal_hz"] = 16_000_000
        plain["update_led"] = None
        drive = copy.deepcopy(base)
        drive["recovery"] = {"pins": ["PB8"], "drive": {"pins": ["PA3"], "level": 1}, "active": 0}
        plat = os.path.join(REPO, "platform", "stm32f1")
        srcs = [os.path.join(REPO, "bootloader", "stm32f1", "main.c"), os.path.join(plat, "src", "recovery.c"),
                os.path.join(plat, "src", "sys.c"), os.path.join(plat, "src", "usbd.c")]
        for name, raw in (("plain", plain), ("drive", drive), ("none", base)):
            with self.subTest(name), tempfile.TemporaryDirectory() as d:
                path = os.path.join(d, "t.json")
                with open(path, "w", encoding="utf-8") as f:
                    json.dump(raw, f)
                gbd.generate("stm32f1", "t", d, 0x3F09, path=path)
                r = subprocess.run([ARM_GCC, "-mcpu=cortex-m3", "-mthumb", "-std=gnu11", "-Wall", "-Wextra",
                                    "-Wno-unused-parameter", "-Werror", "-fsyntax-only", "-DGEMIFI_BOOTLOADER",
                                    "-DBOOT_VERSION_MAJOR=0", "-DBOOT_VERSION_MINOR=1", "-DBOOT_VERSION_PATCH=2",
                                    "-I" + d, "-I" + os.path.join(plat, "include"),
                                    "-I" + os.path.join(REPO, "core", "include"), "-I" + os.path.join(plat, "boards"),
                                    '-DBOARD_HEADER="generic-stm32f1.h"', *srcs], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)


DEVICE = """\
gemifi: 2
name: t
board_id: {bid}
platform: {plat}
chip: {chip}
{board}usb: {{ vid: 0x1209, pid: 0x0002 }}
buttons:
  - {{ id: a, pin: {{ number: {pin}, mode: INPUT_PULLUP, inverted: true }}, action: cc_momentary, cc: 1 }}
leds:
  - {{ id: l, pin: {led} }}
"""


def load(bid="0x0002", plat="stm32f1", chip="STM32F103RB", board=None, pin="PB12", led="PB13"):
    text = DEVICE.format(bid=bid, plat=plat, chip=chip, board=f"board: {board}\n" if board else "", pin=pin, led=led)
    data = gd.load_yaml(text, "t.yaml")
    return gd.validate(data, "t.yaml")


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestBuilderBoard(unittest.TestCase):
    def msgs(self, diags, level=None):
        return [d.msg for d in diags if level is None or d.level == level]

    def test_default_board(self):
        dev, diags = load()
        self.assertEqual(dev.board["id"], "m-audio-code61")
        self.assertFalse(dev.board_given)
        self.assertIsNone(gd.board_mismatch(dev))
        self.assertIn('#define GM_BOARD_PROFILE "m-audio-code61"', gd.generate(dev, "t.yaml")["gm_device_config.h"])
        dev, _ = load("0x0001", "nuvoton-nuc1xx", "NUC120LC1BN", pin="PA1", led="PA10")
        self.assertEqual(dev.board["id"], "keystation-mini32-mk3")

    def test_old_files_stay_valid(self):
        """No board: and a DIY board_id (every file from before profiles): valid, but not buildable."""
        dev, diags = load("0x3F11")
        self.assertIsNotNone(dev)
        self.assertEqual(self.msgs(diags), [])
        self.assertIn("isn't the board_id of board 'm-audio-code61'", gd.board_mismatch(dev))
        self.assertIn("generic-stm32f103c8", gd.board_mismatch(dev))

    def test_board_id_must_match(self):
        dev, diags = load("0x3F11", board="m-audio-code61")
        self.assertIsNone(dev)
        self.assertTrue(any("isn't the board_id" in m for m in self.msgs(diags, "error")))
        dev, diags = load("0x0002", chip="STM32F103C8", board="generic-stm32f103c8")
        self.assertIsNone(dev)
        self.assertTrue(any("DIY range" in m for m in self.msgs(diags, "error")))
        dev, diags = load("0x3F03", chip="STM32F103C8", board="generic-stm32f103c8")
        self.assertIsNotNone(dev, self.msgs(diags))
        self.assertIsNone(gd.board_mismatch(dev))

    def test_unknown_and_misplaced(self):
        dev, diags = load(board="generic-stm32f103")
        self.assertIsNone(dev)
        self.assertTrue(any("did you mean 'generic-stm32f103c8'" in m for m in self.msgs(diags, "error")))
        dev, diags = load("0x3F01", "esp32s3", "esp32-s3-devkitc-1", board="m-audio-code61", pin="GPIO9", led="GPIO10")
        self.assertIsNone(dev)
        self.assertTrue(any("board profiles are for" in m for m in self.msgs(diags, "error")))

    def test_chip_warning(self):
        _dev, diags = load("0x3F03", chip="STM32F103RB", board="generic-stm32f103c8")
        self.assertTrue(any("isn't the part" in m for m in self.msgs(diags, "warning")))

    def test_reserved_pins(self):
        cases = [("PA8", "PB13", "error", "USB D+ pull-up"), ("PB12", "PC15", "error", "soft-power latch"),
                 ("PC13", "PB13", "error", "power button"), ("PB12", "PB8", "warning", "read at reset")]
        for pin, led, level, text in cases:
            with self.subTest(pin=pin, led=led):
                _dev, diags = load(pin=pin, led=led)
                self.assertTrue(any(text in m for m in self.msgs(diags, level)), self.msgs(diags))
        _dev, diags = load(pin="PB8")                       # a recovery pin as an input: fine
        self.assertEqual([m for m in self.msgs(diags) if "board profile" in m], [])
        _dev, diags = load("0x3F03", chip="STM32F103C8", board="generic-stm32f103c8", pin="PA8", led="PC15")
        self.assertEqual([m for m in self.msgs(diags) if "board profile" in m], [])

    def test_storage_budget_from_the_profile(self):
        dev, _ = load()
        self.assertEqual((dev.budget["page"], dev.budget["pages"]), (1024, 3))
        p = gbd.load("stm32f1", "generic-stm32f103c8")
        dev.board = dict(p, flash=dict(p["flash"], config_pages=5))
        b = gd.storage_budget(dev)
        self.assertEqual(b["pages"], 5)
        self.assertGreater(b["capacity"], dev.budget["capacity"])


if __name__ == "__main__":
    unittest.main()

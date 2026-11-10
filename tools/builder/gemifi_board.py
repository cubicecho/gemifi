#!/usr/bin/env python3
"""gemifi board profiles: what a board is, as data (issue #116). Standard library only.

A board profile is a JSON file, platform/<platform>/boards/<board>.json, next to the board's C header. It says
which part of the chip gemifi may use and which pins the board itself needs:

  - the crystal (crystal_hz), from which the clock setup is derived (stm32f1: PLLMUL and HSE / 2);
  - the flash layout: size, page size, the bootloader's size, the app area, the config-store pages, the
    update-flag page (stm32f1);
  - the RAM size and the SRAM mailbox address;
  - optional board pins: a switched USB D+ pull-up, soft power (button + latch), the reset-time recovery
    combo, an update-mode LED, and other reserved pins, each with the reason.

Two users read it:

  - make (platform/board-profile.mk) runs `gemifi_board.py gen` before compiling: it writes board_layout.h
    (the BOARD_* layout macros), board_app.ld / board_boot.ld (the linker MEMORY regions and layout
    symbols), board.mk (BOARD_ID, the board header, mkimage's flags) and layout.json (the image layout that
    mkimage, gemifi.py and the web updater check a .gfw against) into <build>/board/.
  - the builder (gemifi_device.py): the `board:` key picks the profile; its reserved pins go into the
    platform check, its config pages into the storage budget.

  gemifi_board.py gen <platform> <board> -o <dir> [--board-id 0x3F03]
  gemifi_board.py list [<platform>]
  gemifi_board.py show <platform> <board>
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PLATFORM_DIR = os.path.join(REPO, "platform")

# The board each platform builds when the device YAML has no `board:` (what every build used before #116).
DEFAULT_BOARD = {"stm32f1": "m-audio-code61", "nuvoton-nuc1xx": "keystation-mini32-mk3"}
PLATFORMS = tuple(DEFAULT_BOARD)
DIY_IDS = range(0x3F00, 0x4000)       # protocol/sysex.md section 7
BOARD_ID_MAX = 0x3FFF

# Where the .gfw header sits: right after the vector table (16 core + 60 IRQs on stm32f1, + 32 on the NUC).
HDR_OFFSET = {"stm32f1": 0x130, "nuvoton-nuc1xx": 0x0C0}
STACK_GUARD = {"stm32f1": 0x100, "nuvoton-nuc1xx": 0}   # bytes kept free above the initial SP (app.ld)
MAILBOX_SIZE = 16

# stm32f1: the PLL must make 72 MHz (USB = PLL / 1.5 = 48 MHz), which every timer and delay in the platform
# assumes. Input: the HSE or HSE / 2; PLLMUL 2..16 (RM0008 7.3.2). An HSE crystal is 4-16 MHz.
STM32F1_SYSCLK = 72_000_000
STM32F1_HSE_RANGE = (4_000_000, 16_000_000)
# NUC1xx: PLLCON values for HCLK = USB clock = 48 MHz, by crystal. Only 12 MHz is tested on hardware.
NUC_PLLCON = {12_000_000: 0xC22E}

PORTS = "ABCDEFG"
ROLES = ("in", "out", "analog")


class ProfileError(ValueError):
    pass


def profile_dir(platform):
    return os.path.join(PLATFORM_DIR, platform, "boards")


def profile_path(platform, board):
    return os.path.join(profile_dir(platform), f"{board}.json")


def boards(platform):
    """The profiles a platform has, sorted (the default first)."""
    d = profile_dir(platform)
    names = sorted(f[:-5] for f in os.listdir(d) if f.endswith(".json")) if os.path.isdir(d) else []
    first = DEFAULT_BOARD.get(platform)
    return ([first] if first in names else []) + [n for n in names if n != first]


def _int(v, what):
    if isinstance(v, bool):
        raise ProfileError(f"{what}: must be a number (got {v!r})")
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        try:
            return int(v.replace("_", ""), 0)
        except ValueError:
            pass
    raise ProfileError(f"{what}: must be a number or a \"0x...\" string (got {v!r})")


def parse_pin(v, what):
    """'PA8' -> 8 (port * 16 + bit)."""
    m = re.fullmatch(r"P([A-G])(\d{1,2})", str(v).strip().upper())
    if not m or int(m.group(2)) > 15:
        raise ProfileError(f"{what}: '{v}' is not a pin (e.g. PA8)")
    return PORTS.index(m.group(1)) * 16 + int(m.group(2))


def pin_name(n):
    return f"P{PORTS[n // 16]}{n % 16}"


def _keys(d, what, allowed):
    if not isinstance(d, dict):
        raise ProfileError(f"{what}: must be an object")
    extra = sorted(set(d) - set(allowed))
    if extra:
        raise ProfileError(f"{what}: unknown key{'s' if len(extra) > 1 else ''} {', '.join(extra)} "
                           f"(known: {', '.join(allowed)})")


def _pins(v, what):
    if not isinstance(v, list) or not v:
        raise ProfileError(f"{what}: must be a list of pins")
    return [parse_pin(p, f"{what}[{i}]") for i, p in enumerate(v)]


def _one_port(pins, what):
    ports = {p // 16 for p in pins}
    if len(ports) != 1:
        raise ProfileError(f"{what}: all pins must be on one port (got {', '.join(map(pin_name, pins))})")
    return ports.pop(), sum(1 << (p % 16) for p in pins)


def _level(v, what):
    if v not in (0, 1):
        raise ProfileError(f"{what}: must be 0 or 1")
    return v


TOP = ["name", "short_name", "platform", "board_id", "chip", "header", "crystal_hz", "flash", "ram",
       "usb_pullup", "swj_nojtag", "soft_power", "recovery", "update_led", "reserved", "note"]
FLASH_F1 = ["base", "size", "page", "boot_size", "app_end", "image_max", "config_base", "config_pages",
            "config_window", "flag_page", "note"]
FLASH_NUC = ["aprom_size", "page", "ldrom_size", "data_base", "data_size", "note"]
RAM = ["base", "size", "mailbox", "note"]


def load(platform, board, path=None):
    """Read and check a profile. -> dict with every value resolved (ints, pin numbers, masks)."""
    if platform not in PLATFORMS:
        raise ProfileError(f"platform '{platform}' has no board profiles (only {', '.join(PLATFORMS)})")
    path = path or profile_path(platform, board)
    if not os.path.exists(path):
        known = ", ".join(boards(platform)) or "none"
        raise ProfileError(f"no board profile '{board}' for {platform} (known: {known})")
    try:
        with open(path, encoding="utf-8") as f:
            raw = json.load(f)
    except json.JSONDecodeError as e:
        raise ProfileError(f"{os.path.relpath(path, REPO)}: {e}") from None
    try:
        return resolve(raw, platform, board)
    except ProfileError as e:
        raise ProfileError(f"{os.path.relpath(path, REPO)}: {e}") from None


def resolve(raw, platform, board):
    _keys(raw, "profile", TOP)
    if raw.get("platform") != platform:
        raise ProfileError(f"platform: is '{raw.get('platform')}', but the profile is under platform/{platform}")
    p = {"id": board, "platform": platform, "name": str(raw.get("name") or board),
         "short_name": str(raw.get("short_name") or raw.get("name") or board),
         "chip": raw.get("chip"), "header": raw.get("header") or f"{board}.h"}
    if not re.fullmatch(r"[\w.-]+\.h", p["header"]):
        raise ProfileError(f"header: '{p['header']}' is not a header file name")
    if p["chip"] is not None:
        try:
            re.compile(p["chip"])
        except re.error as e:
            raise ProfileError(f"chip: not a regular expression ({e})") from None
    bid = raw.get("board_id")
    p["board_id"] = None if bid is None else _int(bid, "board_id")
    if p["board_id"] is not None and not 0 < p["board_id"] <= BOARD_ID_MAX:
        raise ProfileError("board_id: must be 0x0001..0x3FFF (or null: the device YAML's, in the DIY range)")
    p["crystal_hz"] = _int(raw.get("crystal_hz"), "crystal_hz")
    if platform == "stm32f1":
        _resolve_f1(raw, p)
    else:
        _resolve_nuc(raw, p)
    p["reserved"] = _reserved(raw, p)
    return p


def stm32f1_pll(hse):
    """-> (pllmul, hse / 2) for SYSCLK = 72 MHz, or raise ProfileError."""
    lo, hi = STM32F1_HSE_RANGE
    if not lo <= hse <= hi:
        raise ProfileError(f"crystal_hz: {hse} is outside the STM32F1 HSE crystal range ({lo}..{hi})")
    for div2 in (0, 1):
        f_in = hse // (2 if div2 else 1)
        if f_in * (2 if div2 else 1) != hse or STM32F1_SYSCLK % f_in:
            continue
        mul = STM32F1_SYSCLK // f_in
        if 2 <= mul <= 16:
            return mul, div2
    raise ProfileError(f"crystal_hz: {hse} Hz can't make 72 MHz with the STM32F1 PLL (HSE or HSE / 2, x2..x16); "
                       "use 6, 8, 9, 12 or 16 MHz")


def _resolve_f1(raw, p):
    p["pll_mul"], p["pll_hse_div2"] = stm32f1_pll(p["crystal_hz"])
    fl = raw.get("flash")
    _keys(fl, "flash", FLASH_F1)
    base, size, page = (_int(fl.get(k), f"flash.{k}") for k in ("base", "size", "page"))
    if page not in (1024, 2048):
        raise ProfileError("flash.page: STM32F1 pages are 1024 (low/medium density) or 2048 (high density)")
    if base != 0x08000000:
        raise ProfileError("flash.base: STM32F1 main flash starts at 0x08000000")
    if size % page or not 16 * 1024 <= size <= 1024 * 1024:
        raise ProfileError("flash.size: must be a whole number of pages, 16 KB..1 MB")
    end = base + size

    def aligned(v, what):
        if v % page:
            raise ProfileError(f"{what}: 0x{v:08X} is not on a {page}-byte page boundary")
        return v

    boot = aligned(_int(fl.get("boot_size"), "flash.boot_size"), "flash.boot_size")
    if boot < 0x2000:
        raise ProfileError("flash.boot_size: the gemifi bootloader needs at least 8 KB")
    app = base + boot
    flag = aligned(_int(fl.get("flag_page", end - page), "flash.flag_page"), "flash.flag_page")
    if not app < flag < end:
        raise ProfileError("flash.flag_page: must be a page inside the flash, above the app")
    n_cfg = _int(fl.get("config_pages", 3), "flash.config_pages")
    if not 2 <= n_cfg <= 8:
        raise ProfileError("flash.config_pages: 2 to 8 pages (platform/stm32f1/src/config_store.c)")
    cfg = aligned(_int(fl.get("config_base", flag - n_cfg * page), "flash.config_base"), "flash.config_base")
    cfg_end = cfg + n_cfg * page
    app_end = aligned(_int(fl.get("app_end", cfg), "flash.app_end"), "flash.app_end")
    win = fl.get("config_window", [app_end, flag])
    if not isinstance(win, list) or len(win) != 2:
        raise ProfileError("flash.config_window: must be [lowest, end] (the config pages must lie in it)")
    w_lo, w_hi = (_int(w, "flash.config_window") for w in win)
    if not (app_end <= w_lo <= cfg and cfg_end <= w_hi <= flag):
        raise ProfileError(f"flash.config_base: the config pages 0x{cfg:08X}..0x{cfg_end - 1:08X} must lie in "
                           f"0x{w_lo:08X}..0x{w_hi - 1:08X}, above the app (0x{app_end:08X}) and below the "
                           f"flag page (0x{flag:08X})")
    if cfg_end - cfg > 0x10000:
        raise ProfileError("flash.config_pages: at most 64 KB (the store keeps 16-bit offsets)")
    if not app + 0x1000 <= app_end:
        raise ProfileError("flash.app_end: the app area is too small")
    img_max = _int(fl.get("image_max", app_end - app), "flash.image_max")
    if img_max % 2 or not app_end - app <= img_max <= cfg - app:
        raise ProfileError("flash.image_max: between the app area's size and the config pages")
    p["flash"] = {"base": base, "size": size, "page": page, "boot_size": boot, "app_base": app,
                  "app_end": app_end, "image_max": img_max, "config_base": cfg, "config_pages": n_cfg,
                  "config_min": w_lo, "config_max": w_hi, "flag_page": flag}
    _resolve_ram(raw, p, 0x20000000, (6 * 1024, 96 * 1024))

    p["swj_nojtag"] = bool(raw.get("swj_nojtag", False))
    up = raw.get("usb_pullup")
    if up is not None:
        _keys(up, "usb_pullup", ["pin", "active_low", "note"])
        up = {"pin": parse_pin(up.get("pin"), "usb_pullup.pin"), "active_low": bool(up.get("active_low", False))}
    p["usb_pullup"] = up
    sp = raw.get("soft_power")
    if sp is not None:
        _keys(sp, "soft_power", ["button", "latch", "note"])
        sp = {"button": parse_pin(sp.get("button"), "soft_power.button"),
              "latch": parse_pin(sp.get("latch"), "soft_power.latch")}
    p["soft_power"] = sp
    rc = raw.get("recovery")
    if rc is not None:
        _keys(rc, "recovery", ["what", "pins", "active", "pull", "drive", "swj_nojtag", "note"])
        pins = _pins(rc.get("pins"), "recovery.pins")
        port, mask = _one_port(pins, "recovery.pins")
        pull = _pins(rc["pull"], "recovery.pull") if "pull" in rc else pins
        pport, pmask = _one_port(pull, "recovery.pull")
        if pport != port:
            raise ProfileError("recovery.pull: must be on the same port as recovery.pins")
        drive = rc.get("drive")
        if drive is not None:
            _keys(drive, "recovery.drive", ["pins", "level"])
            dpins = _pins(drive.get("pins"), "recovery.drive.pins")
            dport, dmask = _one_port(dpins, "recovery.drive.pins")
            if dport == port and dmask & (mask | pmask):
                raise ProfileError("recovery.drive.pins: overlap recovery.pins / pull")
            drive = {"port": dport, "mask": dmask, "level": _level(drive.get("level", 0), "recovery.drive.level"),
                     "pins": dpins}
        rc = {"what": str(rc.get("what", "")), "pins": pins, "port": port, "mask": mask,
              "pull_mask": pmask | mask, "pull": pull, "active": _level(rc.get("active", 0), "recovery.active"),
              "drive": drive, "swj_nojtag": bool(rc.get("swj_nojtag", False))}
    p["recovery"] = rc
    led = raw.get("update_led")
    if led is not None:
        _keys(led, "update_led", ["pin", "active_low", "note"])
        led = {"pin": parse_pin(led.get("pin"), "update_led.pin"), "active_low": bool(led.get("active_low", False))}
    p["update_led"] = led


def _resolve_ram(raw, p, base_default, size_range):
    ram = raw.get("ram")
    _keys(ram, "ram", RAM)
    base = _int(ram.get("base", base_default), "ram.base")
    size = _int(ram.get("size"), "ram.size")
    lo, hi = size_range
    if base != base_default or not lo <= size <= hi or size % 1024:
        raise ProfileError(f"ram: SRAM at 0x{base_default:08X}, {lo // 1024}..{hi // 1024} KB in whole KB")
    mb = _int(ram.get("mailbox", base + size - MAILBOX_SIZE), "ram.mailbox")
    if mb != base + size - MAILBOX_SIZE:
        raise ProfileError(f"ram.mailbox: must be the top {MAILBOX_SIZE} bytes of RAM (0x{base + size - 16:08X})")
    p["ram"] = {"base": base, "size": size, "mailbox": mb}


def _resolve_nuc(raw, p):
    if p["crystal_hz"] not in NUC_PLLCON:
        raise ProfileError(f"crystal_hz: {p['crystal_hz']} Hz isn't supported on NUC1xx yet "
                           f"(known PLL setups: {', '.join(str(k) for k in NUC_PLLCON)})")
    p["pllcon"] = NUC_PLLCON[p["crystal_hz"]]
    fl = raw.get("flash")
    _keys(fl, "flash", FLASH_NUC)
    aprom, page, ldrom, dbase, dsize = (_int(fl.get(k), f"flash.{k}") for k in
                                        ("aprom_size", "page", "ldrom_size", "data_base", "data_size"))
    if page != 512:
        raise ProfileError("flash.page: NUC1xx pages are 512 bytes")
    if aprom % page or not 8 * 1024 <= aprom <= 128 * 1024:
        raise ProfileError("flash.aprom_size: whole pages, 8..128 KB")
    if ldrom != 4096:
        raise ProfileError("flash.ldrom_size: NUC1xx LDROM is 4 KB")
    if dsize % page or dsize < 3 * page or dbase % page or dbase < aprom:
        raise ProfileError("flash.data_base / data_size: page-aligned data flash above APROM, at least 3 pages")
    p["flash"] = {"aprom_size": aprom, "page": page, "ldrom_size": ldrom, "data_base": dbase, "data_size": dsize,
                  "config_pages": dsize // page - 1}    # the last data-flash page holds the device ID
    _resolve_ram(raw, p, 0x20000000, (4 * 1024, 16 * 1024))
    for k in ("usb_pullup", "soft_power", "recovery", "update_led"):
        if raw.get(k) is not None:
            raise ProfileError(f"{k}: not supported on nuvoton-nuc1xx (the board header does it)")
    p["swj_nojtag"] = False
    p["usb_pullup"] = p["soft_power"] = p["recovery"] = p["update_led"] = None


def _reserved(raw, p):
    """-> {pin: [(level, roles, why)]}: the profile's reserved list plus what its board pins imply."""
    out = {}

    def add(pins, level, roles, why):
        for n in pins:
            out.setdefault(n, []).append((level, tuple(roles), why))

    if p["usb_pullup"]:
        add([p["usb_pullup"]["pin"]], "error", ROLES, "switches the USB D+ pull-up (the platform drives it)")
    if p["soft_power"]:
        add([p["soft_power"]["latch"]], "error", ROLES, "is the soft-power latch: driving it low switches the "
                                                        "board off")
        add([p["soft_power"]["button"]], "error", ROLES, "is the power button (held 2 s: off)")
    if p["recovery"]:
        what = p["recovery"]["what"] or "the recovery combo"
        add(p["recovery"]["pins"], "warning", ("out",),
            f"is read at reset for {what}: whatever drives it must not hold it "
            f"{'high' if p['recovery']['active'] else 'low'} at power-up, or the board starts in update mode")
    for i, r in enumerate(raw.get("reserved") or []):
        _keys(r, f"reserved[{i}]", ["pins", "level", "roles", "why"])
        level = r.get("level", "warning")
        if level not in ("error", "warning"):
            raise ProfileError(f"reserved[{i}].level: error or warning")
        roles = r.get("roles", list(ROLES))
        if not isinstance(roles, list) or not set(roles) <= set(ROLES):
            raise ProfileError(f"reserved[{i}].roles: a list of {', '.join(ROLES)}")
        if not r.get("why"):
            raise ProfileError(f"reserved[{i}].why: say why the pins are reserved")
        add(_pins(r.get("pins"), f"reserved[{i}].pins"), level, roles, str(r["why"]))
    return out


def pin_check(p, num, role):
    """-> [(level, message)] for using pin num as role on this board (the builder's platform check)."""
    return [(lvl, f"{pin_name(num)} {why} on {p['name']} (board profile '{p['id']}')")
            for lvl, roles, why in p["reserved"].get(num, []) if role in roles]


def board_id_for(p, board_id=None):
    """The board_id an image for this board carries: the profile's own, or (no fixed id) the device's."""
    if p["board_id"] is not None:
        if board_id is not None and board_id != p["board_id"]:
            raise ProfileError(f"board_id 0x{board_id:04X} doesn't match board '{p['id']}' (0x{p['board_id']:04X})")
        return p["board_id"]
    if board_id is None:
        raise ProfileError(f"board '{p['id']}' has no board_id of its own: pass the device's (BOARD_ID=0x3F..)")
    if board_id not in DIY_IDS:
        raise ProfileError(f"board '{p['id']}' is a DIY board: its board_id must be in 0x3F00..0x3FFF "
                           f"(got 0x{board_id:04X})")
    return board_id


def store_budget(p):
    """The config store the builder's storage budget counts with: (page, pages)."""
    return p["flash"]["page"], p["flash"]["config_pages"]


def image_layout(p, board_id):
    """The .gfw layout of this board (mkimage --layout, gemifi.py, the web updater)."""
    fl, ram = p["flash"], p["ram"]
    if p["platform"] == "stm32f1":
        base, max_size, area = fl["app_base"], fl["image_max"], "the app area"
    else:
        base, max_size, area = 0, fl["aprom_size"], "APROM"
    return {"gemifi_board_layout": 1, "board_id": board_id, "profile": p["id"], "name": p["name"],
            "platform": p["platform"], "hdr_offset": HDR_OFFSET[p["platform"]], "base": base,
            "max_size": max_size, "area": area, "sram_start": ram["base"], "sram_end": ram["base"] + ram["size"],
            "page": fl["page"]}


# ---------------------------------------------------------------------------------------------
# generated files

def _h(v):
    return f"0x{v:08X}u"


def _pp(n):
    return n // 16, n % 16


def gen_header(p, board_id):
    src = os.path.relpath(profile_path(p["platform"], p["id"]), REPO)
    fl, ram = p["flash"], p["ram"]
    L = [f"/* board_layout.h: generated by tools/builder/gemifi_board.py from {src}.",
         " * Do not edit: change the profile. SPDX-License-Identifier: MIT */",
         "#ifndef GEMIFI_BOARD_LAYOUT_H", "#define GEMIFI_BOARD_LAYOUT_H", "",
         f'#define BOARD_PROFILE "{p["id"]}"',
         f"#define BOARD_ID 0x{board_id:04X}",
         f'#define BOARD_NAME "{p["short_name"]}"',
         f"#define BOARD_CRYSTAL_HZ {p['crystal_hz']}u"]

    def d(name, val, note=None):
        L.append(f"#define {name} {val}" + (f"  /* {note} */" if note else ""))

    if p["platform"] == "stm32f1":
        d("BOARD_PLL_MUL", p["pll_mul"], f"HSE{' / 2' if p['pll_hse_div2'] else ''} x {p['pll_mul']} = 72 MHz")
        d("BOARD_PLL_HSE_DIV2", p["pll_hse_div2"])
        L.append("")
        L.append("/* ---- flash ---- */")
        d("BOARD_FLASH_BASE", _h(fl["base"]))
        d("BOARD_FLASH_SIZE", _h(fl["size"]))
        d("BOARD_FLASH_PAGE", f"{fl['page']}u")
        d("BOARD_BOOT_SIZE", _h(fl["boot_size"]), "the gemifi (or vendor) bootloader")
        d("BOARD_APP_BASE", _h(fl["app_base"]))
        d("BOARD_APP_END", _h(fl["app_end"]), "the image ends below this")
        d("BOARD_IMAGE_MAX", f"{fl['image_max']}u")
        d("BOARD_CONFIG_BASE", _h(fl["config_base"]))
        d("BOARD_CONFIG_PAGES", f"{fl['config_pages']}u")
        d("BOARD_CONFIG_MIN", _h(fl["config_min"]), "the config pages lie in [MIN, MAX)")
        d("BOARD_CONFIG_MAX", _h(fl["config_max"]))
        d("BOARD_FLAG_PAGE", _h(fl["flag_page"]), "the update-request flag page")
        L.append("")
        L.append("/* ---- RAM ---- */")
        d("BOARD_RAM_BASE", _h(ram["base"]))
        d("BOARD_RAM_SIZE", _h(ram["size"]))
        d("BOARD_MAILBOX_ADDR", _h(ram["mailbox"]), "the top 16 bytes (protocol/sysex.md 6)")
        L.append("")
        L.append("/* ---- board pins (port: 0 = A; pin = port * 16 + bit) ---- */")
        if p["swj_nojtag"]:
            d("BOARD_SWJ_NOJTAG", 1, "JTAG off, SWD on: frees PB3/PB4/PA15")
        if p["usb_pullup"]:
            d("BOARD_USB_PULLUP_PIN", p["usb_pullup"]["pin"], pin_name(p["usb_pullup"]["pin"]))
            if p["usb_pullup"]["active_low"]:
                d("BOARD_USB_PULLUP_ACTIVE_LOW", 1)
        if p["soft_power"]:
            for k, nm, note in (("button", "BTN", "low = pressed"), ("latch", "LATCH", "high = stay on")):
                port, bit = _pp(p["soft_power"][k])
                d(f"BOARD_POWER_{nm}_PORT", port, pin_name(p["soft_power"][k]))
                d(f"BOARD_POWER_{nm}_PIN", bit, note)
        rc = p["recovery"]
        if rc:
            if rc["what"]:
                L.append(f"/* recovery combo: {rc['what']} */")
            if rc["swj_nojtag"]:
                d("BOARD_RECOVERY_SWJ_NOJTAG", 1)
            if rc["drive"]:
                d("BOARD_RECOVERY_DRIVE_PORT", rc["drive"]["port"])
                d("BOARD_RECOVERY_DRIVE_MASK", f"0x{rc['drive']['mask']:04X}u",
                  ", ".join(map(pin_name, rc["drive"]["pins"])))
                d("BOARD_RECOVERY_DRIVE_LEVEL", rc["drive"]["level"])
            d("BOARD_RECOVERY_PORT", rc["port"])
            d("BOARD_RECOVERY_PULL_MASK", f"0x{rc['pull_mask']:04X}u")
            d("BOARD_RECOVERY_MASK", f"0x{rc['mask']:04X}u", ", ".join(map(pin_name, rc["pins"])))
            d("BOARD_RECOVERY_ACTIVE", rc["active"], "the level of a pressed button")
        else:
            d("BOARD_RECOVERY_NONE", 1, "no reset-time recovery combo")
        if p["update_led"]:
            port, bit = _pp(p["update_led"]["pin"])
            d("BOARD_UPDATE_LED_PORT", port, pin_name(p["update_led"]["pin"]))
            d("BOARD_UPDATE_LED_PIN", bit)
            d("BOARD_UPDATE_LED_ACTIVE_LOW", int(p["update_led"]["active_low"]))
    else:
        d("BOARD_PLLCON", f"0x{p['pllcon']:04X}u", "HCLK = USB = 48 MHz")
        L.append("")
        L.append("/* ---- flash ---- */")
        d("BOARD_APROM_SIZE", _h(fl["aprom_size"]))
        d("BOARD_FLASH_PAGE", f"{fl['page']}u")
        d("BOARD_LDROM_SIZE", _h(fl["ldrom_size"]))
        d("BOARD_DATA_BASE", _h(fl["data_base"]))
        d("BOARD_DATA_SIZE", _h(fl["data_size"]))
        L.append("")
        L.append("/* ---- RAM ---- */")
        d("BOARD_RAM_BASE", _h(ram["base"]))
        d("BOARD_RAM_SIZE", _h(ram["size"]))
        d("BOARD_MAILBOX_ADDR", _h(ram["mailbox"]), "the top 16 bytes (protocol/sysex.md 6)")
    L += ["", "#endif", ""]
    return "\n".join(L)


def _ld_head(p, what):
    src = os.path.relpath(profile_path(p["platform"], p["id"]), REPO)
    return [f"/* {what}: generated by tools/builder/gemifi_board.py from {src}. Do not edit. */"]


def gen_ld(p, kind):
    """kind 'app' or 'boot': the MEMORY regions and the layout symbols the linker script asserts on."""
    fl, ram = p["flash"], p["ram"]
    L = _ld_head(p, f"board_{kind}.ld")
    if p["platform"] == "stm32f1":
        org, length = (fl["base"], fl["boot_size"]) if kind == "boot" else (fl["app_base"], fl["app_end"] - fl["app_base"])
        ram_len = ram["size"]
        syms = {"BOARD_FLASH_BASE": fl["base"], "BOARD_APP_BASE": fl["app_base"], "BOARD_APP_END": fl["app_end"],
                "BOARD_MAILBOX": ram["mailbox"], "BOARD_STACK_GUARD": STACK_GUARD["stm32f1"]}
    else:
        org, length = 0, fl["ldrom_size"] if kind == "boot" else fl["aprom_size"]
        ram_len = ram["mailbox"] - ram["base"]      # RAM ends at the mailbox
        syms = {"BOARD_MAILBOX": ram["mailbox"]}
    L += [f"{k} = 0x{v:08X};" for k, v in syms.items()]
    L += ["MEMORY", "{",
          f"    FLASH (rx)  : ORIGIN = 0x{org:08X}, LENGTH = 0x{length:X}",
          f"    RAM   (rwx) : ORIGIN = 0x{ram['base']:08X}, LENGTH = 0x{ram_len:X}",
          "}", ""]
    return "\n".join(L)


def gen_mk(p, board_id, out_dir):
    lay = image_layout(p, board_id)
    return "\n".join([
        "# board.mk: generated by tools/builder/gemifi_board.py. Do not edit.",
        f"BOARD_PROFILE_ID := {p['id']}",
        f"BOARD_ID_RESOLVED := 0x{board_id:04X}",
        f"BOARD_HEADER_FILE := {p['header']}",
        f"BOARD_LAYOUT_JSON := {os.path.join(os.path.abspath(out_dir), 'layout.json')}",
        f"BOARD_MKIMAGE_FLAGS := --hdr-offset 0x{lay['hdr_offset']:X} --base 0x{lay['base']:X} "
        f"--max-size {lay['max_size']} --sram-end 0x{lay['sram_end']:X}",
        ""])


def _write(path, text):
    """Write only when it changes, so make doesn't rebuild for nothing."""
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return False
    except OSError:
        pass
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)
    return True


def generate(platform, board, out_dir, board_id=None, path=None):
    """Write board_layout.h, board_app.ld, board_boot.ld, board.mk and layout.json into out_dir."""
    p = load(platform, board, path)
    bid = board_id_for(p, board_id)
    os.makedirs(out_dir, exist_ok=True)
    files = {"board_layout.h": gen_header(p, bid), "board_app.ld": gen_ld(p, "app"),
             "board_boot.ld": gen_ld(p, "boot"), "board.mk": gen_mk(p, bid, out_dir),
             "layout.json": json.dumps(image_layout(p, bid), indent=2) + "\n"}
    for name, text in files.items():
        _write(os.path.join(out_dir, name), text)
    return p


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen", help="write the generated layout files")
    g.add_argument("platform")
    g.add_argument("board")
    g.add_argument("-o", "--out", required=True)
    g.add_argument("--board-id", type=lambda s: int(s, 0), help="the device's board_id (needed by DIY boards)")
    li = sub.add_parser("list", help="the board profiles")
    li.add_argument("platform", nargs="?")
    sh = sub.add_parser("show", help="a profile, resolved")
    sh.add_argument("platform")
    sh.add_argument("board")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "gen":
            generate(a.platform, a.board, a.out, a.board_id)
        elif a.cmd == "list":
            for plat in [a.platform] if a.platform else PLATFORMS:
                for b in boards(plat):
                    p = load(plat, b)
                    bid = f"0x{p['board_id']:04X}" if p["board_id"] is not None else "DIY"
                    print(f"{plat:16} {b:28} {bid:6} {p['name']}{' (default)' if b == DEFAULT_BOARD[plat] else ''}")
        else:
            p = load(a.platform, a.board)
            p = dict(p, reserved={pin_name(k): v for k, v in p["reserved"].items()})
            print(json.dumps(p, indent=2, default=str))
    except (ProfileError, OSError) as e:
        print(f"gemifi_board: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

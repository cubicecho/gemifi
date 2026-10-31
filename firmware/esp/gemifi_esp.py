# gemifi_esp.py: PlatformIO extra script shared by the firmware/esp projects.
#
# - adds core/include to the include path;
# - custom_gm_core = gsx: compiles core/src/gsx.c only (hello);
#   custom_gm_core = all: compiles core/src/*.c plus the device config in
#   $GM_DEVICE_DIR (gm_device_config.h/.c; absolute, or relative to the repo
#   root), defaulting per env to devices/generated-example-esp8266 or
#   devices/generated-example-esp32;
# - after the build, writes $BUILD_DIR/webflash/: the flash parts and an
#   ESP Web Tools manifest.json (first install from the browser).
#
# SPDX-License-Identifier: MIT
import json
import os
import re
import shutil

from SCons import Builder
from platformio.builder.tools.piobuild import scons_patched_match_splitext

Import("env")  # noqa: F821

ROOT = os.path.abspath(os.path.join(env.subst("$PROJECT_DIR"), "..", "..", ".."))
CORE = os.path.join(ROOT, "core")
pioenv = env.subst("$PIOENV")
mcu = env.BoardConfig().get("build.mcu", "esp8266").lower()

env.Append(CPPPATH=[os.path.join(CORE, "include")])

mode = env.GetProjectOption("custom_gm_core", "gsx")
device_dir = None
if mode == "all":
    device_dir = os.environ.get("GM_DEVICE_DIR")
    if not device_dir:
        if mcu == "esp32":
            print("gemifi: env %s needs GM_DEVICE_DIR (the bundled example targets "
                  "the ESP32-S3)" % pioenv)
            env.Exit(1)
        name = "generated-example-esp8266" if mcu == "esp8266" else "generated-example-esp32"
        device_dir = os.path.join(ROOT, "devices", name)
    if not os.path.isabs(device_dir):
        device_dir = os.path.join(ROOT, device_dir)
    device_dir = os.path.abspath(device_dir)
    for f in ("gm_device_config.h", "gm_device_config.c"):
        if not os.path.isfile(os.path.join(device_dir, f)):
            print("gemifi: %s not found in %s" % (f, device_dir))
            env.Exit(1)
    print("gemifi: device config %s" % device_dir)
    env.Append(CPPPATH=[device_dir])

# Object names must be <file>.c.o, as PlatformIO names them for Arduino
# projects (the ESP8266 linker script puts *.c.o code in flash, anything
# else in IRAM). PlatformIO only patches SCons for that once it loads the
# framework, after pre: scripts, so do it here first.
Builder.match_splitext = scons_patched_match_splitext
if device_dir:
    env.BuildSources("$BUILD_DIR/gm_core", os.path.join(CORE, "src"), "+<*.c>")
    env.BuildSources("$BUILD_DIR/gm_device", device_dir, "+<*.c>")
else:
    env.BuildSources("$BUILD_DIR/gm_core", os.path.join(CORE, "src"), "+<gsx.c>")


def device_name():
    if device_dir:
        h = open(os.path.join(device_dir, "gm_device_config.h")).read()
        m = re.search(r'#define\s+GM_DEVICE_NAME\s+"([^"]*)"', h)
        if m:
            return m.group(1)
    return "gemifi " + os.path.basename(env.subst("$PROJECT_DIR"))


FAMILY = {"esp32": "ESP32", "esp32s2": "ESP32-S2", "esp32s3": "ESP32-S3",
          "esp8266": "ESP8266"}


def webflash(source, target, env):
    build = env.subst("$BUILD_DIR")
    out = os.path.join(build, "webflash")
    os.makedirs(out, exist_ok=True)
    parts = []
    if mcu != "esp8266":
        for off, path in env.get("FLASH_EXTRA_IMAGES", []):
            path = env.subst(path)
            if os.path.isfile(path):
                shutil.copy(path, out)
                parts.append({"path": os.path.basename(path), "offset": int(env.subst(off), 0)})
        app_off = int(env.subst(env.get("ESP32_APP_OFFSET", "0x10000")), 0)
    else:
        app_off = 0
    shutil.copy(os.path.join(build, "firmware.bin"), out)
    parts.append({"path": "firmware.bin", "offset": app_off})
    manifest = {
        "name": device_name(),
        "version": env.GetProjectOption("custom_gm_version", "dev"),
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": FAMILY.get(mcu, mcu.upper()), "parts": parts}],
    }
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("gemifi: wrote %s" % os.path.join(out, "manifest.json"))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", webflash)

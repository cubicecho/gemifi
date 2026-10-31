# gemifi_esp.py: PlatformIO extra script shared by the firmware/esp projects.
#
# - adds core/include to the include path;
# - custom_gm_core = gsx: compiles core/src/gsx.c only (hello);
#   custom_gm_core = all: compiles core/src/*.c plus the device config:
#   $GM_DEVICE_DIR (an already generated gm_device_config.h/.c, as
#   tools/builder passes it), else generated here from the device YAML
#   $GM_DEVICE (default per env: devices/esp8266-example.yaml or
#   devices/esp32-example.yaml) into $BUILD_DIR/gm_device_config; paths are
#   absolute or relative to the repo root;
# - after the build, writes $BUILD_DIR/webflash/: the flash parts and an
#   ESP Web Tools manifest.json (first install from the browser).
#
# SPDX-License-Identifier: MIT
import json
import os
import re
import shutil
import subprocess
import sys

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
    if device_dir:
        if not os.path.isabs(device_dir):
            device_dir = os.path.join(ROOT, device_dir)
    else:
        yaml = os.environ.get("GM_DEVICE")
        if not yaml:
            if mcu == "esp32":
                print("gemifi: env %s needs GM_DEVICE=<device.yaml> (the bundled example "
                      "targets the ESP32-S3)" % pioenv)
                env.Exit(1)
            yaml = os.path.join("devices", "esp8266-example.yaml" if mcu == "esp8266"
                                else "esp32-example.yaml")
        if not os.path.isabs(yaml):
            yaml = os.path.join(ROOT, yaml)
        device_dir = os.path.join(env.subst("$BUILD_DIR"), "gm_device_config")
        # Only changed files are rewritten, so an unchanged YAML rebuilds nothing.
        # The system python3, not PlatformIO's: the builder needs PyYAML.
        py = shutil.which("python3") or sys.executable
        r = subprocess.run([py, os.path.join(ROOT, "tools", "builder", "gemifi_build.py"),
                            "generate", yaml, "-o", device_dir],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        if r.returncode:
            print(r.stdout, end="")
            print("gemifi: couldn't generate the device config from %s" % yaml)
            env.Exit(1)
        print("gemifi: device config generated from %s" % os.path.relpath(yaml, ROOT))
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


def config_str(macro):
    """A string #define from the device config, or None."""
    if device_dir:
        h = open(os.path.join(device_dir, "gm_device_config.h")).read()
        m = re.search(r'#define\s+%s\s+"([^"]*)"' % macro, h)
        if m:
            return m.group(1)
    return None


def device_name():
    return config_str("GM_DEVICE_NAME") or "gemifi " + os.path.basename(env.subst("$PROJECT_DIR"))


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
        "version": config_str("GM_FW_VERSION_STR") or env.GetProjectOption("custom_gm_version", "dev"),
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": FAMILY.get(mcu, mcu.upper()), "parts": parts}],
    }
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("gemifi: wrote %s" % os.path.join(out, "manifest.json"))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", webflash)

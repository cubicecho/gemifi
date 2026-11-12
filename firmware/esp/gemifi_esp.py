# gemifi_esp.py: PlatformIO pre: script shared by the firmware/esp projects (framework = espidf).
#
# It runs before PlatformIO's ESP-IDF builder, and sets up what that builder's CMake run reads:
# - custom_gm_core = gsx (hello): GM_CORE=gsx, the gemifi_esp component compiles core/src/gsx.c only;
#   custom_gm_core = all (midi-controller): GM_CORE=all and GM_DEVICE_DIR, the component compiles
#   core/src/*.c and the device config: $GM_DEVICE_DIR (an already generated gm_device_config.h/.c,
#   as tools/builder passes it), else generated here from the device YAML $GM_DEVICE (default:
#   devices/esp32-example.yaml on the S2 / S3; the ESP32 has none) into $BUILD_DIR/gm_device_config.
#   Paths are absolute or relative to the repo root;
# - the sdkconfig: firmware/esp/sdkconfig.defaults, then sdkconfig.<transport> (the device config's
#   GM_MIDI_TRANSPORT: serial, usb or ble; without a device config the chip's default), and
#   ESP-IDF adds <file>.<target> for each. It is kept in $BUILD_DIR/sdkconfig.<transport>, not in
#   the project;
# - after the build, writes $BUILD_DIR/webflash/: the flash parts and an ESP Web Tools manifest.json
#   (first install from the browser).
#
# SPDX-License-Identifier: MIT
import json
import os
import re
import shutil
import subprocess
import sys

Import("env")  # noqa: F821

ESP = os.path.abspath(os.path.join(env.subst("$PROJECT_DIR"), ".."))
ROOT = os.path.abspath(os.path.join(ESP, "..", ".."))
pioenv = env.subst("$PIOENV")
board = env.BoardConfig()
mcu = board.get("build.mcu", "esp32").lower()
NATIVE_USB = mcu in ("esp32s2", "esp32s3")

mode = env.GetProjectOption("custom_gm_core", "gsx")
os.environ["GM_CORE"] = mode
device_dir = None
if mode == "all":
    device_dir = os.environ.get("GM_DEVICE_DIR")
    if device_dir:
        if not os.path.isabs(device_dir):
            device_dir = os.path.join(ROOT, device_dir)
    else:
        yaml = os.environ.get("GM_DEVICE")
        if not yaml:
            if not NATIVE_USB:
                print("gemifi: env %s needs GM_DEVICE=<device.yaml> (the bundled example "
                      "targets the ESP32-S3)" % pioenv)
                env.Exit(1)
            yaml = os.path.join("devices", "esp32-example.yaml")
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
    os.environ["GM_DEVICE_DIR"] = device_dir


def config_h():
    return open(os.path.join(device_dir, "gm_device_config.h")).read() if device_dir else ""


def config_str(macro):
    """A string #define from the device config, or None."""
    m = re.search(r'#define\s+%s\s+"([^"]*)"' % macro, config_h())
    return m.group(1) if m else None


# The transport picks the sdkconfig fragment (gm_hal.h GM_MIDI_USB / _SERIAL / _BLE).
m = re.search(r"#define\s+GM_MIDI_TRANSPORT\s+GM_MIDI_(\w+)", config_h())
transport = m.group(1).lower() if m else ("usb" if NATIVE_USB else "serial")
if transport not in ("usb", "serial", "ble"):
    print("gemifi: unknown GM_MIDI_TRANSPORT %s in the device config" % transport)
    env.Exit(1)
if transport == "usb" and not NATIVE_USB:
    print("gemifi: midi.transport usb needs an ESP32-S2 or -S3 (%s has no USB; use serial or ble)" % mcu)
    env.Exit(1)
if transport == "ble" and mcu == "esp32s2":
    print("gemifi: midi.transport ble needs an ESP32 or ESP32-S3 (the S2 has no Bluetooth)")
    env.Exit(1)
defaults = [os.path.join(ESP, "sdkconfig.defaults"), os.path.join(ESP, "sdkconfig." + transport)]
extra = board.get("build.cmake_extra_args", "")
board.update("build.cmake_extra_args",
             (extra + " " if extra else "") + '-DSDKCONFIG_DEFAULTS="%s"' % ";".join(defaults))
# One sdkconfig per transport in the build dir: ESP-IDF only applies defaults to options the file
# doesn't have yet, so another transport in the same build dir must not reuse it.
board.update("build.esp-idf.sdkconfig_path",
             os.path.join(env.subst("$BUILD_DIR"), "sdkconfig." + transport))
print("gemifi: midi.transport %s (sdkconfig.%s)" % (transport, transport))


def device_name():
    return config_str("GM_DEVICE_NAME") or "gemifi " + os.path.basename(env.subst("$PROJECT_DIR"))


FAMILY = {"esp32": "ESP32", "esp32s2": "ESP32-S2", "esp32s3": "ESP32-S3"}


def webflash(source, target, env):
    build = env.subst("$BUILD_DIR")
    out = os.path.join(build, "webflash")
    os.makedirs(out, exist_ok=True)
    parts = []
    for off, path in env.get("FLASH_EXTRA_IMAGES", []):
        path = env.subst(path)
        if os.path.isfile(path):
            shutil.copy(path, out)
            parts.append({"path": os.path.basename(path), "offset": int(env.subst(off), 0)})
    app_off = int(env.subst(env.get("ESP32_APP_OFFSET", "0x10000")), 0)
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

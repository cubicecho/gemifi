#!/usr/bin/env python3
"""gemifi builder: device YAML -> generated C tables -> firmware image (ESPHome style).

  gemifi_build.py validate <device.yaml> [<device.yaml> ...]
  gemifi_build.py generate <device.yaml> -o <dir>
  gemifi_build.py build <device.yaml> [-o <out dir>]     (default build/devices/<name>/)
  gemifi_build.py presets
  gemifi_build.py new <name> --preset <preset> [--dir ~/gemifi-devices]
  gemifi_build.py dashboard [--host H] [--port 6055] [--web DIR] [--config-dir DIR] [--build-dir DIR]
                            [--allowed-host H ...] [--tls-cert F --tls-key F]
                            (each also a GEMIFI_* env var; GEMIFI_PASSWORD turns on Basic auth)

The YAML format is core/README.md "Device YAML". Presets are the YAML files in devices/.
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gemifi_device as gd  # noqa: E402

PRESET_DIR = os.path.join(REPO, "devices")
USER_DIR = os.path.expanduser("~/gemifi-devices")
NUC_PROJECT = os.path.join(REPO, "firmware", "nuvoton-nuc1xx", "midi-controller")
# Bare-metal platforms built with make + arm-none-eabi-gcc: platform name -> project dir.
MAKE_PROJECTS = {
    "nuvoton-nuc1xx": NUC_PROJECT,
    "stm32f1": os.path.join(REPO, "firmware", "stm32f1", "midi-controller"),
}
ESP_PROJECT = os.path.join(REPO, "firmware", "esp", "midi-controller")


def rel(p):
    """Path relative to the repo when inside it (for messages and generated headers)."""
    ap = os.path.abspath(p)
    return os.path.relpath(ap, REPO) if ap.startswith(REPO + os.sep) else ap


def device_stem(path):
    return os.path.splitext(os.path.basename(path))[0]


def need_yaml():
    if gd.yaml is None:
        print(f"gemifi_build: {gd.YAML_HINT}", file=sys.stderr)
        sys.exit(2)


def check(path, quiet=False, out=None):
    """Validate; print diagnostics (errors to stderr unless out is given). Returns Device or None."""
    dev, diags = gd.load_device(path)
    info = out or sys.stdout
    bad = out or sys.stderr
    for d in diags:
        d.file = rel(path)
        print(d, file=info if d.level == "warning" else bad)
    if dev is None:
        n = len(gd.errors(diags))
        print(f"{rel(path)}: {n} error{'s' if n != 1 else ''}", file=bad)
    elif not quiet:
        print(f"{rel(path)}: OK: {summary(dev)}", file=info)
    return dev


def summary(dev):
    parts = [f"{dev.name} ({dev.platform.name}, board 0x{dev.board_id:04X})"]
    for n, lst in (("hub", dev.hubs), ("button", dev.buttons), ("LED", dev.leds),
                   ("analog", dev.analogs), ("encoder", dev.encoders), ("pad", dev.pads)):
        if lst:
            parts.append(f"{len(lst)} {n}{'s' if len(lst) != 1 and n != 'analog' else ''}")
    if dev.matrix:
        parts.append(f"{dev.matrix.keys}-key {dev.matrix.kind} matrix")
    if dev.presets_given and dev.budget:
        parts.append("presets: " + gd.budget_text(dev.budget))
    dma = "".join(f"\n  {t}" for t in (gd.dma_text(dev), gd.timers_text(dev)) if t)
    return ", ".join(parts) + dma


# ---------------------------------------------------------------------------------------------
# platform builds

def find_pio():
    p = shutil.which("pio") or shutil.which("platformio")
    if p:
        return p
    p = os.path.expanduser("~/.venvs/pio/bin/pio")
    return p if os.path.exists(p) else None


def run(cmd, env=None, cwd=None, out=None):
    out = out or sys.stdout
    print("$ " + " ".join(cmd), file=out, flush=True)
    proc = subprocess.Popen(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1)
    for line in proc.stdout:
        out.write(line)
        out.flush()
    return proc.wait()


def image_stem(dev):
    """Built images are named after the node name and version, as ESPHome names its builds:
    keystation-mini32-0.1.5.gfw (no project.version: keystation-mini32.gfw)."""
    return dev.node_name + ("-" + ".".join(map(str, dev.version)) if dev.version else "")


IMAGE_FILE_RE = re.compile(r".+\.(gfw|bin)")


def platform_build(dev, config_dir, out_dir, out=None):
    """Run the platform's build. Returns the list of image files copied into out_dir, or None."""
    out = out or sys.stdout
    stem = image_stem(dev)
    if dev.platform.name in MAKE_PROJECTS:
        project = MAKE_PROJECTS[dev.platform.name]
        if not os.path.exists(os.path.join(project, "Makefile")):
            print(f"error: {rel(project)}/Makefile doesn't exist yet: can't build the firmware "
                  f"(the generated config is in {rel(config_dir)})", file=out)
            return None
        bdir = os.path.join(out_dir, "fw")
        rc = run(["make", "-C", project, f"DEVICE_DIR={config_dir}", f"BUILD={bdir}"], out=out)
        if rc:
            print(f"error: make failed ({rc})", file=out)
            return None
        imgs = [os.path.join(bdir, "midi-controller.gfw")]
        names = [f"{stem}.gfw"]
        extra = os.path.join(bdir, "midi-controller.bin")
        if os.path.exists(extra):
            imgs.append(extra)
            names.append(f"{stem}.bin")
    else:
        if not os.path.exists(os.path.join(ESP_PROJECT, "platformio.ini")):
            print(f"error: {rel(ESP_PROJECT)}/platformio.ini doesn't exist yet: can't build the firmware "
                  f"(the generated config is in {rel(config_dir)})", file=out)
            return None
        pio = find_pio()
        if not pio:
            print("error: PlatformIO not found (put pio on PATH, or `python3 -m venv ~/.venvs/pio && "
                  "~/.venvs/pio/bin/pip install platformio`)", file=out)
            return None
        penv = dev.platform.name
        # pio run has no --project-option: the board goes in an extra ini that
        # firmware/esp/midi-controller/platformio.ini includes (extra_configs).
        extra_ini = os.path.join(out_dir, "board.ini")
        with open(extra_ini, "w") as f:
            f.write(f"[env:{penv}]\nboard = {dev.chip}\n")
        env = dict(os.environ, GM_DEVICE_DIR=config_dir, GM_PIO_EXTRA=extra_ini,
                   PLATFORMIO_BUILD_DIR=os.path.join(out_dir, "pio"))   # per-device build dir
        rc = run([pio, "run", "-d", ESP_PROJECT, "-e", penv], env=env, out=out)
        if rc:
            print(f"error: pio run failed ({rc})", file=out)
            return None
        bdir = os.path.join(out_dir, "pio", penv)
        imgs, names = [], []
        for f in sorted(glob.glob(os.path.join(bdir, "*.gfw")) + glob.glob(os.path.join(bdir, "*.bin"))):
            base = os.path.basename(f)
            imgs.append(f)
            # firmware.bin -> <stem>.bin (the OTA image), firmware.factory.bin ->
            # <stem>-factory.bin (merged, flash at 0x0), others <stem>-<file>
            if base == "firmware.bin":
                names.append(f"{stem}.bin")
            elif base.startswith("firmware."):
                names.append(f"{stem}-{base[len('firmware.'):]}")
            else:
                names.append(f"{stem}-{base}")
        if not imgs:
            print(f"error: no image found in {rel(bdir)}", file=out)
            return None
        web = os.path.join(bdir, "webflash")      # ESP Web Tools manifest + parts
        if os.path.isdir(web):
            dst = os.path.join(out_dir, "webflash")
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(web, dst)
            print(f"web install files: {rel(dst)}/manifest.json", file=out)
    # The last build's images go, so an old version or node name doesn't linger next to the new one.
    for f in os.listdir(out_dir):
        if IMAGE_FILE_RE.fullmatch(f) and os.path.isfile(os.path.join(out_dir, f)):
            os.remove(os.path.join(out_dir, f))
    copied = []
    for src, name in zip(imgs, names):
        if not os.path.exists(src):
            print(f"error: expected {rel(src)} after the build", file=out)
            return None
        dst = os.path.join(out_dir, name)
        shutil.copyfile(src, dst)
        copied.append(dst)
    return copied


SOURCE_HASH_FILE = "source.sha256"


def source_hash(path):
    """SHA-256 of a device file, or None if it can't be read."""
    try:
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return None


def built_hash(out_dir):
    """The source hash recorded by the last successful build in out_dir, or None."""
    try:
        with open(os.path.join(out_dir, SOURCE_HASH_FILE)) as f:
            return f.read().strip() or None
    except OSError:
        return None


def do_build(path, out_dir=None, out=None):
    """validate + generate + platform build. Returns 0 on success.

    On success the YAML's hash goes in <out_dir>/source.sha256, so the dashboard can tell
    when the images no longer match the file (hashed before the build, so an edit made
    during the build still counts as a change)."""
    out = out or sys.stdout
    digest = source_hash(path)
    dev = check(path, out=out)
    if dev is None:
        return 1
    out_dir = os.path.abspath(out_dir or os.path.join(REPO, "build", "devices", device_stem(path)))
    config_dir = os.path.join(out_dir, "config")
    for f in gd.write_generated(dev, config_dir, rel(path)):
        print(f"generated {rel(f)}", file=out)
    imgs = platform_build(dev, config_dir, out_dir, out=out)
    if imgs is None:
        print("BUILD FAILED", file=out)
        return 1
    if digest is not None:
        with open(os.path.join(out_dir, SOURCE_HASH_FILE), "w") as f:
            f.write(digest + "\n")
    print("BUILD OK:", file=out)
    for f in imgs:
        print(f"  {rel(f)} ({os.path.getsize(f)} bytes)", file=out)
    if dev.platform.name == "nuvoton-nuc1xx":
        print("Flash the .gfw with the web updater (web/updater, `npm run dev`) or "
              "tools/host/gemifi.py flash.", file=out)
    elif dev.platform.name == "stm32f1":
        print("Flash the .gfw with the web updater or tools/host/gemifi.py flash, through the gemifi "
              "bootloader (bootloader/stm32f1, put on once over SWD).", file=out)
    return 0


# ---------------------------------------------------------------------------------------------
# presets

def list_presets():
    """[(name, path, platform, description)] for devices/*.yaml."""
    out = []
    for p in sorted(glob.glob(os.path.join(PRESET_DIR, "*.yaml")) + glob.glob(os.path.join(PRESET_DIR, "*.yml"))):
        out.append((device_stem(p), p) + peek(p))
    return out


def peek(path):
    """(platform, description, display name) from a YAML without full validation. The display name is
    friendly_name, else name (the node name)."""
    return peek_info(path)[:3]


def peek_info(path):
    """peek(), plus the node name ("" when name: isn't a valid one): what a device reports in HELLO."""
    try:
        with open(path, encoding="utf-8") as f:
            data = gd.load_yaml(f.read())
        if isinstance(data, dict):
            node = data.get("name")
            return (str(data.get("platform", "?")), str(data.get("description", "") or ""),
                    str(data.get("friendly_name") or node or ""),
                    node if isinstance(node, str) and gd.NODE_NAME_RE.fullmatch(node) else "")
    except (OSError, UnicodeDecodeError, gd.YamlLoadError):
        pass
    return ("?", "(unreadable)", "", "")


def new_device(name, preset, directory, force=False):
    """Copy preset to <directory>/<name>.yaml. Returns the path; raises ValueError."""
    name = re.sub(r"\.ya?ml$", "", name)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name):
        raise ValueError(f"'{name}' isn't a valid device name (letters, digits, . _ -)")
    presets = {p[0]: p[1] for p in list_presets()}
    if preset not in presets:
        raise ValueError(f"unknown preset '{preset}' (presets: {', '.join(presets) or 'none'})")
    os.makedirs(directory, exist_ok=True)
    dst = os.path.join(directory, name + ".yaml")
    if os.path.exists(dst) and not force:
        raise ValueError(f"{dst} already exists (use --force to overwrite)")
    with open(presets[preset], encoding="utf-8") as f:
        text = f.read()
    # the node name follows the file name (as ESPHome's does), when it can be one
    node = name.lower()
    if gd.NODE_NAME_RE.fullmatch(node):
        text = re.sub(r"^name:[^#\n]*?(\s*#.*)?$", lambda m: f"name: {node}{m.group(1) or ''}", text,
                      count=1, flags=re.M)
    with open(dst, "w", encoding="utf-8") as f:
        f.write(f"# Created from preset {preset} (devices/{os.path.basename(presets[preset])}).\n" + text)
    return dst


# ---------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("validate", help="check device YAMLs")
    s.add_argument("device", nargs="+")
    s = sub.add_parser("generate", help="write gm_device_config.h/.c")
    s.add_argument("device")
    s.add_argument("-o", "--out", required=True, help="output directory")
    s = sub.add_parser("build", help="validate, generate and build the firmware")
    s.add_argument("device")
    s.add_argument("-o", "--out", help="output directory (default build/devices/<name>/)")
    sub.add_parser("presets", help="list the presets in devices/")
    s = sub.add_parser("new", help="start a device YAML from a preset")
    s.add_argument("name")
    s.add_argument("--preset", required=True)
    s.add_argument("--dir", default=USER_DIR, help=f"where to put it (default {USER_DIR})")
    s.add_argument("--force", action="store_true", help="overwrite an existing file")
    env = os.environ.get
    s = sub.add_parser("dashboard", help="web dashboard (default 127.0.0.1:6055)",
                       description="Web dashboard. Every flag has a GEMIFI_* env var; GEMIFI_PASSWORD "
                       "(and GEMIFI_USERNAME, default gemifi) turns on HTTP Basic auth, "
                       "GEMIFI_READONLY_PRESETS=1 makes devices/ read-only.")
    s.add_argument("--host", default=env("GEMIFI_HOST", "127.0.0.1"),
                   help="bind address (GEMIFI_HOST, default 127.0.0.1)")
    s.add_argument("--port", type=int, default=int(env("GEMIFI_PORT", "6055")),
                   help="port (GEMIFI_PORT, default 6055)")
    s.add_argument("--web", default=env("GEMIFI_WEB"),
                   help="static web app to serve at / (GEMIFI_WEB, default web/updater/dist if built)")
    s.add_argument("--config-dir", "--user-dir", dest="config_dir", default=env("GEMIFI_CONFIG_DIR", USER_DIR),
                   help=f"your device YAMLs (GEMIFI_CONFIG_DIR, default {USER_DIR})")
    s.add_argument("--build-dir", default=env("GEMIFI_BUILD_DIR"),
                   help="build outputs, <dir>/<presets|mine>/<name>/ (GEMIFI_BUILD_DIR, default build/devices)")
    s.add_argument("--allowed-host", action="append", dest="allowed_hosts",
                   help="accepted Host header, name or name:port; repeatable "
                   "(GEMIFI_ALLOWED_HOSTS, comma-separated)")
    s.add_argument("--tls-cert", default=env("GEMIFI_TLS_CERT"), help="PEM certificate: serve https (GEMIFI_TLS_CERT)")
    s.add_argument("--tls-key", default=env("GEMIFI_TLS_KEY"), help="PEM private key (GEMIFI_TLS_KEY)")
    a = ap.parse_args(argv)
    need_yaml()

    if a.cmd == "validate":
        bad = [p for p in a.device if check(p) is None]
        return 1 if bad else 0
    if a.cmd == "generate":
        dev = check(a.device)
        if dev is None:
            return 1
        for f in gd.write_generated(dev, a.out, rel(a.device)):
            print(f"wrote {f}")
        return 0
    if a.cmd == "build":
        return do_build(a.device, a.out)
    if a.cmd == "presets":
        rows = list_presets()
        if not rows:
            print("no presets in devices/")
        w = max([len(r[0]) for r in rows] + [6])
        for name, _p, plat, desc, _n in rows:
            print(f"{name:<{w}}  {plat:<15} {desc}")
        return 0
    if a.cmd == "new":
        try:
            p = new_device(a.name, a.preset, os.path.expanduser(a.dir), a.force)
        except (ValueError, OSError) as e:
            print(f"gemifi_build: {e}", file=sys.stderr)
            return 1
        print(f"created {p}\nedit it, then: {sys.argv[0]} build {p}")
        return 0
    if a.cmd == "dashboard":
        import dashboard
        hosts = a.allowed_hosts or [h for h in env("GEMIFI_ALLOWED_HOSTS", "").split(",") if h.strip()]
        auth = (env("GEMIFI_USERNAME") or "gemifi", env("GEMIFI_PASSWORD")) if env("GEMIFI_PASSWORD") else None
        return dashboard.serve(a.port, os.path.expanduser(a.config_dir), host=a.host, web_dir=a.web,
                               build_dir=a.build_dir and os.path.expanduser(a.build_dir),
                               allowed_hosts=[h.strip() for h in hosts], tls_cert=a.tls_cert,
                               tls_key=a.tls_key, auth=auth)
    return 2


if __name__ == "__main__":
    sys.exit(main())

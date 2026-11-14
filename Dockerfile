# gemifi builder: the dashboard (define a device, edit its YAML, build the firmware) plus the
# web updater (flash it from the browser), in one container. tools/builder/README.md "Docker".
#
#   docker build -t gemifi .
#   docker run --rm -p 6055:6055 -v ./config:/config -v gemifi-cache:/cache gemifi
#
# NUC1xx builds work out of the box. ESP toolchains are not in the image: PlatformIO downloads
# them (~6 GB) into /cache/platformio on the first ESP build.

# ---- stage 1: the web updater (static files) ----
# Runs on the build machine's own platform: its output is plain files, so a multi-arch build
# doesn't run npm under emulation.
FROM --platform=$BUILDPLATFORM node:26-slim AS web
WORKDIR /src/web/updater
COPY web/updater/package.json web/updater/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY web/updater/ ./
RUN npm run build

# ---- stage 2: builder + dashboard ----
FROM python:3.14-slim-trixie

# gcc-arm-none-eabi without its recommended newlib (~2.5 GB): the NUC firmware is -nostdlib
# and only needs the compiler's own headers and libgcc. git is for PlatformIO packages.
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc-arm-none-eabi make openssl git tini \
 && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir platformio==6.2.0 pyyaml==6.0.3 pyserial==3.5

WORKDIR /opt/gemifi
# Only what the builds use: core + components + platform + firmware sources, the presets, the builder and
# tools/host (mkimage.py for the .gfw header; gemifi.py for flashing from a shell).
COPY core/ core/
COPY platform/ platform/
COPY firmware/ firmware/
COPY devices/ devices/
COPY protocol/ protocol/
COPY components/ components/
COPY tools/builder/ tools/builder/
COPY tools/host/ tools/host/
COPY README.md LICENSE ./
COPY --from=web /src/web/updater/dist/ web/updater/dist/
COPY docker/entrypoint.sh /usr/local/bin/gemifi-entrypoint
# `pio run -d` needs a writable project dir, and ESP-IDF writes dependencies.lock and
# managed_components/ there (the build itself goes in the device's build dir). Writable for any
# `user:`; with PUID/PGID the entrypoint also gives them to that user.
RUN chmod a+rwx firmware/esp/midi-controller firmware/esp/hello

ENV GEMIFI_HOST=0.0.0.0 \
    GEMIFI_PORT=6055 \
    GEMIFI_CONFIG_DIR=/config \
    GEMIFI_BUILD_DIR=/config/.build \
    GEMIFI_WEB=/opt/gemifi/web/updater/dist \
    GEMIFI_READONLY_PRESETS=1 \
    PLATFORMIO_CORE_DIR=/cache/platformio \
    PLATFORMIO_WORKSPACE_DIR=/cache/pio-workspace \
    PLATFORMIO_SETTING_ENABLE_TELEMETRY=no \
    PLATFORMIO_SETTING_CHECK_PLATFORMIO_INTERVAL=36500 \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

VOLUME ["/config", "/cache"]
EXPOSE 6055
HEALTHCHECK --interval=60s --timeout=5s --start-period=10s \
  CMD python3 -c "import os,sys,urllib.request as u,ssl,base64;s='https' if os.environ.get('GEMIFI_TLS_CERT') or os.environ.get('GEMIFI_TLS')=='self-signed' else 'http';r=u.Request(s+'://localhost:'+os.environ['GEMIFI_PORT']+'/api/info');p=os.environ.get('GEMIFI_PASSWORD');p and r.add_header('Authorization','Basic '+base64.b64encode((os.environ.get('GEMIFI_USERNAME','gemifi')+':'+p).encode()).decode());u.urlopen(r,timeout=4,context=ssl._create_unverified_context())" || exit 1
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/gemifi-entrypoint"]
CMD []

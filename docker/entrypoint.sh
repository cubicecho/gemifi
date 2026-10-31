#!/bin/sh
# gemifi container entrypoint: optional self-signed TLS, drop to PUID:PGID, start the dashboard.
#
#   GEMIFI_TLS=self-signed   make (once) and use /config/.tls/{cert,key}.pem, unless
#                            GEMIFI_TLS_CERT/GEMIFI_TLS_KEY are given
#   GEMIFI_TLS_HOSTS         comma-separated names/IPs for the certificate (default localhost)
#   PUID / PGID              user/group to run as when started as root (default 1000:1000;
#                            PUID=0 stays root). With `user:` in compose this is skipped.
# Extra arguments go to `gemifi_build.py dashboard`.
set -eu

CONFIG=${GEMIFI_CONFIG_DIR:-/config}
CACHE=/cache
mkdir -p "$CONFIG" 2>/dev/null || true

if [ "${GEMIFI_TLS:-}" = "self-signed" ] && [ -z "${GEMIFI_TLS_CERT:-}" ]; then
    tls=$CONFIG/.tls
    hosts=${GEMIFI_TLS_HOSTS:-localhost}
    mkdir -p "$tls"
    if [ ! -s "$tls/cert.pem" ] || [ ! -s "$tls/key.pem" ] || [ "$(cat "$tls/hosts" 2>/dev/null)" != "$hosts" ]; then
        san="" cn=""
        for h in $(echo "$hosts" | tr ',' ' '); do
            [ -n "$cn" ] || cn=$h
            case $h in
                *[!0-9.]*) case $h in *:*) san="$san,IP:$h" ;; *) san="$san,DNS:$h" ;; esac ;;
                *) san="$san,IP:$h" ;;
            esac
        done
        case "$san," in *,DNS:localhost,*) ;; *) san="$san,DNS:localhost" ;; esac
        case "$san," in *,IP:127.0.0.1,*) ;; *) san="$san,IP:127.0.0.1" ;; esac
        san=${san#,}
        echo "gemifi: making a self-signed certificate for $san in $tls"
        umask 077
        openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 825 \
            -subj "/CN=${cn:-localhost}/O=gemifi self-signed" \
            -addext "subjectAltName=$san" -addext "extendedKeyUsage=serverAuth" \
            -keyout "$tls/key.pem" -out "$tls/cert.pem" 2>/dev/null
        umask 022
        echo "$hosts" > "$tls/hosts"
        chmod 644 "$tls/cert.pem" "$tls/hosts"
    fi
    export GEMIFI_TLS_CERT="$tls/cert.pem" GEMIFI_TLS_KEY="$tls/key.pem"
fi

set -- python3 /opt/gemifi/tools/builder/gemifi_build.py dashboard "$@"

if [ "$(id -u)" = 0 ] && [ "${PUID:-1000}" != 0 ]; then
    uid=${PUID:-1000} gid=${PGID:-1000}
    mkdir -p "$CACHE/platformio" "$CACHE/pio-workspace" "$CACHE/home" "${GEMIFI_BUILD_DIR:-$CONFIG/.build}"
    # Fix ownership only where it's wrong, so a populated 6 GB cache isn't walked every start.
    for d in "$CACHE" "$CACHE/platformio" "$CACHE/pio-workspace" "$CACHE/home" \
             "${GEMIFI_BUILD_DIR:-$CONFIG/.build}" "$CONFIG/.tls"; do
        [ -e "$d" ] || continue
        [ "$(stat -c %u:%g "$d")" = "$uid:$gid" ] || chown -R "$uid:$gid" "$d"
    done
    # The config dir itself: only take it over when docker created it (root-owned).
    [ "$(stat -c %u "$CONFIG")" != 0 ] || chown "$uid:$gid" "$CONFIG"
    export HOME="$CACHE/home"
    exec setpriv --reuid="$uid" --regid="$gid" --clear-groups -- "$@"
fi

export HOME="${HOME:-/tmp}"
[ -w "$HOME" ] || export HOME=/tmp
exec "$@"

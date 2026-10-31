"""gemifi dashboard (ESPHome-dashboard style): list / create / edit / validate / build devices.

Started by `gemifi_build.py dashboard` (flags and GEMIFI_* env vars: README.md "Dashboard").
Standard library only. Files are addressed as "<root>/<file>.yaml" with root "presets" (devices/)
or "mine" (the config dir); anything else is refused. Outputs go to <build dir>/<root>/<stem>/.
With a web dir (web/updater/dist) the web updater is served at / and flashes the built images
from the browser; otherwise a small inline page is served.

Security: the Host header is checked (DNS rebinding), POSTs must be application/json (a
cross-site form can't send that without a CORS preflight), and with GEMIFI_PASSWORD set every
request needs HTTP Basic auth.
"""
import base64
import hmac
import ipaddress
import json
import mimetypes
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import gemifi_build as gb
import gemifi_device as gd

NAME = "gemifi builder"
VERSION = "0.1.0"
NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\.ya?ml")
IMAGE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\.(gfw|bin)")
DEFAULT_BUILD_DIR = os.path.join(gb.REPO, "build", "devices")
DEFAULT_WEB_DIR = os.path.join(gb.REPO, "web", "updater", "dist")
MIME = {".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript", ".css": "text/css",
        ".json": "application/json", ".webmanifest": "application/manifest+json", ".svg": "image/svg+xml",
        ".png": "image/png", ".ico": "image/x-icon", ".wasm": "application/wasm", ".woff2": "font/woff2",
        ".woff": "font/woff", ".txt": "text/plain", ".map": "application/json", ".bin": "application/octet-stream"}
_build_lock = threading.Lock()


def env_flag(name):
    return os.environ.get(name, "").strip().lower() in ("1", "true", "yes", "on")


class Roots:
    """The two YAML roots plus where their build outputs go."""

    def __init__(self, user_dir, build_dir=None, preset_dir=None, readonly_presets=None):
        self.dirs = {"presets": preset_dir or gb.PRESET_DIR, "mine": user_dir}
        self.build_dir = os.path.abspath(build_dir or DEFAULT_BUILD_DIR)
        if readonly_presets is None:
            readonly_presets = env_flag("GEMIFI_READONLY_PRESETS")
        self.readonly_presets = readonly_presets or not os.access(self.dirs["presets"], os.W_OK)

    def resolve(self, ident):
        """'mine/foo.yaml' -> absolute path, or None if it isn't a well-formed id."""
        if not isinstance(ident, str) or ident.count("/") != 1:
            return None
        root, name = ident.split("/")
        base = self.dirs.get(root)
        if base is None or not NAME_RE.fullmatch(name):
            return None
        path = os.path.realpath(os.path.join(base, name))
        if os.path.dirname(path) != os.path.realpath(base):
            return None
        return path

    def writable(self, ident):
        root = ident.split("/")[0]
        if root == "presets" and self.readonly_presets:
            return False
        p = self.resolve(ident)
        return p is not None and (os.access(p, os.W_OK) if os.path.exists(p)
                                  else os.access(os.path.dirname(p), os.W_OK))

    def out_dir(self, ident):
        """Per-id output dir: presets/x.yaml and mine/x.yaml don't share one."""
        root, name = ident.split("/")
        return os.path.join(self.build_dir, root, gb.device_stem(name))

    def images(self, ident):
        d = self.out_dir(ident)
        if not os.path.isdir(d):
            return []
        return sorted(f for f in os.listdir(d) if IMAGE_RE.fullmatch(f) and os.path.isfile(os.path.join(d, f)))

    def is_stale(self, ident):
        """Built, but the YAML changed since: the images don't match the file any more."""
        if not self.images(ident):
            return False
        recorded = gb.built_hash(self.out_dir(ident))
        return recorded is None or recorded != gb.source_hash(self.resolve(ident))

    def listing(self):
        out = []
        for root, base in self.dirs.items():
            if not os.path.isdir(base):
                continue
            for name in sorted(os.listdir(base)):
                if NAME_RE.fullmatch(name) and os.path.isfile(os.path.join(base, name)):
                    plat, desc, dname = gb.peek(os.path.join(base, name))
                    ident = f"{root}/{name}"
                    out.append({"id": ident, "root": root, "file": name, "name": dname, "platform": plat,
                                "description": desc, "images": self.images(ident),
                                "stale": self.is_stale(ident), "writable": self.writable(ident)})
        return out


def toolchains():
    return {"nuvoton-nuc1xx": bool(shutil.which("arm-none-eabi-gcc") and shutil.which("make")),
            "esp": gb.find_pio() is not None}


def is_loopback(host):
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def split_host(h):
    """'Example.com:6055' -> ('example.com', '6055'); '[::1]:80' -> ('::1', '80'); port may be None."""
    h = (h or "").strip().lower()
    if h.startswith("["):
        end = h.find("]")
        if end < 0:
            return h, None
        rest = h[end + 1:]
        return h[1:end], (rest[1:] if rest.startswith(":") else None)
    if h.count(":") == 1:
        name, port = h.split(":")
        return name, port
    return h, None


class HostCheck:
    """Which Host headers to accept.

    Loopback bind: only 127.0.0.1 / localhost / [::1] on our port (plus any --allowed-host).
    Other binds: the --allowed-host list plus the loopback names on any port; with no list,
    any Host (the caller warns).
    """

    def __init__(self, bind, port, allowed=()):
        self.port = str(port)
        self.allowed = [split_host(a) for a in allowed if a.strip()]
        self.loopback = is_loopback(bind)
        self.any = not self.loopback and not self.allowed

    def ok(self, header):
        if self.any:
            return True
        if not header:
            return False
        name, port = split_host(header)
        if name in ("127.0.0.1", "localhost", "::1") and (port == self.port or not self.loopback):
            return True
        return any(name == an and (ap is None or ap == port) for an, ap in self.allowed)


def make_handler(roots, port, *, bind="127.0.0.1", allowed_hosts=(), web_dir=None, auth=None):
    """auth: (username, password) to require HTTP Basic auth, or None."""
    hosts = HostCheck(bind, port, allowed_hosts)
    web_root = os.path.realpath(web_dir) if web_dir else None
    want_auth = None
    if auth:
        want_auth = b"Basic " + base64.b64encode(f"{auth[0]}:{auth[1]}".encode())

    class H(BaseHTTPRequestHandler):
        server_version = "gemifi-dashboard"
        timeout = 120   # a stalled client can't hold a thread forever

        def handle(self):
            if isinstance(self.connection, ssl.SSLSocket):
                try:
                    self.connection.do_handshake()
                except (ssl.SSLError, OSError) as e:
                    self.log_message("TLS handshake failed: %s", e)
                    return
            super().handle()

        def log_message(self, fmt, *args):
            sys.stderr.write("dashboard: " + fmt % args + "\n")

        # -- helpers --
        def send(self, code, body, ctype="application/json", extra=None, cache="no-store"):
            data = body if isinstance(body, bytes) else (
                json.dumps(body).encode() if ctype == "application/json" else body.encode())
            self.send_response(code)
            text = ctype.startswith("text/") or ctype in ("application/json", "application/manifest+json")
            self.send_header("Content-Type", ctype + ("; charset=utf-8" if text else ""))
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Cache-Control", cache)
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(data)

        def fail(self, code, msg, extra=None):
            self.send(code, {"error": msg}, extra=extra)

        def gate(self):
            """Host check, then Basic auth. False when a response was already sent."""
            if not hosts.ok(self.headers.get("Host")):
                self.fail(403, "bad Host header (see --allowed-host / GEMIFI_ALLOWED_HOSTS)")
                return False
            if want_auth is not None:
                got = (self.headers.get("Authorization") or "").encode("latin-1", "replace")
                if not hmac.compare_digest(got, want_auth):
                    self.fail(401, "authentication required",
                              {"WWW-Authenticate": 'Basic realm="gemifi", charset="UTF-8"'})
                    return False
            return True

        def body(self):
            # JSON only: a cross-site form can't send application/json without a CORS preflight.
            if not (self.headers.get("Content-Type") or "").startswith("application/json"):
                self.fail(415, "expected application/json")
                return None
            try:
                n = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                n = -1
            if n < 0 or n > 1 << 20:
                self.fail(413, "too large")
                return None
            try:
                b = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                self.fail(400, "bad JSON")
                return None
            if not isinstance(b, dict):
                self.fail(400, "expected a JSON object")
                return None
            return b

        def path_arg(self, ident):
            p = roots.resolve(ident)
            if p is None:
                self.fail(400, "bad device id")
            return p

        # -- GET --
        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            if not self.gate():
                return
            u = urllib.parse.urlsplit(self.path)
            q = dict(urllib.parse.parse_qsl(u.query))
            if u.path == "/api/info":
                return self.send(200, {"name": NAME, "version": VERSION, "user_dir": roots.dirs["mine"],
                                       "toolchains": toolchains()})
            if u.path == "/api/devices":
                return self.send(200, {"devices": roots.listing(), "user_dir": roots.dirs["mine"]})
            if u.path == "/api/presets":
                return self.send(200, {"presets": [{"name": n, "platform": pl, "description": d}
                                                   for n, _p, pl, d, _nm in gb.list_presets()]})
            if u.path == "/api/file":
                p = self.path_arg(q.get("id"))
                if p is None:
                    return
                if not os.path.isfile(p):
                    return self.fail(404, "no such file")
                with open(p, encoding="utf-8", errors="replace") as f:
                    return self.send(200, {"id": q["id"], "text": f.read()})
            if u.path in ("/api/image", "/download"):
                ident = q.get("id")
                if self.path_arg(ident) is None:
                    return
                name = q.get("file", "")
                if not IMAGE_RE.fullmatch(name) or name not in roots.images(ident):
                    return self.fail(404, "no such image")
                with open(os.path.join(roots.out_dir(ident), name), "rb") as f:
                    data = f.read()
                return self.send(200, data, "application/octet-stream",
                                 {"Content-Disposition": f'attachment; filename="{name}"'})
            if u.path.startswith("/api/"):
                return self.fail(404, "not found")
            if web_root is None:
                if u.path == "/":
                    return self.send(200, PAGE, "text/html")
                return self.fail(404, "not found")
            return self.static(u.path)

        def static(self, url_path):
            rel = urllib.parse.unquote(url_path).lstrip("/")
            if rel == "" or rel.endswith("/"):
                rel += "index.html"
            if "\0" in rel or "\\" in rel:
                return self.fail(404, "not found")
            path = os.path.realpath(os.path.join(web_root, rel))
            if not path.startswith(web_root + os.sep):
                return self.fail(404, "not found")
            if not os.path.isfile(path):
                # SPA fallback for extensionless routes; missing assets stay 404.
                if "." in os.path.basename(rel):
                    return self.fail(404, "not found")
                path = os.path.join(web_root, "index.html")
                if not os.path.isfile(path):
                    return self.fail(404, "not found")
            ext = os.path.splitext(path)[1].lower()
            ctype = MIME.get(ext) or mimetypes.guess_type(path)[0] or "application/octet-stream"
            immutable = os.path.relpath(path, web_root).split(os.sep)[0] == "assets"
            with open(path, "rb") as f:
                data = f.read()
            return self.send(200, data, ctype,
                             cache="public, max-age=31536000, immutable" if immutable else "no-cache")

        # -- POST --
        def do_POST(self):
            if not self.gate():
                return
            u = urllib.parse.urlsplit(self.path)
            b = self.body()
            if b is None:
                return
            if u.path == "/api/validate":
                text = b.get("text")
                if not isinstance(text, str):
                    return self.fail(400, "text required")
                ident = b.get("id")
                dev, diags = gd.load_device(ident if isinstance(ident, str) and ident else "device.yaml", text)
                return self.send(200, {"ok": dev is not None, "summary": gb.summary(dev) if dev else "",
                                       "diags": [d.as_dict() for d in diags]})
            if u.path == "/api/save":
                p = self.path_arg(b.get("id"))
                if p is None:
                    return
                if not isinstance(b.get("text"), str):
                    return self.fail(400, "text required")
                if not roots.writable(b["id"]):
                    return self.fail(403, "read-only: copy it to your devices with New")
                os.makedirs(os.path.dirname(p), exist_ok=True)
                tmp = p + ".tmp"
                with open(tmp, "w", encoding="utf-8") as f:
                    f.write(b["text"])
                os.replace(tmp, p)
                return self.send(200, {"ok": True})
            if u.path == "/api/new":
                try:
                    p = gb.new_device(str(b.get("name", "")), str(b.get("preset", "")), roots.dirs["mine"])
                except (ValueError, OSError) as e:
                    return self.fail(400, str(e))
                return self.send(200, {"id": "mine/" + os.path.basename(p)})
            if u.path == "/api/delete":
                ident = b.get("id")
                p = self.path_arg(ident)
                if p is None:
                    return
                if not ident.startswith("mine/"):
                    return self.fail(403, "only your own devices (mine/) can be deleted")
                if not os.path.isfile(p):
                    return self.fail(404, "no such file")
                if _build_lock.locked():
                    return self.fail(409, "a build is running")
                try:
                    os.remove(p)
                except OSError as e:
                    return self.fail(403, f"can't delete: {e.strerror}")
                shutil.rmtree(roots.out_dir(ident), ignore_errors=True)
                return self.send(200, {"ok": True})
            if u.path == "/api/build":
                ident = b.get("id")
                p = self.path_arg(ident)
                if p is None:
                    return
                if not os.path.isfile(p):
                    return self.fail(404, "no such file")
                return self.stream_build(p, roots.out_dir(ident))
            self.fail(404, "not found")

        def stream_build(self, path, out_dir):
            if not _build_lock.acquire(blocking=False):
                return self.fail(409, "a build is already running")
            try:
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.send_header("X-Accel-Buffering", "no")   # reverse proxies: don't buffer the log
                self.end_headers()   # HTTP/1.0: the body ends when the connection closes
                cmd = [sys.executable, "-u", os.path.join(gb.HERE, "gemifi_build.py"), "build", path,
                       "-o", out_dir]
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=gb.REPO)
                try:
                    for line in iter(proc.stdout.readline, b""):
                        self.wfile.write(line)
                        self.wfile.flush()
                    rc = proc.wait()
                    self.wfile.write(f"\n[exit {rc}]\n".encode())
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError, TimeoutError):
                    proc.kill()
                    proc.wait()
                finally:
                    proc.stdout.close()
            finally:
                _build_lock.release()

    return H


def find_web_dir(web_dir):
    """--web if given (must exist), else web/updater/dist if built, else None (inline page)."""
    if web_dir:
        if not os.path.isfile(os.path.join(web_dir, "index.html")):
            raise SystemExit(f"gemifi dashboard: {web_dir}/index.html not found")
        return os.path.abspath(web_dir)
    if os.path.isfile(os.path.join(DEFAULT_WEB_DIR, "index.html")):
        return DEFAULT_WEB_DIR
    return None


def serve(port, user_dir, *, host="127.0.0.1", web_dir=None, build_dir=None, allowed_hosts=(),
          tls_cert=None, tls_key=None, auth=None):
    roots = Roots(user_dir, build_dir)
    web = find_web_dir(web_dir)
    if bool(tls_cert) != bool(tls_key):
        raise SystemExit("gemifi dashboard: --tls-cert and --tls-key go together")
    handler = make_handler(roots, port, bind=host, allowed_hosts=allowed_hosts, web_dir=web, auth=auth)
    server_cls = type("Server", (ThreadingHTTPServer,), {
        "address_family": socket.AF_INET6 if ":" in host else socket.AF_INET, "daemon_threads": True})
    srv = server_cls((host, port), handler)
    scheme = "http"
    if tls_cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(tls_cert, tls_key)
        # Handshake in the handler thread, not in accept() on the main loop.
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True, do_handshake_on_connect=False)
        scheme = "https"
    shown = "localhost" if host in ("0.0.0.0", "::") else (f"[{host}]" if ":" in host else host)
    print(f"gemifi dashboard: {scheme}://{shown}:{port}/  (presets: {gb.rel(roots.dirs['presets'])}"
          f"{' read-only' if roots.readonly_presets else ''}, yours: {user_dir}, builds: {roots.build_dir}, "
          f"web: {web or 'inline page'}; Ctrl-C to stop)", flush=True)
    if not is_loopback(host):
        if not allowed_hosts:
            print("gemifi dashboard: warning: listening on a non-loopback address with no --allowed-host: "
                  "any Host header is accepted" + ("" if auth else
                  "; anyone who can reach this port can edit and build: set GEMIFI_PASSWORD"),
                  file=sys.stderr, flush=True)
        elif not auth:
            print("gemifi dashboard: warning: listening on a non-loopback address without a password: "
                  "set GEMIFI_PASSWORD", file=sys.stderr, flush=True)
        if scheme == "http":
            print("gemifi dashboard: note: browsers only allow Web MIDI / Web Serial on https or localhost; "
                  "use --tls-cert/--tls-key (or an SSH tunnel) to flash from another machine",
                  file=sys.stderr, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>gemifi dashboard</title>
<style>
:root{--bg:#f6f6f4;--fg:#1d1d1b;--muted:#6b6b66;--card:#fff;--line:#d9d9d4;--acc:#2a6df4;--err:#c0352b;--warn:#a86b00;--ok:#2d7d46}
@media (prefers-color-scheme:dark){:root{--bg:#18181a;--fg:#e8e8e6;--muted:#9a9a96;--card:#222225;--line:#3a3a3e;--acc:#6d9bff;--err:#ff7a6e;--warn:#e0a83a;--ok:#5fc27e}}
*{box-sizing:border-box}body{margin:0;font:14px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:12px 16px;border-bottom:1px solid var(--line);display:flex;gap:16px;align-items:baseline;flex-wrap:wrap}
header h1{font-size:18px;margin:0}header a{color:var(--acc)}main{display:grid;grid-template-columns:320px 1fr;gap:16px;padding:16px}
@media (max-width:800px){main{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px}
.dev{padding:8px;border-radius:6px;cursor:pointer;border:1px solid transparent}.dev:hover{border-color:var(--line)}.dev.sel{border-color:var(--acc)}
.dev b{display:block}.small{color:var(--muted);font-size:12px}
textarea{width:100%;min-height:55vh;font:13px/1.4 ui-monospace,monospace;background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:8px;tab-size:2}
button,select,input{font:inherit;padding:5px 10px;border-radius:6px;border:1px solid var(--line);background:var(--card);color:var(--fg)}
button.primary{background:var(--acc);color:#fff;border-color:var(--acc)}button:disabled{opacity:.5}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:8px 0}
.diag{font:12px/1.4 ui-monospace,monospace;white-space:pre-wrap;margin:2px 0;cursor:pointer}.error{color:var(--err)}.warning{color:var(--warn)}.okmsg{color:var(--ok)}
pre#log{max-height:40vh;overflow:auto;background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:8px;font-size:12px;white-space:pre-wrap}
h2{font-size:14px;margin:12px 0 6px}
</style></head><body>
<header><h1>gemifi dashboard</h1><span class="small">device YAML &rarr; firmware. Flash and configure with the
web updater: build it (<code>cd web/updater &amp;&amp; npm ci &amp;&amp; npm run build</code>) and restart, and it is served here.
Format: core/README.md &ldquo;Device YAML&rdquo;.</span></header>
<main>
<section class="card"><h2>Devices</h2><div id="list"></div>
<h2>New from preset</h2>
<div class="row"><input id="newname" placeholder="my-controller" size="14"><select id="preset"></select><button id="create">Create</button></div>
<div class="small" id="userdir"></div></section>
<section class="card"><div class="row"><b id="cur">pick a device</b><span class="small" id="dirty"></span></div>
<textarea id="ed" spellcheck="false" disabled></textarea>
<div class="row"><button id="save" disabled>Save</button><button id="validate" disabled>Validate</button><button id="build" class="primary" disabled>Save &amp; build</button><span id="imgs"></span></div>
<div id="diags"></div><pre id="log" hidden></pre></section>
</main>
<script>
const $=id=>document.getElementById(id);let cur=null,devices=[];
async function api(path,body){const r=await fetch(path,body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
 const j=await r.json().catch(()=>({error:r.statusText}));if(!r.ok)throw new Error(j.error||r.statusText);return j}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function refresh(){const j=await api('/api/devices');devices=j.devices;$('userdir').textContent='New devices go in '+j.user_dir;
 $('list').innerHTML=devices.map(d=>`<div class="dev${cur===d.id?' sel':''}" data-id="${esc(d.id)}"><b>${esc(d.name||d.file)}</b>
 <span class="small">${esc(d.root)}/${esc(d.file)} &middot; ${esc(d.platform)}${d.images.length?(d.stale?' &middot; out of date':' &middot; built'):''}</span><div class="small">${esc(d.description)}</div></div>`).join('')||'<span class="small">none</span>';
 document.querySelectorAll('.dev').forEach(e=>e.onclick=()=>openDev(e.dataset.id));showImages()}
async function openDev(id){if(cur&&$('dirty').textContent&&!confirm('Discard unsaved changes?'))return;
 const j=await api('/api/file?id='+encodeURIComponent(id));cur=id;$('ed').value=j.text;$('ed').disabled=false;
 ['save','validate','build'].forEach(b=>$(b).disabled=false);$('cur').textContent=id;$('dirty').textContent='';$('diags').innerHTML='';$('log').hidden=true;refresh()}
function showImages(){const d=devices.find(x=>x.id===cur);$('imgs').innerHTML=d?d.images.map(f=>`<a href="/api/image?id=${encodeURIComponent(cur)}&file=${encodeURIComponent(f)}">${esc(f)}</a>`).join(' '):''}
function gotoLine(n){const t=$('ed'),lines=t.value.split('\n');let p=0;for(let i=0;i<n-1&&i<lines.length;i++)p+=lines[i].length+1;t.focus();t.setSelectionRange(p,p+(lines[n-1]||'').length)}
async function validate(){const j=await api('/api/validate',{id:cur,text:$('ed').value});
 $('diags').innerHTML=(j.ok?`<div class="okmsg">OK: ${esc(j.summary)}</div>`:'')+j.diags.map(d=>`<div class="diag ${d.level}" data-line="${d.line}">line ${d.line||'?'}: ${d.level}: ${esc(d.path?d.path+': ':'')}${esc(d.msg)}</div>`).join('');
 document.querySelectorAll('.diag').forEach(e=>e.onclick=()=>gotoLine(+e.dataset.line));return j.ok}
async function save(){await api('/api/save',{id:cur,text:$('ed').value});$('dirty').textContent=''}
async function build(){await save();if(!await validate())return;const log=$('log');log.hidden=false;log.textContent='';$('build').disabled=true;
 try{const r=await fetch('/api/build',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:cur})});
  if(!r.ok){log.textContent=(await r.json()).error;return}
  const rd=r.body.getReader(),dec=new TextDecoder();for(;;){const {done,value}=await rd.read();if(done)break;log.textContent+=dec.decode(value,{stream:true});log.scrollTop=log.scrollHeight}}
 finally{$('build').disabled=false;refresh()}}
$('ed').oninput=()=>$('dirty').textContent='(unsaved)';
$('ed').onkeydown=e=>{if(e.key==='Tab'){e.preventDefault();document.execCommand('insertText',false,'  ')}};
$('save').onclick=()=>save().catch(e=>alert(e.message));$('validate').onclick=()=>validate().catch(e=>alert(e.message));$('build').onclick=()=>build().catch(e=>alert(e.message));
$('create').onclick=async()=>{try{const j=await api('/api/new',{name:$('newname').value.trim(),preset:$('preset').value});await refresh();openDev(j.id)}catch(e){alert(e.message)}};
api('/api/presets').then(j=>$('preset').innerHTML=j.presets.map(p=>`<option value="${esc(p.name)}">${esc(p.name)} (${esc(p.platform)})</option>`).join(''));
refresh();
</script></body></html>
"""

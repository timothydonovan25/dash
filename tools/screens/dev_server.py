#!/usr/bin/env python3
"""The screen editor on a laptop, against the simulator's card.

    cmake -S test -B build-host && cmake --build build-host --target screens_json
    python3 tools/screens/dev_server.py [--card sim_card] [--port 8090]
    open http://localhost:8090/screens

Serves components/services/web/screens.html and the same endpoints the
dash's hotspot does (services/src/wifi_service.cpp), backed by build-host's
screens_json, so a .dui is checked by the firmware's own decode(). Layouts
land in <card>/ui/, where the simulator reads them (`layout reload` on its
stdin, or restart it). A Live draft is written to <card>/../draft.dui:
`layout draft <that path>` shows it in the sim.
"""
import argparse, http.server, json, os, re, subprocess, urllib.parse

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../.."))
PAGE = os.path.join(ROOT, "components/services/web/screens.html")
NAME = re.compile(r"^[a-z0-9-]{1,16}\.dui$")


def tool(*args, data=None):
    exe = os.path.join(ROOT, "build-host/screens_json")
    return subprocess.run([exe, *args], input=data, capture_output=True)


class H(http.server.BaseHTTPRequestHandler):
    card = "sim_card"

    def send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def ui(self, *p):
        return os.path.join(self.card, "ui", *p)

    def active(self):
        try:
            return open(self.ui("active")).read().strip()
        except OSError:
            return ""

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path in ("/", "/screens"):
            return self.send(200, open(PAGE, "rb").read(), "text/html; charset=utf-8")
        if u.path == "/api/fields":
            return self.send(200, tool("fields").stdout)
        if u.path == "/api/values":
            return self.send(200, tool("values", q.get("ids", [""])[0] or "0").stdout)
        if u.path == "/api/ui":
            return self.send(200, {"card": True, "active": self.active(), "error": ""})
        if u.path == "/api/ui/stock":
            return self.send(200, tool("stock").stdout, "application/octet-stream")
        if u.path == "/ui/":
            os.makedirs(self.ui(), exist_ok=True)
            return self.send(200, [{"name": n, "size": os.path.getsize(self.ui(n))}
                                   for n in sorted(os.listdir(self.ui())) if NAME.match(n)])
        if u.path.startswith("/ui/") and NAME.match(u.path[4:]):
            try:
                return self.send(200, open(self.ui(u.path[4:]), "rb").read(), "application/octet-stream")
            except OSError:
                return self.send(404, {"ok": False, "error": "no such file"})
        self.send(404, {"ok": False, "error": "not found"})

    def checked(self):
        data = self.body()
        r = tool("check", data=data)
        res = json.loads(r.stdout or b'{"ok":false,"error":"no answer"}')
        return data, res

    def do_PUT(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/api/draft":
            data, res = self.checked()
            if not res["ok"]:
                return self.send(400, res)
            open(os.path.join(self.card, "..", "draft.dui"), "wb").write(data)
            return self.send(200, {"ok": True})
        if u.path.startswith("/ui/"):
            name = u.path[4:]
            if not NAME.match(name):
                return self.send(400, {"ok": False, "error": "name: a-z, 0-9 and -, then .dui"})
            data, res = self.checked()
            if not res["ok"]:
                return self.send(400, res)
            os.makedirs(self.ui(), exist_ok=True)
            open(self.ui(name), "wb").write(data)
            return self.send(200, {"ok": True})
        self.send(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/api/ui":
            name = urllib.parse.parse_qs(u.query).get("active", [""])[0]
            if name and not NAME.match(name):
                return self.send(400, {"ok": False, "error": "not a .dui name"})
            os.makedirs(self.ui(), exist_ok=True)
            if name:
                open(self.ui("active"), "w").write(name + "\n")
            elif os.path.exists(self.ui("active")):
                os.remove(self.ui("active"))
            return self.send(200, {"ok": True})
        self.send(404, {"ok": False, "error": "not found"})

    def do_DELETE(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/api/draft":
            return self.send(200, {"ok": True})
        if u.path.startswith("/ui/") and NAME.match(u.path[4:]):
            name = u.path[4:]
            if os.path.exists(self.ui(name)):
                os.remove(self.ui(name))
            if self.active() == name:
                os.remove(self.ui("active"))
            return self.send(200, {"ok": True})
        self.send(404, {"ok": False, "error": "not found"})

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--card", default="sim_card")
    ap.add_argument("--port", type=int, default=8090)
    a = ap.parse_args()
    H.card = os.path.abspath(a.card)
    print(f"editor: http://localhost:{a.port}/screens   card: {H.card}")
    http.server.ThreadingHTTPServer(("127.0.0.1", a.port), H).serve_forever()


if __name__ == "__main__":
    main()

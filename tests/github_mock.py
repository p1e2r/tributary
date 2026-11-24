#!/usr/bin/env python3
"""A tiny in-memory mock of the GitHub REST endpoints Tributary's editor uses."""
import base64, hashlib, json, re, sys, threading, subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, unquote

LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/ghmock-log.json"
lock = threading.Lock()
blobs = {}
repos = {}
events = []


def blob(data: bytes) -> str:
    sha = hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()
    blobs[sha] = data
    return sha


def tree_sha(entries):
    s = json.dumps(sorted([(e["path"], e["mode"], e["sha"]) for e in entries])).encode()
    return hashlib.sha1(s).hexdigest()


def make_repo(full, files, push, default="main"):
    entries = [{"path": p, "mode": m, "type": "blob", "sha": blob(d), "size": len(d)} for p, (d, m) in files.items()]
    t = tree_sha(entries)
    c = hashlib.sha1((full + t).encode()).hexdigest()
    repos[full] = {"push": push, "default": default, "trees": {t: entries}, "commits": {c: {"tree": t, "parents": [], "message": "init"}},
                   "branches": {default: c, "dev": c}, "pulls": []}


png = subprocess.run(["convert", "-size", "96x64", "gradient:#0366d6-#28a745", "png:-"], capture_output=True).stdout
make_repo("octo/demo", {
    "README.md": (b"# Demo\n\nA small project used to try **Tributary**'s editor.\n\n## Building\n\n```sh\n./scripts/build.sh\n```\n", "100644"),
    "src/main.cpp": (b'#include <cstdio>\n#include "util.hpp"\n\n// Entry point\nint main(int argc, char** argv) {\n    const char* name = argc > 1 ? argv[1] : "world";\n    std::printf("Hello, %s!\\n", name);\n    return add(2, 3) == 5 ? 0 : 1;\n}\n', "100644"),
    "src/util.hpp": (b"#pragma once\n\n/* Adds two numbers. */\ninline int add(int a, int b) {\n    return a + b;\n}\n", "100644"),
    "docs/guide.md": (b"# Guide\n\nRun the program with your name.\n", "100644"),
    "package.json": (b'{\n  "name": "demo",\n  "version": "1.0.0",\n  "private": true\n}\n', "100644"),
    "scripts/build.sh": (b"#!/bin/sh\n# build the demo\nc++ -O2 src/main.cpp -o demo\n", "100755"),
    "assets/logo.png": (png, "100644"),
}, push=False)
make_repo("tester/notes", {"notes.md": (b"# Notes\n\n- first\n", "100644"), "todo.txt": (b"buy milk\n", "100644")}, push=True)


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length", 0))
        return json.loads(self.rfile.read(n) or b"{}")

    def repo_json(self, full):
        r = repos[full]
        o, n = full.split("/")
        return {"full_name": full, "name": n, "owner": {"login": o}, "default_branch": r["default"], "private": False, "fork": full.startswith("tester/") and n == "demo",
                "clone_url": f"https://github.com/{full}.git", "html_url": f"https://github.com/{full}", "permissions": {"push": r["push"], "pull": True}}

    def do_GET(self):
        u = urlparse(self.path)
        p = unquote(u.path)
        with lock:
            if p == "/_bump":  # test hook: someone else pushes a change to README.md on main
                r = repos["octo/demo"]
                c = r["branches"]["main"]
                entries = [dict(e) for e in r["trees"][r["commits"][c]["tree"]]]
                for e in entries:
                    if e["path"] == "README.md":
                        e["sha"] = blob(b"# Demo\n\nChanged by someone else.\n")
                t = tree_sha(entries)
                r["trees"][t] = entries
                nc = hashlib.sha1((t + "bump").encode()).hexdigest()
                r["commits"][nc] = {"tree": t, "parents": [c], "message": "bump"}
                r["branches"]["main"] = nc
                return self.send(200, {"ok": True})
            if p == "/user":
                return self.send(200, {"login": "tester", "name": "Test User", "id": 7, "email": "tester@example.com", "avatar_url": ""})
            if p == "/user/emails":
                return self.send(200, [{"email": "tester@example.com", "verified": True, "primary": True}])
            if p == "/user/repos":
                return self.send(200, [self.repo_json(k) for k in repos])
            if p == "/user/orgs":
                return self.send(200, [])
            m = re.match(r"^/repos/([^/]+/[^/]+)(/.*)?$", p)
            if not m or m.group(1) not in repos:
                return self.send(404, {"message": "Not Found"})
            full, rest = m.group(1), m.group(2) or ""
            r = repos[full]
            if rest == "":
                return self.send(200, self.repo_json(full))
            if rest == "/branches":
                return self.send(200, [{"name": b} for b in r["branches"]])
            mm = re.match(r"^/git/ref/heads/(.+)$", rest)
            if mm:
                b = mm.group(1)
                if b not in r["branches"]:
                    return self.send(404, {"message": "Not Found"})
                return self.send(200, {"ref": "refs/heads/" + b, "object": {"sha": r["branches"][b], "type": "commit"}})
            mm = re.match(r"^/git/commits/(\w+)$", rest)
            if mm:
                c = r["commits"].get(mm.group(1))
                return self.send(200, {"sha": mm.group(1), "tree": {"sha": c["tree"]}}) if c else self.send(404, {"message": "Not Found"})
            mm = re.match(r"^/git/trees/(\w+)$", rest)
            if mm:
                t = r["trees"].get(mm.group(1))
                return self.send(200, {"sha": mm.group(1), "tree": t, "truncated": False}) if t is not None else self.send(404, {"message": "Not Found"})
            mm = re.match(r"^/git/blobs/(\w+)$", rest)
            if mm and mm.group(1) in blobs:
                d = blobs[mm.group(1)]
                return self.send(200, {"sha": mm.group(1), "size": len(d), "encoding": "base64", "content": base64.encodebytes(d).decode()})
            if rest == "/pulls":
                return self.send(200, [])
            return self.send(404, {"message": "Not Found"})

    def do_POST(self):
        u = urlparse(self.path)
        p = unquote(u.path)
        b = self.body()
        with lock:
            m = re.match(r"^/repos/([^/]+/[^/]+)(/.*)$", p)
            if not m or m.group(1) not in repos:
                return self.send(404, {"message": "Not Found"})
            full, rest = m.group(1), m.group(2)
            r = repos[full]
            if rest == "/forks":
                fk = "tester/" + full.split("/")[1]
                if fk not in repos:
                    repos[fk] = json.loads(json.dumps({k: v for k, v in r.items() if k != "pulls"}))
                    repos[fk]["push"] = True
                    repos[fk]["pulls"] = []
                events.append({"fork": fk})
                return self.send(202, self.repo_json(fk))
            if rest == "/merge-upstream":
                return self.send(200, {"message": "ok"})
            if not r["push"] and rest.startswith("/git/"):
                return self.send(403, {"message": "Resource not accessible by integration"})
            if rest == "/git/blobs":
                data = base64.b64decode(b["content"]) if b.get("encoding") == "base64" else b["content"].encode()
                return self.send(201, {"sha": blob(data)})
            if rest == "/git/trees":
                base = {e["path"]: e for e in r["trees"].get(b.get("base_tree"), [])}
                # base trees may live in the upstream (fork network)
                if not base:
                    for other in repos.values():
                        if b.get("base_tree") in other["trees"]:
                            base = {e["path"]: e for e in other["trees"][b["base_tree"]]}
                for e in b["tree"]:
                    if e.get("sha") is None:
                        base.pop(e["path"], None)
                    else:
                        base[e["path"]] = {"path": e["path"], "mode": e["mode"], "type": "blob", "sha": e["sha"], "size": len(blobs.get(e["sha"], b""))}
                entries = sorted(base.values(), key=lambda e: e["path"])
                t = tree_sha(entries)
                r["trees"][t] = entries
                return self.send(201, {"sha": t})
            if rest == "/git/commits":
                c = hashlib.sha1((b["tree"] + b["message"] + "".join(b["parents"])).encode()).hexdigest()
                r["commits"][c] = {"tree": b["tree"], "parents": b["parents"], "message": b["message"]}
                return self.send(201, {"sha": c})
            if rest == "/git/refs":
                name = b["ref"][len("refs/heads/"):]
                if name in r["branches"]:
                    return self.send(422, {"message": "Reference already exists"})
                r["branches"][name] = b["sha"]
                return self.send(201, {"ref": b["ref"], "object": {"sha": b["sha"]}})
            if rest == "/pulls":
                num = len(r["pulls"]) + 1
                head = b["head"]
                head_repo = (head.split(":")[0] + "/" + full.split("/")[1]) if ":" in head else full
                head_branch = head.split(":")[-1]
                hr = repos[head_repo]
                commit = hr["commits"][hr["branches"][head_branch]]
                files = {e["path"]: blobs[e["sha"]].decode("utf-8", "replace") if e["path"].endswith((".md", ".cpp", ".hpp", ".json", ".sh", ".txt")) else "<binary>"
                         for e in hr["trees"][commit["tree"]]}
                pr = {"number": num, "title": b["title"], "body": b.get("body", ""), "head": head, "base": b["base"], "draft": b.get("draft", False),
                      "commit_message": commit["message"], "files": files}
                r["pulls"].append(pr)
                events.append({"pull": pr, "repo": full})
                json.dump(events, open(LOG, "w"), indent=1)
                return self.send(201, {"number": num, "html_url": f"https://github.com/{full}/pull/{num}"})
            return self.send(404, {"message": "Not Found"})


ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8765), H).serve_forever()

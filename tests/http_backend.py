#!/usr/bin/env python3
"""Tiny smart-HTTP git server (wraps `git http-backend`) for transport tests,
plus a Git LFS server (batch API + basic transfer) at <repo>.git/info/lfs."""
import http.server, subprocess, sys, os, json, hashlib, re
ROOT = sys.argv[1]; PORT = int(sys.argv[2])
LFS = os.path.join(ROOT, '.lfs-objects')
os.makedirs(LFS, exist_ok=True)
class H(http.server.BaseHTTPRequestHandler):
    def reply(self, code, obj=None, raw=None, ctype='application/vnd.git-lfs+json'):
        data = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b'')
        self.send_response(code); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
    def lfs(self, path, body):
        host = 'http://' + self.headers.get('Host')
        m = re.match(r'^(.*\.git)/info/lfs/objects/batch$', path)
        if m and self.command == 'POST':
            req = json.loads(body)
            out = []
            for o in req['objects']:
                have = os.path.exists(os.path.join(LFS, o['oid']))
                e = {'oid': o['oid'], 'size': o['size'], 'authenticated': True}
                href = host + '/_lfs/' + o['oid']
                if req['operation'] == 'download':
                    if have: e['actions'] = {'download': {'href': href, 'header': {'X-Test': 'dl'}}}
                    else: e['error'] = {'code': 404, 'message': 'Object does not exist'}
                elif not have:
                    e['actions'] = {'upload': {'href': href, 'header': {'X-Test': 'up'}}, 'verify': {'href': host + '/_lfs_verify'}}
                out.append(e)
            return self.reply(200, {'transfer': 'basic', 'objects': out})
        m = re.match(r'^/_lfs/([0-9a-f]{64})$', path)
        if m:
            fp = os.path.join(LFS, m.group(1))
            if self.command == 'PUT':
                if hashlib.sha256(body).hexdigest() != m.group(1): return self.reply(422, {'message': 'bad hash'})
                open(fp, 'wb').write(body); return self.reply(200, {})
            if os.path.exists(fp): return self.reply(200, raw=open(fp, 'rb').read(), ctype='application/octet-stream')
            return self.reply(404, {'message': 'missing'})
        if path == '/_lfs_verify':
            o = json.loads(body)
            return self.reply(200 if os.path.exists(os.path.join(LFS, o['oid'])) else 404, {})
        return False
    def handle_one(self):
        path, _, query = self.path.partition('?')
        length = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(length) if length else b''
        if '/info/lfs/' in path or path.startswith('/_lfs'):
            if self.lfs(path, body) is not False: return
            return self.reply(404, {'message': 'not found'})
        env = dict(os.environ, GIT_PROJECT_ROOT=ROOT, GIT_HTTP_EXPORT_ALL='1', REQUEST_METHOD=self.command,
                   PATH_INFO=path, QUERY_STRING=query, CONTENT_TYPE=self.headers.get('Content-Type', ''),
                   CONTENT_LENGTH=str(len(body)), REMOTE_USER='tester', REMOTE_ADDR='127.0.0.1')
        auth = self.headers.get('Authorization')
        if os.environ.get('REQUIRE_AUTH') and auth is None:
            self.send_response(401); self.send_header('WWW-Authenticate', 'Basic realm="t"'); self.send_header('Content-Length','0'); self.end_headers(); return
        out = subprocess.run(['git', 'http-backend'], input=body, env=env, capture_output=True).stdout
        head, _, payload = out.partition(b'\r\n\r\n')
        status = 200
        hdrs = []
        for line in head.split(b'\r\n'):
            k, _, v = line.decode().partition(':')
            if k.lower() == 'status': status = int(v.strip().split()[0])
            elif k: hdrs.append((k, v.strip()))
        self.send_response(status)
        for k, v in hdrs: self.send_header(k, v)
        self.send_header('Content-Length', str(len(payload))); self.end_headers(); self.wfile.write(payload)
    do_GET = do_POST = do_PUT = handle_one
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()

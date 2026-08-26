#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""最小 WebDAV 测试服务器（仅标准库），用于本地验证 davsync.py。

用法:
    python dav_server.py [docroot] [port]
例:
    python dav_server.py /tmp/davtest/remote 8765

支持: PROPFIND(Depth 0/1) / GET / PUT / MKCOL / DELETE / MOVE
支持 Basic 认证（可选）: --user name --pass secret
"""
import argparse
import os
import shutil
import sys
from email.utils import formatdate
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse


def make_handler(root, user, password):
    auth = None
    if user:
        import base64
        token = base64.b64encode(("%s:%s" % (user, password)).encode()).decode()
        auth = "Basic " + token

    class DAVHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        # -- 工具 --------------------------------------------------------
        def _fs(self, url_path):
            rel = unquote(urlparse(url_path).path).lstrip("/")
            return os.path.normpath(os.path.join(root, rel))

        def _href(self, fs_path):
            rel = os.path.relpath(fs_path, root).replace(os.sep, "/")
            return "/" if rel == "." else "/" + rel

        def _etag(self, fs_path):
            st = os.stat(fs_path)
            return '"%x-%x"' % (st.st_size, int(st.st_mtime))

        def _send(self, code, body=b"", ctype="text/plain; charset=utf-8", extra=None):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if body:
                self.wfile.write(body)

        def _check_auth(self):
            if auth is None:
                return True
            if self.headers.get("Authorization") == auth:
                return True
            self._send(401, b"Unauthorized", extra={"WWW-Authenticate": 'Basic realm="dav"'})
            return False

        def _read_body(self):
            length = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(length) if length else b""

        # -- 方法 --------------------------------------------------------
        def do_GET(self):
            if not self._check_auth():
                return
            fs = self._fs(self.path)
            if os.path.isdir(fs):
                self._send(405, b"directory")
                return
            if not os.path.isfile(fs):
                self._send(404, b"Not Found")
                return
            with open(fs, "rb") as f:
                body = f.read()
            self._send(200, body, "application/octet-stream", extra={
                "Last-Modified": formatdate(os.path.getmtime(fs), usegmt=True),
                "ETag": self._etag(fs),
            })

        def do_PUT(self):
            if not self._check_auth():
                return
            body = self._read_body()
            fs = self._fs(self.path)
            existed = os.path.isfile(fs)
            os.makedirs(os.path.dirname(fs), exist_ok=True)
            with open(fs, "wb") as f:
                f.write(body)
            self._send(204 if existed else 201)

        def do_MKCOL(self):
            if not self._check_auth():
                return
            self._read_body()
            fs = self._fs(self.path)
            if os.path.exists(fs):
                self._send(405)
                return
            os.makedirs(fs)
            self._send(201)

        def do_DELETE(self):
            if not self._check_auth():
                return
            self._read_body()
            fs = self._fs(self.path)
            if not os.path.exists(fs):
                self._send(404)
                return
            if os.path.isdir(fs):
                shutil.rmtree(fs)
            else:
                os.remove(fs)
            self._send(204)

        def do_MOVE(self):
            if not self._check_auth():
                return
            self._read_body()
            dest = self.headers.get("Destination")
            if not dest:
                self._send(400, b"missing Destination")
                return
            fs_src = self._fs(self.path)
            fs_dst = self._fs(dest)
            if not os.path.exists(fs_src):
                self._send(404)
                return
            os.makedirs(os.path.dirname(fs_dst), exist_ok=True)
            shutil.move(fs_src, fs_dst)
            self._send(201)

        def do_PROPFIND(self):
            if not self._check_auth():
                return
            self._read_body()
            depth = self.headers.get("Depth", "1")
            fs = self._fs(self.path)
            if not os.path.exists(fs):
                self._send(404, b"Not Found")
                return
            paths = [fs]
            if depth != "0" and os.path.isdir(fs):
                paths += [os.path.join(fs, n) for n in sorted(os.listdir(fs))]
            parts = ['<?xml version="1.0" encoding="utf-8"?>', '<d:multistatus xmlns:d="DAV:">']
            for p in paths:
                isdir = os.path.isdir(p)
                st = os.stat(p)
                size = 0 if isdir else st.st_size
                rt = ("<d:resourcetype><d:collection/></d:resourcetype>"
                      if isdir else "<d:resourcetype/>")
                cl = "" if isdir else "<d:getcontentlength>%d</d:getcontentlength>" % size
                parts.append(
                    "<d:response><d:href>%s</d:href><d:propstat><d:prop>%s%s"
                    "<d:getlastmodified>%s</d:getlastmodified>"
                    "<d:getetag>%s</d:getetag>"
                    "</d:prop><d:status>HTTP/1.1 200 OK</d:status>"
                    "</d:propstat></d:response>" % (
                        self._href(p), rt, cl,
                        formatdate(st.st_mtime, usegmt=True),
                        '"%x-%x"' % (size, int(st.st_mtime)),
                    )
                )
            parts.append("</d:multistatus>")
            self._send(207, "\n".join(parts).encode("utf-8"),
                       "application/xml; charset=utf-8")

        def log_message(self, fmt, *args):
            sys.stderr.write("[dav] %s\n" % (fmt % args))

    return DAVHandler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("port", nargs="?", type=int, default=8765)
    ap.add_argument("--user", default="")
    ap.add_argument("--pass", dest="password", default="")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    os.makedirs(root, exist_ok=True)
    httpd = ThreadingHTTPServer(("127.0.0.1", args.port),
                                make_handler(root, args.user, args.password))
    print("WebDAV test server: http://127.0.0.1:%d/  root=%s  auth=%s"
          % (args.port, root, "on" if args.user else "off"), flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

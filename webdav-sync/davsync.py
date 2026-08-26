#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DavSync —— 零依赖 WebDAV 双向同步器

仅使用 Python 标准库，将本地目录与 WebDAV 目录保持双向同步。

核心思路（三方比对）：
    本地快照  vs  上次同步状态  vs  远端快照
    - 仅本地变化        -> 上传
    - 仅远端变化        -> 下载
    - 两端都变化        -> 按冲突策略处理（可配置）
    - 一端删除、一端未变 -> 按配置传播删除（或恢复）

特性：
    - 冲突策略可配置：newer / local / remote / both / error
    - 删除传播可开关，忽略规则支持 Glob
    - 模拟运行（--dry-run）与状态查看（status）
    - 配置文件支持 ${ENV_VAR} 与环境变量密码，避免明文泄露

用法：
    python davsync.py init                        # 生成示例配置 davsync.json
    python davsync.py sync  -c davsync.json       # 执行双向同步
    python davsync.py status -c davsync.json      # 查看待同步项（不修改任何东西）
    python davsync.py sync  -c davsync.json --dry-run
"""
from __future__ import annotations

import argparse
import base64
import fnmatch
import json
import os
import posixpath
import re
import shutil
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from email.utils import parsedate_to_datetime
from urllib.parse import quote, unquote, urlparse
from xml.etree import ElementTree as ET

DAV = "{DAV:}"

PROPFIND_BODY = (
    '<?xml version="1.0" encoding="utf-8"?>'
    '<d:propfind xmlns:d="DAV:">'
    '<d:prop><d:resourcetype/><d:getcontentlength/>'
    '<d:getlastmodified/><d:getetag/></d:prop>'
    '</d:propfind>'
).encode("utf-8")

CONFLICT_POLICIES = ("newer", "local", "remote", "both", "error")

LABEL = {
    "upload": "上传",
    "download": "下载",
    "delete_local": "删除本地",
    "delete_remote": "删除远端",
    "keep_both": "冲突保留两份",
    "conflict": "冲突未处理",
    "forget": "清理记录",
    "ok": "跳过",
}

DEFAULT_CONFIG = {
    "url": "",                    # WebDAV 服务器地址，如 https://dav.example.com/dav/
    "username": "",               # 用户名（留空则匿名）
    "password": "",               # 密码
    "password_env": "",           # 优先从该环境变量读取密码，如 DAVSYNC_PASSWORD
    "local_dir": "",              # 本地目录
    "remote_dir": "",             # 远端子目录（相对 url，留空为根）
    "ignore": [                   # 忽略规则：无通配符时按路径段匹配任意层级
        ".davsync.json",
        "*.davsync.part",
        ".DS_Store",
        "Thumbs.db",
        ".git",
        "*.tmp",
        "~$*",
    ],
    "conflict_policy": "newer",   # newer | local | remote | both | error
    "propagate_delete": True,     # 是否把一端的删除传播到另一端
    "allow_mass_delete": False,   # 大规模删除保护：疑似对端数据丢失时中止（--force 可越过）
    "state_file": ".davsync.json",# 同步状态文件（相对 local_dir）
    "timeout": 30,                # HTTP 超时（秒）
    "pre_command": "",            # 同步前执行的 shell 命令（如 SQLite WAL checkpoint）
    "post_command": "",           # 同步完成后执行的 shell 命令（仅全部成功时）
}


class SyncError(Exception):
    """同步过程中的可报告错误。"""


# ---------------------------------------------------------------------------
# WebDAV 客户端（仅标准库）
# ---------------------------------------------------------------------------
class FileInfo:
    __slots__ = ("rel", "is_dir", "size", "mtime", "etag")

    def __init__(self, rel: str):
        self.rel = rel
        self.is_dir = False
        self.size = -1
        self.mtime = None
        self.etag = None


class WebDAVClient:
    """极简 WebDAV 客户端：PROPFIND / GET / PUT / MKCOL / DELETE / MOVE。"""

    def __init__(self, base_url, username=None, password=None, timeout=30):
        self.base_url = base_url if base_url.endswith("/") else base_url + "/"
        self.timeout = timeout
        self._auth = None
        if username:
            raw = "%s:%s" % (username, password or "")
            self._auth = "Basic " + base64.b64encode(raw.encode("utf-8")).decode("ascii")
        self._base_path = urlparse(self.base_url).path
        # 显式绕过系统代理，保证本地/内网服务器可达
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    # -- 基础请求 ------------------------------------------------------------
    def _url(self, rel):
        rel = (rel or "").strip("/")
        return self.base_url + quote(rel, safe="/") if rel else self.base_url

    def _raw(self, method, rel, data=None, headers=None):
        h = dict(headers or {})
        if self._auth:
            h["Authorization"] = self._auth
        req = urllib.request.Request(self._url(rel), data=data, method=method, headers=h)
        try:
            with self.opener.open(req, timeout=self.timeout) as resp:
                return resp.status, resp.read(), dict(resp.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)

    # -- 目录/文件操作 -------------------------------------------------------
    def stat_dir(self, rel):
        """PROPFIND Depth=1，返回 {relpath: FileInfo}（不含自身）；目录不存在返回 None。"""
        status, body, _ = self._raw(
            "PROPFIND", rel, data=PROPFIND_BODY,
            headers={"Depth": "1", "Content-Type": "application/xml"},
        )
        if status == 404:
            return None
        if status not in (200, 207):
            raise SyncError("PROPFIND %s: HTTP %s" % (rel or "/", status))
        return self._parse(body, rel)

    def walk(self):
        """递归列出远端全部文件，返回 {relpath: FileInfo}。"""
        files, seen, stack = {}, set(), [""]
        while stack:
            cur = stack.pop()
            if cur in seen:
                continue
            seen.add(cur)
            entries = self.stat_dir(cur)
            if entries is None:
                continue
            for path, info in entries.items():
                if info.is_dir:
                    stack.append(path)
                else:
                    files[path] = info
        return files

    def download(self, rel, dest_path):
        h = {}
        if self._auth:
            h["Authorization"] = self._auth
        req = urllib.request.Request(self._url(rel), method="GET", headers=h)
        try:
            with self.opener.open(req, timeout=self.timeout) as resp, open(dest_path, "wb") as f:
                shutil.copyfileobj(resp, f)
        except urllib.error.HTTPError as e:
            raise SyncError("GET %s: HTTP %s" % (rel, e.code)) from None

    def upload(self, rel, local_path):
        with open(local_path, "rb") as f:
            data = f.read()
        status, _, _ = self._raw(
            "PUT", rel, data=data,
            headers={"Content-Type": "application/octet-stream"},
        )
        if status not in (200, 201, 204):
            raise SyncError("PUT %s: HTTP %s" % (rel, status))

    def ensure_dir(self, rel):
        """逐级 MKCOL，保证远端目录存在（已存在时服务器返回 405，视为成功）。"""
        cur = ""
        for part in [p for p in rel.split("/") if p]:
            cur = "%s/%s" % (cur, part) if cur else part
            status, _, _ = self._raw("MKCOL", cur)
            if status not in (200, 201, 405):
                raise SyncError("MKCOL %s: HTTP %s" % (cur, status))

    def delete(self, rel):
        status, _, _ = self._raw("DELETE", rel)
        if status not in (200, 202, 204, 404):
            raise SyncError("DELETE %s: HTTP %s" % (rel, status))

    # -- PROPFIND 响应解析 ---------------------------------------------------
    def _rel(self, href):
        path = urlparse(href).path if "://" in href else href
        path = unquote(path)
        bp = self._base_path
        if bp and path.startswith(bp):
            path = path[len(bp):]
        else:
            bp2 = bp.rstrip("/")
            if bp2 and path.startswith(bp2):
                path = path[len(bp2):]
        return path.strip("/")

    def _parse(self, body, parent):
        out = {}
        root = ET.fromstring(body)
        parent = (parent or "").strip("/")
        for resp in root.iter(DAV + "response"):
            href = resp.find(DAV + "href")
            if href is None or not href.text:
                continue
            rel = self._rel(href.text.strip())
            if not rel or rel == parent:
                continue
            info = FileInfo(rel)
            for ps in resp.findall(DAV + "propstat"):
                st = ps.find(DAV + "status")
                if st is not None and "200" not in (st.text or ""):
                    continue
                prop = ps.find(DAV + "prop")
                if prop is None:
                    continue
                rt = prop.find(DAV + "resourcetype")
                if rt is not None and rt.find(DAV + "collection") is not None:
                    info.is_dir = True
                el = prop.find(DAV + "getcontentlength")
                if el is not None and el.text and el.text.isdigit():
                    info.size = int(el.text)
                el = prop.find(DAV + "getlastmodified")
                if el is not None and el.text:
                    try:
                        info.mtime = parsedate_to_datetime(el.text).timestamp()
                    except (TypeError, ValueError):
                        pass
                el = prop.find(DAV + "getetag")
                if el is not None and el.text:
                    info.etag = el.text.strip().strip('"')
            out[rel] = info
        return out


# ---------------------------------------------------------------------------
# 同步器
# ---------------------------------------------------------------------------
class Syncer:
    def __init__(self, cfg, verbose=False):
        self.cfg = cfg
        self.verbose = verbose
        self.local_dir = os.path.abspath(cfg["local_dir"])
        if not os.path.isdir(self.local_dir):
            raise SyncError("本地目录不存在: %s" % self.local_dir)
        # 配置文件若位于 local_dir 内，自动排除，避免凭据被上传
        cp = cfg.get("_config_path")
        if cp:
            try:
                rel = os.path.relpath(cp, self.local_dir)
            except ValueError:
                rel = None
            if rel and not rel.startswith(".."):
                ig = self.cfg.setdefault("ignore", [])
                if rel not in ig:
                    ig.append(rel)
        self.client = WebDAVClient(
            cfg["_base_url"],
            cfg.get("username") or None,
            cfg.get("password") or None,
            cfg.get("timeout") or 30,
        )
        self.state_path = cfg.get("state_file") or ".davsync.json"
        if not os.path.isabs(self.state_path):
            self.state_path = os.path.join(self.local_dir, self.state_path)
        # 状态文件本身永远不同步（此前会被上传到远端，属无意义且危险的行为）
        srel = os.path.relpath(self.state_path, self.local_dir)
        if not srel.startswith(".."):
            ig = self.cfg.setdefault("ignore", [])
            if srel not in ig:
                ig.append(srel)
        self.state = self._load_state()
        self.failed = set()

    # -- 状态 ----------------------------------------------------------------
    def _load_state(self):
        if os.path.isfile(self.state_path):
            try:
                with open(self.state_path, encoding="utf-8") as f:
                    return json.load(f)
            except (OSError, ValueError):
                print("警告: 状态文件损坏，将重建: %s" % self.state_path, file=sys.stderr)
        return {}

    def _save_state(self):
        tmp = self.state_path + ".tmp"
        os.makedirs(os.path.dirname(self.state_path) or ".", exist_ok=True)
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.state, f, ensure_ascii=False, indent=1, sort_keys=True)
        os.replace(tmp, self.state_path)

    # -- 扫描 ----------------------------------------------------------------
    def scan_local(self):
        """返回 {relpath: (mtime, size)}，按配置剪枝忽略目录。"""
        out = {}
        for dirpath, dirnames, filenames in os.walk(self.local_dir):
            rel_dir = os.path.relpath(dirpath, self.local_dir)
            if rel_dir == ".":
                rel_dir = ""
            dirnames[:] = [
                d for d in dirnames
                if not self.is_ignored(self._join(rel_dir, d))
            ]
            for fn in filenames:
                rel = self._join(rel_dir, fn)
                if self.is_ignored(rel):
                    continue
                st = os.stat(os.path.join(dirpath, fn))
                out[rel] = (st.st_mtime, st.st_size)
        return out

    @staticmethod
    def _join(rel_dir, name):
        return "%s/%s" % (rel_dir, name) if rel_dir else name

    def scan_remote(self):
        return self.client.walk()

    def is_ignored(self, rel):
        parts = rel.split("/")
        for pat in self.cfg.get("ignore") or []:
            p = str(pat).strip().strip("/")
            if not p:
                continue
            if "*" in p or "?" in p or "[" in p:
                if fnmatch.fnmatch(rel, p):
                    return True
                if any(fnmatch.fnmatch(part, p) for part in parts):
                    return True
                if p.endswith("/**") and rel.startswith(p[:-2]):
                    return True
            else:
                if p in parts or rel == p:
                    return True
        return False

    # -- 变化签名 ------------------------------------------------------------
    @staticmethod
    def _l_sig(l):
        return (round(l[0], 3), l[1])

    @staticmethod
    def _state_l_sig(s):
        sl = s.get("l") if s else None
        if not sl:
            return None
        return (round(sl[0], 3), sl[1])

    @staticmethod
    def _r_sig(r):
        return (int(r.mtime) if r.mtime is not None else None, r.size, r.etag)

    # -- 计划 ----------------------------------------------------------------
    def plan(self):
        self._local = self.scan_local()
        self._remote = self.scan_remote()
        actions = []
        for rel in sorted(set(self._local) | set(self._remote) | set(self.state)):
            if self.is_ignored(rel):
                continue
            l = self._local.get(rel)
            r = self._remote.get(rel)
            s = self.state.get(rel)
            if l and r:
                if s:
                    l_changed = self._l_sig(l) != self._state_l_sig(s)
                    r_changed = self._r_sig(r) != tuple(s.get("r") or ())
                else:
                    # 首次同步且两端内容明显一致 -> 直接记为已同步
                    if (l[1] == r.size and r.mtime is not None
                            and abs(l[0] - r.mtime) <= 2):
                        actions.append(("ok", rel, "首次同步: 识别为相同文件"))
                        continue
                    l_changed = r_changed = True
                if l_changed and r_changed:
                    actions.append(self._conflict(rel, l, r))
                elif l_changed:
                    actions.append(("upload", rel))
                elif r_changed:
                    actions.append(("download", rel))
                else:
                    actions.append(("ok", rel))
            elif l:
                if s and s.get("r"):
                    if self.cfg.get("propagate_delete", True):
                        actions.append(("delete_local", rel, "远端已删除"))
                    else:
                        actions.append(("upload", rel, "远端已删除, 重新上传"))
                else:
                    actions.append(("upload", rel, "新增"))
            elif r:
                if s and s.get("l"):
                    if self.cfg.get("propagate_delete", True):
                        actions.append(("delete_remote", rel, "本地已删除"))
                    else:
                        actions.append(("download", rel, "本地已删除, 重新下载"))
                else:
                    actions.append(("download", rel, "新增"))
            else:
                if s:
                    actions.append(("forget", rel))
        return actions

    def _conflict(self, rel, l, r):
        policy = self.cfg.get("conflict_policy", "newer")
        if policy == "error":
            return ("conflict", rel, "两端均已修改, 请手动处理")
        if policy == "local":
            return ("upload", rel, "冲突: 本地优先")
        if policy == "remote":
            return ("download", rel, "冲突: 远端优先")
        if policy == "both":
            return ("keep_both", rel, "冲突: 保留两份")
        # newer
        lm, rm = l[0], r.mtime or 0.0
        note = "冲突: 较新者胜 (本地 %s vs 远端 %s)" % (self._ts(lm), self._ts(rm))
        return ("upload", rel, note) if lm >= rm else ("download", rel, note)

    @staticmethod
    def _ts(t):
        return datetime.fromtimestamp(t).strftime("%m-%d %H:%M:%S") if t else "-"

    # -- 执行 ----------------------------------------------------------------
    def execute(self, actions):
        stats = {k: 0 for k in LABEL}
        for act in actions:
            kind, rel = act[0], act[1]
            note = act[2] if len(act) > 2 else ""
            if kind == "ok":
                if self.verbose:
                    print("  = 跳过 %s" % rel)
                continue
            if kind == "forget":
                stats[kind] += 1
                continue
            if kind == "conflict":
                self.failed.add(rel)
                stats[kind] += 1
                print("  ! 冲突未处理: %s (%s)" % (rel, note), file=sys.stderr)
                continue
            try:
                if kind == "upload":
                    parent = posixpath.dirname(rel)
                    if parent:
                        self.client.ensure_dir(parent)
                    self.client.upload(rel, os.path.join(self.local_dir, rel))
                    print("  ↑ 上传 %s%s" % (rel, self._note(note)))
                elif kind == "download":
                    self._download(rel, rel)
                    print("  ↓ 下载 %s%s" % (rel, self._note(note)))
                elif kind == "keep_both":
                    base, ext = posixpath.splitext(rel)
                    stamp = time.strftime("%Y%m%d-%H%M%S")
                    conflict_rel = "%s.conflict-%s%s" % (base, stamp, ext)
                    self._download(rel, conflict_rel)
                    parent = posixpath.dirname(rel)
                    if parent:
                        self.client.ensure_dir(parent)
                    self.client.upload(rel, os.path.join(self.local_dir, rel))
                    print("  ⇅ 冲突保留两份: %s + %s" % (rel, conflict_rel))
                elif kind == "delete_local":
                    os.remove(os.path.join(self.local_dir, rel))
                    print("  × 删除本地 %s%s" % (rel, self._note(note)))
                elif kind == "delete_remote":
                    self.client.delete(rel)
                    print("  × 删除远端 %s%s" % (rel, self._note(note)))
                stats[kind] += 1
            except Exception as e:  # 单文件失败不影响其他文件
                self.failed.add(rel)
                print("  ✗ 失败 %s: %s" % (rel, e), file=sys.stderr)
        self._rebuild_state()
        return stats

    @staticmethod
    def _note(note):
        return "  (%s)" % note if note else ""

    def _download(self, rel, dest_rel):
        """先写临时文件再原子替换；成功后把本地 mtime 设为远端 mtime，保证状态稳定。"""
        info = self._remote.get(rel)
        dest = os.path.join(self.local_dir, dest_rel)
        os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
        tmp = dest + ".davsync.part"
        try:
            self.client.download(rel, tmp)
            os.replace(tmp, dest)
        except Exception:
            if os.path.exists(tmp):
                os.remove(tmp)
            raise
        if info and info.mtime is not None:
            os.utime(dest, (info.mtime, info.mtime))

    def _rebuild_state(self):
        """执行后重新扫描两端，重建状态；失败项保留旧状态以便下次重试。"""
        try:
            local = self.scan_local()
        except OSError:
            local = {}
        try:
            remote = self.scan_remote()
        except Exception:
            remote = {}
        new_state = {}
        for rel in set(local) | set(remote):
            if rel in self.failed:
                if rel in self.state:
                    new_state[rel] = self.state[rel]
                continue
            entry = {}
            entry["l"] = [local[rel][0], local[rel][1]] if rel in local else None
            if rel in remote:
                r = remote[rel]
                entry["r"] = [r.mtime, r.size, r.etag]
            else:
                entry["r"] = None
            new_state[rel] = entry
        self.state = new_state
        self._save_state()


# ---------------------------------------------------------------------------
# 配置与 CLI
# ---------------------------------------------------------------------------
def _expand_env(value):
    if not isinstance(value, str):
        return value
    return re.sub(r"\$\{(\w+)\}", lambda m: os.environ.get(m.group(1), ""), value)


def load_config(path):
    with open(path, encoding="utf-8") as f:
        user = json.load(f)
    if not isinstance(user, dict):
        raise SyncError("配置文件必须是 JSON 对象")
    cfg = dict(DEFAULT_CONFIG)
    cfg.update(user)
    cfg["_config_path"] = os.path.abspath(path)
    # local_dir 留空或为 "." 时，默认跟随配置文件所在目录
    if not cfg.get("local_dir") or cfg["local_dir"] in (".", "./"):
        cfg["local_dir"] = os.path.dirname(cfg["_config_path"]) or "."
    env_name = cfg.get("password_env")
    if env_name and env_name in os.environ:
        cfg["password"] = os.environ[env_name]
    for key in ("url", "username", "password", "local_dir", "remote_dir"):
        cfg[key] = _expand_env(cfg.get(key) or "")
    if not cfg["url"]:
        raise SyncError("配置缺少 url")
    if not cfg["local_dir"]:
        raise SyncError("配置缺少 local_dir")
    if cfg.get("conflict_policy") not in CONFLICT_POLICIES:
        raise SyncError("conflict_policy 必须是: %s" % "/".join(CONFLICT_POLICIES))
    base = cfg["url"].rstrip("/")
    rd = (cfg.get("remote_dir") or "").strip("/")
    if rd:
        base += "/" + quote(rd, safe="/")
    cfg["_base_url"] = base + "/"
    return cfg


def _fmt_action(act):
    kind, rel = act[0], act[1]
    note = act[2] if len(act) > 2 else ""
    return "%s %s%s" % (LABEL.get(kind, kind), rel, ("  (%s)" % note) if note else "")


def _print_pending(actions, verbose):
    pending = [a for a in actions if a[0] != "ok"]
    if not pending:
        print("✓ 两端一致，无需同步")
        return 0
    print("待同步 %d 项:" % len(pending))
    for a in pending:
        print("  " + _fmt_action(a))
    return 0


def _run_hook(cmd, name, cwd=None):
    """执行用户自定义前/后置 shell 命令；失败抛 SyncError 中止同步。"""
    import subprocess
    print("▶ %s: %s" % (name, cmd))
    r = subprocess.run(cmd, shell=True, cwd=cwd)
    if r.returncode != 0:
        raise SyncError("%s 失败（退出码 %d）: %s" % (name, r.returncode, cmd))


def cmd_sync(cfg, dry_run=False, verbose=False, force=False):
    pre = (cfg.get("pre_command") or "").strip()
    if pre and not dry_run:
        _run_hook(pre, "前置命令", cwd=cfg.get("local_dir") or None)
    syncer = Syncer(cfg, verbose=verbose)
    print("本地: %s" % syncer.local_dir)
    print("远端: %s" % cfg["_base_url"])
    actions = syncer.plan()
    # -- 大规模删除保护 ----------------------------------------------------
    # 场景：对端目录被清空/重置（如服务器重建、测试目录被清理），而本地仍在。
    # 此时"对端删除"会被传播到本地，等于一次误删全部文件。此处按比例拦截。
    dels = [a for a in actions if a[0] in ("delete_local", "delete_remote")]
    tracked = len(syncer.state)
    suspicious = (len(dels) >= 2 and tracked > 0
                  and len(dels) >= max(2, tracked // 2))
    if dels and suspicious and not (force or cfg.get("allow_mass_delete")):
        msg = ("检测到大规模删除（%d 项，占已跟踪文件 %d 项的半数以上），"
               "疑似对端数据丢失。为保护本地数据已中止同步。\n"
               "  - 若确认要删除: 在配置中设 \"allow_mass_delete\": true，"
               "或运行时加 --force\n"
               "  - 若是误报(如更换了远端目录): 先核对 url/remote_dir 是否正确"
               % (len(dels), tracked))
        if dry_run:
            print("⚠ %s\n[模拟运行] 本次将删除:" % msg)
            for a in dels:
                print("  " + _fmt_action(a))
        else:
            raise SyncError(msg)
    pending = [a for a in actions if a[0] != "ok"]
    if not pending:
        print("✓ 两端一致，无需同步")
        return 0
    if dry_run:
        print("[模拟运行] 将执行 %d 项操作:" % len(pending))
        for a in pending:
            print("  " + _fmt_action(a))
        return 0
    print("开始同步，共 %d 项操作:" % len(pending))
    stats = syncer.execute(actions)
    summary = ", ".join("%s %d" % (LABEL[k], v) for k, v in stats.items() if v)
    print("完成: %s" % (summary or "无操作"))
    if syncer.failed:
        print("⚠ %d 项失败，状态已保留，下次运行自动重试" % len(syncer.failed), file=sys.stderr)
        return 1
    post = (cfg.get("post_command") or "").strip()
    if post and not dry_run:
        _run_hook(post, "后置命令", cwd=cfg.get("local_dir") or None)
    return 0


def cmd_status(cfg, verbose=False):
    syncer = Syncer(cfg, verbose=verbose)
    return _print_pending(syncer.plan(), verbose)


def cmd_init(path):
    if os.path.exists(path):
        print("配置文件已存在: %s" % path)
        return 1
    template = {
        "url": "https://dav.example.com/dav/",
        "username": "your_username",
        "password": "",
        "password_env": "DAVSYNC_PASSWORD",
        "local_dir": "./notes",
        "remote_dir": "notes",
        "ignore": DEFAULT_CONFIG["ignore"],
        "conflict_policy": "newer",
        "propagate_delete": True,
        "allow_mass_delete": False,
        "state_file": ".davsync.json",
        "timeout": 30,
        "pre_command": "",
        "post_command": "",
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(template, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("已生成示例配置 %s，编辑后执行: python davsync.py sync -c %s" % (path, path))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="davsync", description="DavSync —— 零依赖 WebDAV 双向同步器"
    )
    sub = parser.add_subparsers(dest="command")

    p_init = sub.add_parser("init", help="生成示例配置文件")
    p_init.add_argument("path", nargs="?", default="davsync.json")

    for name, help_text in (("sync", "执行双向同步"), ("status", "查看待同步项")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("-c", "--config", default="davsync.json", help="配置文件路径")
        p.add_argument("--dry-run", action="store_true", help="只显示将执行的操作")
        p.add_argument("-v", "--verbose", action="store_true", help="显示全部文件(含跳过项)")
        p.add_argument("--force", action="store_true",
                       help="越过大规模删除保护强制执行")

    args = parser.parse_args(argv)
    if not args.command:
        parser.print_help()
        return 2
    try:
        if args.command == "init":
            return cmd_init(args.path)
        cfg = load_config(args.config)
        if args.command == "status":
            return cmd_status(cfg, verbose=args.verbose)
        return cmd_sync(cfg, dry_run=args.dry_run, verbose=args.verbose,
                        force=getattr(args, "force", False))
    except SyncError as e:
        print("错误: %s" % e, file=sys.stderr)
        return 1
    except FileNotFoundError as e:
        print("错误: 文件不存在 %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

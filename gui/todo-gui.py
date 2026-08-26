#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
cpp-todo GUI —— 把 Web 界面包装为原生桌面窗口（pywebview / macOS WKWebView）

用法:
    python gui/todo-gui.py [--port 8931] [--db PATH] [--debug] [--timeout N]

行为:
    1. 若目标端口未被占用，自动启动 build/todo serve 作为后端子进程；
       若已有服务在运行（如浏览器版已启动），则直接复用，不重复拉起。
    2. 打开原生窗口加载 http://127.0.0.1:PORT/（完整保留 Web 界面全部功能）。
    3. 窗口关闭时自动结束由本进程拉起的后端；外部已有服务不受影响。

--timeout N 仅供自动化测试使用：N 秒后自动销毁窗口并退出。
"""
import argparse
import os
import socket
import subprocess
import sys
import time
import webbrowser

import webview

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TODO_BIN = os.path.join(ROOT, "build", "todo")
HOST = "127.0.0.1"
DEFAULT_PORT = 8931
WAIT_READY = 8.0  # 后端就绪等待上限（秒）
ICNS = os.path.join(os.path.dirname(__file__), "icons", "icon.icns")
DEVNULL = getattr(subprocess, "DEVNULL", open(os.devnull, "wb"))


def port_open(port, host=HOST):
    """探测端口是否已被占用"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.4)
        return s.connect_ex((host, port)) == 0


def apply_dock_icon(icns_path):
    """设置 macOS Dock 图标（仅在 pyobjc/AppKit 可用时生效）。"""
    if not sys.platform == "darwin" or not os.path.exists(icns_path):
        return
    try:
        import AppKit
        from Foundation import NSImage
        img = NSImage.alloc().initWithContentsOfFile_(icns_path)
        if img is not None:
            AppKit.NSApplication.sharedApplication().setApplicationIconImage_(img)
    except Exception as e:
        print(f"[gui] 设置 Dock 图标失败: {e}")


class Backend:
    """管理 build/todo serve 子进程的生命周期"""

    def __init__(self, port, db):
        self.port = port
        self.db = db
        self.proc = None
        self.owns = False

    def start(self):
        if port_open(self.port):
            print(f"[gui] 端口 {self.port} 已有服务在运行，直接复用")
            return
        if not os.path.exists(TODO_BIN):
            sys.exit(f"[gui] 未找到后端二进制: {TODO_BIN}\n     请先执行 ./build.sh 构建")
        cmd = [TODO_BIN, "serve", "--port", str(self.port)]
        if self.db:
            cmd += ["--db", self.db]
        print("[gui] 启动后端:", " ".join(cmd))
        self.proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        self.owns = True
        deadline = time.time() + WAIT_READY
        while time.time() < deadline:
            if port_open(self.port):
                print(f"[gui] 后端就绪: http://{HOST}:{self.port}/")
                return
            if self.proc.poll() is not None:
                sys.exit(f"[gui] 后端进程异常退出 (code={self.proc.returncode})")
            time.sleep(0.2)
        sys.exit("[gui] 后端启动超时，请检查 build/todo serve 是否可用")

    def stop(self):
        if self.proc and self.owns and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
            print("[gui] 后端已停止")


def main():
    ap = argparse.ArgumentParser(description="cpp-todo 桌面 GUI")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help=f"后端端口（默认 {DEFAULT_PORT}）")
    ap.add_argument("--db", default=None,
                    help="数据库路径（默认遵循后端规则：--db > ~/.cpp-todo.conf > ./data/todo.db）")
    ap.add_argument("--debug", action="store_true", help="显示开发者工具")
    ap.add_argument("--timeout", type=float, default=None,
                    help="（测试用）N 秒后自动关闭窗口并退出")
    args = ap.parse_args()

    backend = Backend(args.port, args.db)
    backend.start()

    url = f"http://{HOST}:{args.port}/"
    apply_dock_icon(ICNS)
    window = webview.create_window(
        "cpp-todo · 本机待办",
        url,
        width=1280,
        height=840,
        min_size=(960, 620),
        resizable=True,
        text_select=True,
    )

    def _auto_close():
        if args.timeout:
            time.sleep(args.timeout)
            try:
                # 渲染探针：确认页面真实加载（同步 DOM 检查，避免 Promise 序列化差异）
                info = window.evaluate_js("""(() => {
                  const c = document.getElementById('content');
                  const err = document.querySelector('.error, .toast, .banner');
                  const active = document.querySelector('.nav-item.active');
                  return JSON.stringify({
                    title: document.title,
                    contentExists: !!c,
                    contentText: c ? c.textContent.slice(0, 120) : null,
                    navItems: document.querySelectorAll('.nav-item').length,
                    activeView: active ? active.dataset.view : null,
                    errorEl: err ? err.textContent.slice(0, 80) : null
                  });
                })()""")
                print(f"[gui] 渲染探针: {info}")
            except Exception as e:
                print(f"[gui] 渲染验证失败: {e}")
            try:
                window.destroy()
            except Exception:
                pass

    try:
        # 原生应用菜单（macOS 菜单栏 / Windows 窗口菜单）
        menu = None
        try:
            menu = [
                webview.menu.Menu("文件", [
                    webview.menu.MenuAction("在浏览器中打开",
                                            lambda: webbrowser.open(url)),
                    webview.menu.MenuSeparator(),
                    webview.menu.MenuAction("退出", lambda: window.destroy()),
                ]),
                webview.menu.Menu("帮助", [
                    webview.menu.MenuAction("关于 cpp-todo", lambda: window.evaluate_js(
                        "alert('cpp-todo · 本机待办\\n本地 C++ 待办应用 · GUI 桌面版')")),
                ]),
            ]
        except Exception as e:
            print(f"[gui] 菜单初始化跳过: {e}")
        webview.start(_auto_close if args.timeout else None,
                      debug=args.debug, menu=menu)
    finally:
        backend.stop()


if __name__ == "__main__":
    main()

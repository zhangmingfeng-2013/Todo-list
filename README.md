# cpp-todo — 本机 C++ 待办应用

零账号 · 零安装 · 全数据本机存储。
用 C++17 + 内嵌 SQLite 实现的完整待办工具，提供 Web 界面与功能齐全的 CLI。编译后输出单个可执行文件，所有运行时依赖均已内置。

## 核心特性

- 标题、Markdown 备注、起止/截止日期、时间提醒、优先级、标签、项目
- 农历提醒（1900–2100）、节气与法定假日支持（需维护节假日表）
- 无限层级子任务、任务依赖（自动成环检测）、重复任务（支持跳过周末/节假日）
- Web 多视图：今日面板、项目树、标签筛选、日历（月/周）、周视图、看板、甘特图、统计仪表盘
- 自然语言快速录入、任务模板、批量操作、撤销系统（最近 200 步）、番茄钟与专注模式
- 本地存储（SQLite WAL）、自动/手动备份、导入/导出（Todo.txt/JSON/CSV/Todoist）

## 快速开始

1. 构建

```bash
./build.sh
```

需要：cmake、C++17 编译器（macOS 自带 clang / Linux gcc）。构建产物：`build/todo`，Web 资源位于 `build/web/`。

2. 启动 Web 界面

```bash
./build/todo serve           # 启动服务（默认 http://127.0.0.1:8931/）
./build/todo open            # 启动并自动打开浏览器
./build/todo serve --port 9000 --db ~/mytodo.db
```

3. 常用 CLI 示例

```bash
./build/todo add "写周报" --due 2026-08-28 --prio high --project 工作 --tag 汇报
./build/todo list --today
./build/todo done 3
./build/todo tree
./build/todo export --format csv
./build/todo backup now
```

完整命令见仓库内 CLI 帮助。

## 桌面 GUI（桌面窗口版）

Web 界面可一键包装为原生桌面窗口（macOS 使用系统 WKWebView，无需打包浏览器内核）。

### 方式 A：原生 .app Bundle（推荐）

打包为标准 macOS 应用，Dock 显示专属图标、Launchpad 识别、支持 `open` 命令启动。

```bash
./gui/build_app.sh                 # 构建 cpp-todo.app Bundle（首次需执行）
open ./cpp-todo.app                # 启动（建议将 cpp-todo.app 拖到 /Applications）
```

- Bundle 已 Ad-hoc 签名，可双击启动而不会被 Gatekeeper 拦截
- 启动器脚本路径：`gui/macos-bundle/launcher.sh`（自动从 bundle 路径反推项目根与 venv）
- Bundle 元数据：`gui/macos-bundle/Info.plist`
- `cpp-todo.app` 是构建产物，已加入 `.gitignore`

### 方式 B：直接 Python 启动（开发调试）

```bash
./run-gui.sh                       # 一键启动桌面窗口
./run-gui.sh --port 9000           # 指定端口
./run-gui.sh --db ~/mytodo.db
```

- 首次运行自动创建 `.venv` 并安装 pywebview（仅需一次，之后直接启动）
- 后端自动管理：端口空闲则拉起 `build/todo serve`，已有服务则直接复用；关闭窗口时自动停止由本进程拉起的后端
- 原生菜单：文件 → 在浏览器中打开 / 退出；帮助 → 关于
- macOS Dock 图标已内置（`gui/icons/icon.icns`），`gui/make_icon.py` 可重新生成
- 依赖：Python 3.9+（macOS 自带 python3 即可）
- 启动器源码：`gui/todo-gui.py`

> **关于 Dock 图标**：仅直接 Python 启动时（方式 B），Dock 图标**可能不会**显示——因为 macOS Dock 图标主要来自 .app Bundle 的 `Info.plist`。如希望 Dock 中显示 cpp-todo 图标，请使用方式 A 的 .app Bundle。

## 存储与同步

- 默认数据库：`./data/todo.db`（可用 `--db PATH` 指定）
- 配置持久化：`~/.cpp-todo.conf`（记录默认库位置）
- 备份：启动时每日自动备份到 `backups/`，支持手动备份与恢复
- 导入/导出：支持 JSON/CSV/Todo.txt/Todoist

## 架构概览

主要源码目录：

```
src/
├── main.cpp        # CLI 入口 + 服务启动
├── api.cpp/.hpp    # REST API
├── http.cpp/.hpp   # 简易 HTTP 静态文件与 API 服务
├── db.cpp/.hpp     # SQLite 封装与 Schema
├── lunar.cpp/.hpp  # 农历算法
├── recurrence.cpp  # 重复任务引擎
├── importer.cpp    # 导入器
└── web/            # 前端单页应用资源
```

依赖策略：SQLite amalgamation 与少量单文件库随仓库分发，目标是“编译后单个二进制可运行”。

## 已知限制

- 农历转换覆盖 1900–2100；超出范围无法转换。
- 重复任务“跳过节假日”依赖 `holidays` 表的准确性（需手动维护或生成）。

## 贡献与许可

欢迎 Issue / PR。遵循常规 Git 流程：fork → branch → PR。项目采用 MIT 许可证（详见 LICENSE）。


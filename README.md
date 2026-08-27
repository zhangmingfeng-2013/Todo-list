# cpp-todo — 本机 C++ 待办应用

零账号 · 零安装 · 数据不出本机。

用 C++17 + 内嵌 SQLite 实现的完整待办工具：**单个可执行文件**、CLI 与 Web 双前端、可选桌面窗口。服务仅监听 `127.0.0.1`，所有数据存储在本机，无云端、无遥测。

## 功能总览

| 领域     | 能力                                                                              |
|--------|---------------------------------------------------------------------------------|
| 任务建模   | 标题、Markdown 备注、起止/截止日期、时间提醒、三级优先级、标签、项目                                         |
| 任务结构   | 无限层级子任务、任务依赖（自动成环检测）                                                            |
| 重复与日历  | 重复任务（每日/周/月/自定义，可跳过周末与节假日）、农历提醒（1900–2100）、节气与法定假日表（`holiday auto` 自动生成）        |
| Web 视图 | 今日面板、项目树、标签、日历（月/周）、时间块日视图、看板、甘特图、统计仪表盘、热力图、回收站（30 天）                           |
| 效率工具   | 自然语言快速录入（`明天下午3点 写周报 #工作 !高`）、命令面板（⌘K / Ctrl+K）、任务模板、批量操作、撤销（最近 200 步）、番茄钟、深色主题 |
| 数据管理   | SQLite WAL 存储、每日自动备份、多格式导入导出、存储位置迁移（U 盘）、WebDAV 同步                              |

## 快速开始

### 1. 构建

```bash
./build.sh
```

需要：cmake、C++17 编译器（macOS 自带 clang / Linux gcc）。
构建产物：`build/todo`（Web 资源自动复制到 `build/web/`，任意目录均可启动）。

> 基础功能（CLI / Web）到此即可用。**可选的本地 AI 助手**需要额外准备模型权重并启动 AI 服务栈，见下文「AI 助手」章节。`build/` 与模型权重不入库（见 `.gitignore`）。

### 2. 启动 Web 界面

```bash
./build/todo serve                # http://127.0.0.1:8931/
./build/todo open                 # 启动并自动打开浏览器
./build/todo serve --port 9000 --db ~/mytodo.db
```

### 3. CLI 示例

```bash
./build/todo add "写周报" --due 2026-08-28 --prio high --project 工作 --tag 汇报
./build/todo list --today
./build/todo done 3               # 重复任务完成后自动生成下一次
./build/todo tree                 # 任务树（子任务缩进）
./build/todo stats                # 完成趋势 / 逾期 / 番茄钟统计
```

## 三种使用方式

### CLI 命令速查

| 分组   | 命令                                                                                                                                                                                             |
|------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 服务   | `serve [--port N] [--db PATH] [--open]` · `open`                                                                                                                                               |
| 任务   | `add` · `list/ls`（`--today` `--project` `--tag` `--status` `--due N天内`）· `tree` · `done <id>` · `undo <id>` · `rm <id> [--purge]` · `restore <id>` · `trash [clear]` · `dep/undep <id> <依赖id>` |
| 组织   | `projects` · `tags` · `calendar [--month YYYY-MM]` · `stats` · `digest`（每日摘要，含农历/节气）                                                                                                           |
| 导入导出 | `import <文件> [--format todotxt\|todoist\|ticktick\|csv]` · `export [--format todotxt\|json\|csv] [--out 文件]`                                                                                   |
| 备份   | `backup now\|list\|restore <文件>`（`serve` 模式每日自动备份）                                                                                                                                             |
| 节假日  | `holiday add/rm YYYY-MM-DD` · `holiday list` · `holiday auto [--year N]`                                                                                                                       |
| 存储   | `storage` · `storage list`（U 盘标记 ◈）· `storage move <路径>` · `storage reset`                                                                                                                     |

全局选项 `--db PATH`；数据库位置优先级：`--db` > `~/.cpp-todo.conf` > `./data/todo.db`。完整参数见 `./build/todo help`。

### Web 界面

原生 HTML/CSS/JS 单页应用（无框架、无构建步骤）。除上表视图外，还包括：保存的筛选器、自然语言快速录入、任务搜索、导入（滴答 / Todoist / Todo.txt）、导出、主题切换、存储位置管理。

### 桌面 GUI（macOS）

Web 界面一键包装为原生桌面窗口（系统 WKWebView，无需打包浏览器内核）：

```bash
./gui/macos-bundle/build_app.sh   # 方式 A：构建 cpp-todo.app Bundle（推荐，Dock 显示图标，已 Ad-hoc 签名）
./run-gui.sh             # 方式 B：直接 Python 启动（开发调试，首次自动创建 .venv 并安装 pywebview）
```

依赖 Python 3.10+（仅方式 B）；后端端口被占用时自动复用已有服务。详见 `gui/`。

## 数据与同步

### 存储与备份

- 数据库默认在 `./data/todo.db`，可通过 `--db` 或存储位置管理指定（支持迁移到 U 盘，配置持久化于 `~/.cpp-todo.conf`）
- 启动 `serve` 时每日自动备份到 `backups/`，支持手动备份与从备份恢复
- 导入：滴答 / Todoist / Todo.txt / CSV；导出：Todo.txt / JSON / CSV，另含全量备份快照格式

### WebDAV 同步（可选）

两种方式将数据同步到自建 WebDAV 服务器（坚果云、Nextcloud 等）：

1. **界面内置**：在设置中配置 WebDAV 并一键同步。服务端原生实现，同步前自动执行 WAL checkpoint，保证上传的主库文件是完整快照。
2. **独立同步器 `webdav-sync/davsync.py`**：零第三方依赖（纯 Python 标准库），本地目录与 WebDAV 双向同步，基于三方比对（本地 / 上次快照 / 远端）判定上传、下载、冲突与删除传播，内置大规模删除保护与 `--dry-run`。

同步策略、冲突处理与 SQLite WAL 注意事项详见 [`webdav-sync/README.md`](webdav-sync/README.md)。

## REST API

`serve` 模式在 `127.0.0.1` 提供 JSON API，可脚本化集成：

| 分组   | 端点                                                                                                                                                |
|------|---------------------------------------------------------------------------------------------------------------------------------------------------|
| 任务   | `GET/POST /api/tasks` · `GET/PUT/DELETE /api/tasks/{id}` · `POST /api/tasks/batch`（批量）· `POST /api/tasks/reorder`                                 |
| 视图聚合 | `/api/today` · `/api/calendar` · `/api/kanban` · `/api/day` · `/api/tree` · `/api/stats` · `/api/heatmap` · `/api/digest`                         |
| 组织实体 | `GET/POST/PUT/DELETE /api/projects` · `/api/tags` · `/api/filters` · `/api/templates`                                                             |
| 数据   | `POST /api/import` · `GET /api/export` · `GET/POST /api/backups` · `GET/DELETE /api/trash` · `GET /api/storage`（`/volumes`、`/move`）               |
| 效率   | `POST /api/quick-add` · `GET /api/search` · `GET/POST /api/undo` · `POST /api/repeat-preview` · `GET/POST/DELETE /api/holidays`（`/holidays/auto`） |
| 同步   | `POST /api/sync`（快照合并导入）· `GET/PUT /api/webdav-config` · `POST /api/webdav-sync`                                                                  |

示例：

```bash
curl --noproxy '*' -X POST http://127.0.0.1:8931/api/tasks \
  -H 'Content-Type: application/json' \
  -d '{"title":"写周报","dueDate":"2026-08-28","priority":2}'
```

## AI 助手（本地离线，可选）

侧栏「AI 助手」提供 4 个本地智能能力，全部在您本机推理，**不联网、不上传任何数据**：

| 能力 | 说明 |
|------|------|
| 任务拆解器 | 输入一个目标，拆成带依赖顺序与预估工时的步骤，可一键添加为子任务 |
| 上下文智能补全 | 粘贴任意文本，自动抽取其中的待办条目 |
| 动态优先级推演 | 读取当前任务池，给出建议执行顺序、可延后项与可舍弃项 |
| 任务预判生成 | 根据近期事件（会议 / 截止日）衍生应提前准备的待办 |

### 架构（三个独立进程）

请求链路：`Web(:8931) ← C++ libcurl 网关 ← AI 服务(:8777) ← 模型服务(:8080)`

- **模型服务** `mlx_lm.server`：加载自训融合模型（`ai/train/fused/`），提供 OpenAI 兼容接口（基于 Apple MLX，原生 macOS）。
- **AI 服务** `ai/app.py`：4 个特征路由 + 容错 JSON 抽取，零第三方依赖（仅 Python 标准库）。
- **主服务** `./build/todo serve`：内嵌 libcurl 网关，将 `/api/ai/*` 转发到 AI 服务。

### 启动

```bash
./ai/start_ai_backend.sh start   # 启动模型服务 + AI 服务
./build/todo serve               # 启动主服务，浏览器访问 http://127.0.0.1:8931/ ，点侧栏「AI 助手」
./ai/start_ai_backend.sh stop    # 停止 AI 后端栈
```

> 脚本用 `nohup` + `disown` 管理进程（macOS 无 `setsid`）；长生命周期服务建议在可托管的环境内运行，避免被 shell 回收。

### 本地准备（模型权重不入库）

仓库**不含**模型权重与构建产物。克隆后若要启用 AI 助手，需本地准备：

1. **Python 环境**：`cd ai/train && python -m venv venv && ./venv/bin/pip install mlx-lm`
2. **基座模型**：首次运行会经 HuggingFace 镜像自动拉取 `mlx-community/Qwen2.5-1.5B-Instruct-4bit`（建议设 `HF_ENDPOINT=https://hf-mirror.com` 加速），存于 `ai/train/base_model/`。
3. **自训融合模型**：依次运行 `ai/train/synthesize.py` → `ai/train/train_lora.py`（内部含 `mlx_lm lora` 与 `mlx_lm fuse`），生成 `ai/train/fused/`。脚本与参数见 `ai/train/`。
4. **构建主服务**：`./build.sh`

> 若未准备模型权重，前端「AI 助手」会提示 `LLM connection failed`——这是预期行为，其余功能不受影响。

## 架构

```
├── src/                    # C++ 后端（单二进制）
│   ├── main.cpp            # CLI 入口与命令分发、serve 启动
│   ├── api.cpp/.hpp        # REST API（30+ 端点）
│   ├── http.cpp/.hpp       # HTTP 服务器（POSIX 线程，每连接一线程）
│   ├── db.cpp/.hpp         # SQLite 封装与 Schema（WAL 模式）
│   ├── storage.cpp/.hpp    # 存储位置管理（~/.cpp-todo.conf）
│   ├── backup.cpp/.hpp     # 自动 / 手动备份
│   ├── lunar.cpp/.hpp      # 农历 / 节气算法（1900–2100）
│   ├── recurrence.cpp/.hpp # 重复任务引擎
│   ├── importer.cpp/.hpp   # 滴答 / Todoist / Todo.txt / CSV 导入
│   ├── exporter.cpp/.hpp   # Todo.txt / JSON / CSV 导出
│   └── json.hpp            # 自研最小 JSON 库
├── web/                    # 前端 SPA（原生 HTML/CSS/JS + 本地化 marked.min.js）
├── vendor/                 # SQLite amalgamation 等单文件依赖
├── ai/                     # 本地 AI 服务与自训工具链（模型权重不入库，见「AI 助手」）
├── webdav-sync/            # 零依赖 Python WebDAV 双向同步器（可选）
└── gui/                    # pywebview 桌面窗口与 .app Bundle 打包（可选）
```

> `build/`（编译产物）、`ai/train/base_model/`、`ai/train/fused/`、`ai/train/adapters/`（模型权重）均为生成物，已被 `.gitignore` 排除，不随仓库分发。

依赖策略：SQLite amalgamation 与单文件库随仓库分发，目标是"编译后单个二进制可运行"，除编译工具链外零外部依赖。

## 已知限制

- 农历转换覆盖 1900–2100，超出范围无法转换。
- 重复任务"跳过节假日"依赖 `holidays` 表的准确性（可用 `holiday auto` 生成）。
- WebDAV 同步为文件级整库快照，双机同时写入建议使用保留双方的冲突策略（详见 `webdav-sync/README.md`）。

## 贡献与许可

欢迎 Issue / PR：fork → branch → PR。项目采用 MIT 许可证（详见 [LICENSE](LICENSE)）。

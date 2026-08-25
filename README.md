# cpp-todo — 本机 C++ 待办应用

零账号 · 零安装 · 数据仅存本机。一个用 **C++17 + 内嵌 SQLite** 构建的完整待办工具，同时提供 **Web 界面**（六大视图）与 **CLI**（35+ 子命令）双入口。所有依赖（SQLite、JSON 解析、HTTP 服务器、农历算法、Markdown 渲染）全部内嵌，**不需要安装任何东西**，编译出的单个二进制即可运行。

---

## 特性一览

### 基础任务字段
| 能力        | 说明                                                 |
|-----------|----------------------------------------------------|
| 标题 / 备注   | 备注为 **Markdown** 富文本（Web 端实时渲染）                    |
| 截止 / 开始日期 | 可选，逾期自动标红                                          |
| 时间提醒      | 可选 `HH:MM`                                         |
| **农历提醒**  | 按农历每年提醒，如 `5-5` 端午、`8-15` 中秋（1900–2100 全范围算法）      |
| 优先级       | 高 / 中 / 低                                          |
| **多级子任务** | 无限嵌套，树形展示                                          |
| 多标签 / 多项目 | 项目支持文件夹嵌套，任务可归属项目并打多个标签                            |
| **任务依赖**  | A 依赖 B，B 未完成时 A 显示 ⛔ 阻塞；添加依赖自动成环检测                 |
| **重复任务**  | 按天 / 周 / 月 / 年 / 自定义周期，可**跳过周末 / 节假日**，完成自动生成下一次实例 |

### 组织视图（Web）
- **今日面板**：已逾期 / 今日到期 / 今日开始 / 无日期 / 今日已完成
- **项目视图**：文件夹式项目树
- **标签筛选**：按标签聚合任务
- **日历视图**：月历 + **农历标注** + 节假日标记 + 重复实例展开 + 农历提醒落点
- **看板视图**：待办 / 进行中 / 已完成三列
- **保存筛选**：如「#学习 + 高优先级 + 7 天内到期」，一键执行

### 存储与同步
- 数据存本机 **SQLite**（WAL 模式，默认 `data/todo.db`）
- **U盘 / 可移动盘存储**：Web 设置里一键迁移数据库到 U盘；CLI 提供 `todo storage move <卷路径>`
- **默认路径持久化**：`~/.cpp-todo.conf` 记录默认库位置，无 `--db` 时自动使用
- **导入**：滴答清单（JSON/CSV）、Todoist（JSON）、Todo.txt
- **零账号**：无任何云端依赖，纯本地 `127.0.0.1` 服务

---

## 快速开始

### 构建（只需一次）

```bash
./build.sh
```

> 需要 `cmake` + `C++17` 编译器（macOS 自带 clang / Linux gcc）。构建产物为单个可执行文件 `build/todo`，Web 资源自动拷贝到 `build/web/`。

### 启动 Web 界面

```bash
./build/todo open          # 启动本地服务并自动打开浏览器
./build/todo serve         # 仅启动服务，访问 http://127.0.0.1:8931/
./build/todo serve --port 9000 --db ~/mytodo.db
```

### CLI 快速上手

```bash
# 添加任务
./build/todo add "写周报" --due 2026-08-28 --prio high --project 工作 --tag 汇报
./build/todo add "晨跑 5km" --repeat weekly --due 2026-08-25 --time 07:00
./build/todo add "中秋买月饼" --lunar 8-15 --project 生活
./build/todo add "学习模板元编程" --parent 5            # 子任务（无限嵌套）

# 查看
./build/todo list                    # 全部任务
./build/todo list --today            # 今日
./build/todo tree                    # 任务树（缩进显示子任务）
./build/todo calendar                # 本月日历（含农历）
./build/todo projects                # 项目列表

# 操作
./build/todo done 3                  # 完成（重复任务自动生成下一次）
./build/todo undo 3                  # 恢复
./build/todo dep 2 1                 # 任务2 依赖任务1
./build/todo undep 2 1               # 移除依赖

# 导入
./build/todo import 滴答导出.json --format ticktick
./build/todo import todoist.json --format todoist
./build/todo import todo.txt --format todotxt

# 节假日（重复任务跳过）
./build/todo holiday add 2026-10-01
./build/todo holiday list
```

---

## Web 界面

启动 `serve` 后浏览器打开 `http://127.0.0.1:<port>/`。

- **侧边栏**：视图导航、项目树（文件夹可展开）、标签云、农历今日、存储位置（可迁移到 U盘）
- **顶栏**：全局搜索（标题 / 备注）、导入入口、新建任务
- **任务编辑器**（点击任务或新建）：标题、Markdown 备注（编辑/预览切换）、优先级、起止日期、时间提醒、农历提醒、项目、父任务、标签（可新建）、重复规则（频率 / 间隔 / 周几 / 跳过周末节假日 / 结束日期）、依赖管理、子任务

---

## CLI 全命令参考

```
服务:        serve [--port N] [--db PATH] [--open]   启动本地服务
             open [--port N]                         启动并打开浏览器
任务:        add / list / tree / done / undo / rm
依赖:        dep <id> <依赖id> / undep <id> <依赖id>
组织:        projects / tags / filter add|list|rm
导入:        import <文件> [--format todotxt|todoist|ticktick|csv]
节假日:      holiday add|list|rm YYYY-MM-DD
全局:        --db PATH   数据库位置（默认 ./data/todo.db）
```

---

## 架构

```
src/
├── main.cpp        CLI 入口（35+ 子命令）+ 服务启动
├── api.cpp/.hpp    REST API（20+ 路由：tasks/tree/today/calendar/kanban/…）
├── http.cpp/.hpp   极简 HTTP 服务器（POSIX socket，线程每连接，静态文件服务）
├── db.cpp/.hpp     SQLite RAII 封装 + Schema（含 WAL、外键、全局互斥）
├── lunar.cpp/.hpp  农历 ⇄ 公历互转（1900–2100 数据表 + 闰月，全范围往返验证）
├── recurrence.cpp/.hpp  重复任务引擎（RRULE 解析 + 实例展开 + 跳过节假日）
├── importer.cpp/.hpp    导入器（滴答 / Todoist / Todo.txt / CSV）
├── storage.cpp/.hpp   存储管理（卷枚举 + 数据库迁移 + 默认路径配置）
├── json.hpp        自研最小 JSON 库（零第三方依赖）
vendor/sqlite3.*    SQLite amalgamation（编译进二进制，零安装）
web/                Web 单页应用（index.html / app.js / style.css / marked.min.js）
```

**依赖策略**：SQLite amalgamation、自研 JSON、自研 HTTP、单头文件农历表、本地化 marked.js —— 全部随仓库分发，编译后单个二进制即完整应用，无任何系统包依赖。

---

## 数据

- 默认数据库：`./data/todo.db`（可用 `--db PATH` 临时指定其他路径）
- 默认路径持久化：`~/.cpp-todo.conf` 记录常用库位置，优先级 `--db > 配置 > ./data/todo.db`
- WAL 模式，外键约束开启，写操作串行化（单进程本地使用）
- 删除数据库文件即清空全部数据（迁移前请先备份）

---

## 已知限制

- 服务绑定 `127.0.0.1`，仅本机可访问（无网络暴露）
- 重复任务跳过节假日依赖 `holidays` 表（需自行维护节假日日期）
- 农历数据表覆盖 1900–2100 年，超出范围无法转换

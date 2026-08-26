# DavSync —— 零依赖 WebDAV 双向同步器

用 **Python 标准库**（无任何第三方依赖）实现本地目录与 WebDAV 服务器的双向同步。
单文件 `davsync.py`，复制即用，Python 3.8+。

## 快速开始

```bash
# 1. 生成示例配置并编辑
python davsync.py init            # 生成 davsync.json

# 2. 查看待同步项（只读，不修改任何东西）
python davsync.py status -c davsync.json

# 3. 执行双向同步
python davsync.py sync -c davsync.json

# 模拟运行：只打印将执行的操作
python davsync.py sync -c davsync.json --dry-run
```

推荐用环境变量传密码，避免明文写在配置里：

```bash
export DAVSYNC_PASSWORD='****'
python davsync.py sync -c davsync.json
```

## 同步算法（三方比对）

每次运行对每个文件比对三方状态：

```
本地当前  vs  上次同步快照(.davsync.json)  vs  远端当前(PROPFIND)
```

| 本地 | 远端 | 判定 |
|---|---|---|
| 已修改 | 未变 | 上传 |
| 未变 | 已修改 | 下载 |
| 已修改 | 已修改 | **冲突**，按 `conflict_policy` 处理 |
| 已删除 | 未变 | 按 `propagate_delete` 删除远端（或重新上传恢复） |
| 未变 | 已删除 | 按 `propagate_delete` 删除本地（或重新下载恢复） |
| 新增 | 不存在 | 上传 |
| 不存在 | 新增 | 下载 |
| 双方都删除 | — | 清理同步记录 |

首次同步时若两端存在同名且大小相同、修改时间接近（≤2s）的文件，直接识别为同一文件，不触发冲突。

## 配置项（davsync.json）

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `url` | （必填） | WebDAV 服务器地址，如 `https://dav.example.com/dav/` |
| `username` / `password` | 空 | Basic 认证；留空则匿名访问 |
| `password_env` | 空 | 优先从该环境变量读取密码（推荐） |
| `local_dir` | （必填） | 本地目录；**留空或写 `.` 表示配置文件所在目录**（推荐配置放存储位置） |
| `remote_dir` | 空 | 远端子目录（相对 `url`） |
| `ignore` | 见示例 | 忽略规则，支持 Glob；无通配符的规则按路径段匹配任意层级（如 `.git` 会忽略任意深度的 .git 目录） |
| `conflict_policy` | `newer` | 冲突策略，见下表 |
| `propagate_delete` | `true` | 是否把一端的删除传播到另一端；`false` 时改为把文件恢复回来 |
| `allow_mass_delete` | `false` | 大规模删除保护开关，见下文「安全机制」 |
| `state_file` | `.davsync.json` | 同步状态文件路径（相对 `local_dir`）；状态文件本身永不参与同步 |
| `timeout` | `30` | HTTP 超时（秒） |
| `pre_command` | 空 | 同步前执行的 shell 命令；失败则中止同步（`--dry-run` 时跳过） |
| `post_command` | 空 | 同步全部成功后执行的 shell 命令 |

配置中的 `url` / `username` / `password` 支持 `${ENV_VAR}` 展开。

### 冲突策略（conflict_policy）

| 策略 | 行为 |
|---|---|
| `newer` | 修改时间较新的一端覆盖另一端（默认） |
| `local` | 本地覆盖远端 |
| `remote` | 远端覆盖本地 |
| `both` | 两份都保留：本地版本上传，远端版本另存为 `文件名.conflict-时间戳.扩展名` |
| `error` | 报告冲突并跳过，不修改任何一端，下次运行重新检测 |

## 本地测试（无需真实服务器）

仓库自带一个最小 WebDAV 测试服务器 `tests/dav_server.py`（同样零依赖）：

```bash
mkdir -p /tmp/davtest/{local,remote}
python tests/dav_server.py /tmp/davtest/remote 8765 &
# 配置 url 指向 http://127.0.0.1:8765/ 即可完整验证双向同步
python tests/dav_server.py /tmp/davtest/remote 8765 --user alice --pass secret   # 带 Basic 认证
```

已验证场景：首次上传、双向增量合并、冲突（newer/both）、删除传播、忽略规则、dry-run、状态查询、大规模删除保护。

### 安全机制：大规模删除保护（allow_mass_delete）

真实事故驱动的设计：WebDAV 服务器目录被清空（服务器重建、`/tmp` 重启清理、误换
`remote_dir`）时，同步器会把「远端文件全部消失」理解为「远端全部删除」，在
`propagate_delete=true` 下会**把本地文件逐一删除**——曾导致正在使用的 SQLite 数据库被误删。

现在的防线：当本次计划的删除操作 ≥ 2 项且达到已跟踪文件数的一半以上时，同步直接中止
并给出说明。确属有意为之的大规模删除，可在配置中设 `"allow_mass_delete": true`，
或运行时加 `--force`。`--dry-run` 不受影响，仅输出警告与删除清单。

## 实战示例：同步 TODO-list 应用数据（SQLite + WAL）

配置文件直接放在应用的存储目录 `data/webdav.json` 里，**所有 WebDAV 参数
（url / 凭据 / 远端目录 / 冲突策略 / 忽略规则 / 钩子命令）都在存储位置内设置**：

```bash
python davsync.py sync -c /path/to/TODO-list/data/webdav.json
```

该模式的三项内建行为：

1. **`local_dir` 留空即同步配置文件所在目录**——配置跟着数据走，整个目录复制到
   任何机器都能用。
2. **配置文件自身自动排除，不会被上传**——即使里面写了明文凭据也不会泄漏到
   WebDAV 服务器。
3. **`pre_command` / `post_command` 在存储目录内执行**——命令可直接使用相对路径
   （如 `todo.db`）。

针对 SQLite WAL 模式的三个关键点：

1. **`pre_command` 先做 WAL checkpoint**：应用运行中主库文件不完整（最新数据在
   `-wal` 里），同步前执行 `PRAGMA wal_checkpoint(TRUNCATE)` 把 WAL 合并进主库，
   保证上传的 `todo.db` 是完整快照（实测 482KB WAL → 0）。
2. **`ignore` 排除 `*.db-wal` / `*.db-shm`**：这两个文件是运行时中间状态，
   同步它们没有意义且必然引发伪冲突。
3. **冲突策略对数据库慎用覆盖**：数据库文件是整库快照，`newer` 覆盖即丢失被覆盖端
   之后的全部修改。**推荐单机上传备份**场景；若双机使用，改用 `both` 策略
   （保留 `todo.conflict-*.db` 副本）以便人工合并，或干脆 `error`。

已实测：通过应用 API 新增任务（`POST /api/tasks`）→ 再次同步仅增量上传
`todo.db` → 远端副本中可查到该任务 → 第三次同步无操作。

## 已知限制

- 变更检测基于 `mtime + size + ETag` 启发式，不做内容哈希；远端服务器若不提供 `getlastmodified`/`getetag`，同尺寸修改可能漏检（坚果云、Nextcloud、Apache、IIS 均提供）。
- 仅支持 Basic 认证（配合 HTTPS 使用；明文 HTTP 仅建议本地/内网）。
- 上传整个文件读入内存，超大文件（>几百 MB）不建议。
- 不同步空目录、不同步文件权限/时间戳以外的元数据。
- 多台设备同时同步同一目录时，建议错开运行时间，或使用 `both`/`error` 策略防止覆盖。

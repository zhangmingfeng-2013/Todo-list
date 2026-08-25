"""数据库初始化与连接管理 — SQLite 本地存储，零配置。"""
import sqlite3
import os
import threading

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "todo.db")

_local = threading.local()

SCHEMA = """
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;

-- 项目（支持文件夹嵌套）
CREATE TABLE IF NOT EXISTS projects (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    parent_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    color TEXT DEFAULT '#4A90D9',
    is_folder INTEGER DEFAULT 0,
    sort_order INTEGER DEFAULT 0,
    created_at TEXT DEFAULT (datetime('now')),
    updated_at TEXT DEFAULT (datetime('now'))
);

-- 标签
CREATE TABLE IF NOT EXISTS tags (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    color TEXT DEFAULT '#8E8E93',
    created_at TEXT DEFAULT (datetime('now'))
);

-- 任务（核心表）
CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    notes TEXT DEFAULT '',
    priority INTEGER DEFAULT 1,            -- 0=低 1=中 2=高
    start_date TEXT,                        -- YYYY-MM-DD
    due_date TEXT,                          -- YYYY-MM-DD
    remind_time TEXT,                       -- HH:MM（可选时间提醒）
    lunar_remind INTEGER DEFAULT 0,         -- 1=按农历提醒
    lunar_date TEXT,                        -- 农历日期描述（如 "八月十五"）
    project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    parent_id INTEGER REFERENCES tasks(id) ON DELETE CASCADE,
    sort_order INTEGER DEFAULT 0,
    status INTEGER DEFAULT 0,               -- 0=待办 1=已完成 2=已归档
    completed_at TEXT,
    repeat_type TEXT,                       -- none|daily|weekly|monthly|yearly|custom
    repeat_interval INTEGER DEFAULT 1,
    repeat_weekdays TEXT,                   -- 逗号分隔的星期几，如 "1,3,5"
    repeat_end_date TEXT,
    skip_holidays INTEGER DEFAULT 0,        -- 1=跳过节假日
    created_at TEXT DEFAULT (datetime('now')),
    updated_at TEXT DEFAULT (datetime('now'))
);

-- 任务-标签多对多
CREATE TABLE IF NOT EXISTS task_tags (
    task_id INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    PRIMARY KEY (task_id, tag_id)
);

-- 任务依赖：depends_on 完成后 task_id 才能开始
CREATE TABLE IF NOT EXISTS task_dependencies (
    task_id INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    depends_on INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    PRIMARY KEY (task_id, depends_on),
    CHECK (task_id != depends_on)
);

-- 保存的筛选视图
CREATE TABLE IF NOT EXISTS saved_filters (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    filter_config TEXT NOT NULL,            -- JSON: {tags:[], priority:[], dueWithin:N, projectId:...}
    sort_order INTEGER DEFAULT 0,
    created_at TEXT DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_tasks_project ON tasks(project_id);
CREATE INDEX IF NOT EXISTS idx_tasks_parent ON tasks(parent_id);
CREATE INDEX IF NOT EXISTS idx_tasks_due ON tasks(due_date);
CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_task_tags_tag ON task_tags(tag_id);
CREATE INDEX IF NOT EXISTS idx_task_deps_dep ON task_dependencies(depends_on);
"""


def get_db() -> sqlite3.Connection:
    """获取当前线程的数据库连接（线程安全）。"""
    conn = getattr(_local, "conn", None)
    if conn is None:
        os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
        conn = sqlite3.connect(DB_PATH)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys = ON")
        _local.conn = conn
    return conn


def init_db():
    """初始化数据库 Schema（幂等）。"""
    conn = get_db()
    conn.executescript(SCHEMA)
    conn.commit()


def query(sql: str, params=()) -> list:
    """执行查询并返回字典列表。"""
    cur = get_db().execute(sql, params)
    return [dict(r) for r in cur.fetchall()]


def query_one(sql: str, params=()) -> dict | None:
    """查询单条记录。"""
    cur = get_db().execute(sql, params)
    r = cur.fetchone()
    return dict(r) if r else None


def execute(sql: str, params=()) -> int:
    """执行写操作并返回 lastrowid。"""
    conn = get_db()
    cur = conn.execute(sql, params)
    conn.commit()
    return cur.lastrowid


def executemany(sql: str, seq):
    conn = get_db()
    conn.executemany(sql, seq)
    conn.commit()

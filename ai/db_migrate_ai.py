#!/usr/bin/env python3
"""为 cpp-todo 的 SQLite 新增 AI 相关字段（P0 迁移）。

- 自动定位数据库：环境变量 AI_DB  >  ~/.cpp-todo.conf 的 db=  >  ./data/todo.db
- 迁移前做 WAL checkpoint 并备份
- 幂等：字段已存在则跳过
"""
import os
import shutil
import sqlite3
from datetime import datetime


def _read_kv(path):
    out = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or line.startswith("["):
                    continue
                if "=" not in line:
                    continue
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    except FileNotFoundError:
        pass
    return out

NEW_COLUMNS = [
    ("tasks", "estimated_minutes", "INTEGER"),
    ("tasks", "parent_id", "INTEGER"),
    ("tasks", "predicted_priority", "REAL"),
]


def find_db():
    if os.environ.get("AI_DB"):
        return os.environ["AI_DB"]
    conf = os.path.expanduser("~/.cpp-todo.conf")
    kv = _read_kv(conf)
    if "db" in kv:
        return kv["db"]
    default = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "data", "todo.db"))
    return default


def existing_columns(conn, table):
    return {row[1] for row in conn.execute(f"PRAGMA table_info({table})").fetchall()}


def main():
    db = find_db()
    if not os.path.exists(db):
        print(f"[migrate] 数据库不存在: {db}")
        raise SystemExit(1)

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup = f"{db}.ai_migrate_{ts}.bak"
    shutil.copy2(db, backup)
    print(f"[migrate] 备份 -> {backup}")

    conn = sqlite3.connect(db)
    conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    for table, col, ctype in NEW_COLUMNS:
        if col in existing_columns(conn, table):
            print(f"[migrate] 跳过 {table}.{col}（已存在）")
            continue
        conn.execute(f"ALTER TABLE {table} ADD COLUMN {col} {ctype}")
        print(f"[migrate] 新增 {table}.{col} ({ctype})")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_tasks_parent ON tasks(parent_id)")
    conn.commit()
    conn.close()
    print("[migrate] 完成。")


if __name__ == "__main__":
    main()

// db.cpp — SQLite 封装实现
#include "db.hpp"

#include <sqlite3.h>
#include <stdexcept>
#include <cstring>
#include <set>

// ---- Schema ----
const char* kSchemaSql = R"SQL(
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA busy_timeout = 5000;

CREATE TABLE IF NOT EXISTS projects (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    parent_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    color TEXT DEFAULT '#4A90D9',
    is_folder INTEGER DEFAULT 0,
    sort_order INTEGER DEFAULT 0,
    created_at TEXT DEFAULT (datetime('now','localtime')),
    updated_at TEXT DEFAULT (datetime('now','localtime'))
);

CREATE TABLE IF NOT EXISTS tags (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    color TEXT DEFAULT '#8E8E93',
    created_at TEXT DEFAULT (datetime('now','localtime'))
);

CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    notes TEXT DEFAULT '',
    priority INTEGER DEFAULT 1,          -- 0=低 1=中 2=高
    start_date TEXT,                     -- YYYY-MM-DD
    due_date TEXT,                       -- YYYY-MM-DD
    remind_time TEXT,                    -- HH:MM 可选时间提醒
    has_reminder INTEGER DEFAULT 0,
    lunar_remind INTEGER DEFAULT 0,      -- 1=按农历提醒
    lunar_date TEXT,                     -- "M-D"，如 "8-15"
    project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    parent_id INTEGER REFERENCES tasks(id) ON DELETE CASCADE,
    sort_order INTEGER DEFAULT 0,
    status TEXT DEFAULT 'todo',          -- todo|doing|done|archived
    completed_at TEXT,
    pomodoros INTEGER DEFAULT 0,         -- 已完成番茄钟数
    deleted_at TEXT,                     -- 软删除时间（回收站），NULL=正常
    repeat_rule TEXT DEFAULT '',         -- JSON: {freq,interval,weekdays,skipWeekends,skipHolidays,endDate}
    created_at TEXT DEFAULT (datetime('now','localtime')),
    updated_at TEXT DEFAULT (datetime('now','localtime'))
);

CREATE TABLE IF NOT EXISTS task_tags (
    task_id INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    tag_id  INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    PRIMARY KEY (task_id, tag_id)
);

CREATE TABLE IF NOT EXISTS task_dependencies (
    task_id INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    depends_on INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    PRIMARY KEY (task_id, depends_on),
    CHECK (task_id != depends_on)
);

CREATE TABLE IF NOT EXISTS saved_filters (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    spec TEXT NOT NULL,                  -- JSON 筛选条件
    sort_order INTEGER DEFAULT 0,
    created_at TEXT DEFAULT (datetime('now','localtime'))
);

CREATE TABLE IF NOT EXISTS holidays (
    date TEXT PRIMARY KEY                 -- YYYY-MM-DD 法定/自定义节假日
);

CREATE TABLE IF NOT EXISTS templates (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    body TEXT NOT NULL,                   -- JSON 任务字段（与 POST /api/tasks 一致，可含 dueOffsetDays/startOffsetDays）
    created_at TEXT DEFAULT (datetime('now','localtime'))
);

CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT
);

CREATE TABLE IF NOT EXISTS undo_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    action TEXT NOT NULL,               -- create|update|delete|purge|complete|reopen|restore
    task_id INTEGER,                    -- 被操作的任务 id（create 时为 0）
    after_id INTEGER DEFAULT 0,         -- 操作产生的新任务 id（create / complete 下一实例）
    label TEXT DEFAULT '',              -- 任务标题（用于提示）
    payload TEXT NOT NULL,              -- JSON：操作前的完整快照（含标签/依赖）
    created_at TEXT DEFAULT (datetime('now','localtime'))
);

CREATE INDEX IF NOT EXISTS idx_tasks_project ON tasks(project_id);
CREATE INDEX IF NOT EXISTS idx_tasks_parent  ON tasks(parent_id);
CREATE INDEX IF NOT EXISTS idx_tasks_due     ON tasks(due_date);
CREATE INDEX IF NOT EXISTS idx_tasks_start   ON tasks(start_date);
CREATE INDEX IF NOT EXISTS idx_tasks_status  ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_tasks_deleted ON tasks(deleted_at);
CREATE INDEX IF NOT EXISTS idx_task_tags_tag ON task_tags(tag_id);
CREATE INDEX IF NOT EXISTS idx_task_deps_dep ON task_dependencies(depends_on);
CREATE INDEX IF NOT EXISTS idx_undo_log_id ON undo_log(id);
)SQL";

std::recursive_mutex& Db::mutex() {
    static std::recursive_mutex m;
    return m;
}

bool Db::Row::has(const std::string& name) const {
    for (size_t i = 0; i < cols.size(); ++i)
        if (cols[i] == name) return true;
    return false;
}

std::string Db::Row::get(const std::string& name, const std::string& def) const {
    for (size_t i = 0; i < cols.size(); ++i)
        if (cols[i] == name) return i < vals.size() && vals[i] != "NULL" ? vals[i] : def;
    return def;
}

long long Db::Row::get_int(const std::string& name, long long def) const {
    std::string v = get(name);
    if (v.empty()) return def;
    try { return std::stoll(v); } catch (...) { return def; }
}

Db::Db(const std::string& path) : path_(path) {
    if (sqlite3_open(path.c_str(), reinterpret_cast<sqlite3**>(&db_)) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(static_cast<sqlite3*>(db_));
        db_ = nullptr;
        return;
    }
    sqlite3_exec(static_cast<sqlite3*>(db_), "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(static_cast<sqlite3*>(db_), "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
}

Db::~Db() {
    if (db_) sqlite3_close(static_cast<sqlite3*>(db_));
}

bool Db::checkpoint() {
    std::lock_guard<std::recursive_mutex> lk(mutex());
    if (!db_) return false;
    return sqlite3_exec(static_cast<sqlite3*>(db_),
                        "PRAGMA wal_checkpoint(TRUNCATE);",
                        nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool Db::reopen(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lk(mutex());
    if (db_) {
        sqlite3_close(static_cast<sqlite3*>(db_));
        db_ = nullptr;
    }
    path_ = path;
    last_error_.clear();
    if (sqlite3_open(path.c_str(), reinterpret_cast<sqlite3**>(&db_)) != SQLITE_OK) {
        last_error_ = db_ ? sqlite3_errmsg(static_cast<sqlite3*>(db_)) : "open failed";
        db_ = nullptr;
        return false;
    }
    sqlite3_exec(static_cast<sqlite3*>(db_), "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(static_cast<sqlite3*>(db_), "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
    return true;
}

bool Db::prepare(sqlite3_stmt** stmt, const std::string& sql,
                 const std::vector<std::string>& params) const {
    if (!db_) return false;
    if (sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql.c_str(), -1, stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(static_cast<sqlite3*>(db_));
        return false;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i] == "NULL") {
            sqlite3_bind_null(*stmt, static_cast<int>(i + 1));
        } else {
            sqlite3_bind_text(*stmt, static_cast<int>(i + 1), params[i].c_str(),
                              static_cast<int>(params[i].size()), SQLITE_TRANSIENT);
        }
    }
    return true;
}

void Db::exec(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lk(mutex());
    if (!db_) throw std::runtime_error("db not open: " + last_error_);
    char* err = nullptr;
    if (sqlite3_exec(static_cast<sqlite3*>(db_), sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + msg);
    }
}

std::vector<Db::Row> Db::query(const std::string& sql,
                               const std::vector<std::string>& params) {
    std::lock_guard<std::recursive_mutex> lk(mutex());
    std::vector<Row> rows;
    if (!db_) return rows;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare(&stmt, sql, params)) return rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row r;
        int n = sqlite3_column_count(stmt);
        for (int i = 0; i < n; ++i) {
            r.cols.push_back(sqlite3_column_name(stmt, i));
            const unsigned char* t = sqlite3_column_text(stmt, i);
            r.vals.push_back(t ? reinterpret_cast<const char*>(t) : "NULL");
        }
        rows.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::optional<Db::Row> Db::query_one(const std::string& sql,
                                     const std::vector<std::string>& params) {
    auto rows = query(sql, params);
    if (rows.empty()) return std::nullopt;
    return rows.front();
}

long long Db::last_insert_rowid() const {
    return db_ ? sqlite3_last_insert_rowid(static_cast<sqlite3*>(db_)) : 0;
}

int Db::changes() const {
    return db_ ? sqlite3_changes(static_cast<sqlite3*>(db_)) : 0;
}

void Db::begin() {
    std::lock_guard<std::recursive_mutex> lk(mutex());
    exec("BEGIN");
}
void Db::commit() {
    std::lock_guard<std::recursive_mutex> lk(mutex());
    exec("COMMIT");
}
void Db::rollback() {
    std::lock_guard<std::recursive_mutex> lk(mutex());
    exec("ROLLBACK");
}

// 旧库迁移：为已存在的 tasks 表补齐新增列（必须在 kSchemaSql 建索引之前执行，
// 否则 idx_tasks_deleted 会因旧库缺列而失败）
static void migrate_schema(Db& db) {
    std::set<std::string> cols;
    for (auto& r : db.query("PRAGMA table_info(tasks)"))
        cols.insert(r.get("name"));
    if (!cols.count("pomodoros"))
        db.exec("ALTER TABLE tasks ADD COLUMN pomodoros INTEGER DEFAULT 0");
    if (!cols.count("deleted_at"))
        db.exec("ALTER TABLE tasks ADD COLUMN deleted_at TEXT");
}

void init_schema(Db& db) {
    // 已有旧库 → 先迁移列，再跑完整 schema（含新索引）
    if (!db.query("SELECT name FROM sqlite_master WHERE type='table' AND name='tasks'")
             .empty())
        migrate_schema(db);
    db.exec(kSchemaSql);
}

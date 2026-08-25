// db.hpp — SQLite RAII 封装 + Schema 定义
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <mutex>

namespace sqlite {
struct sqlite3;
}
struct sqlite3_stmt;   // 与 sqlite3.h 的 typedef 同名同型

class Db {
public:
    explicit Db(const std::string& path);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    struct Row {
        std::vector<std::string> cols;
        std::vector<std::string> vals;
        bool has(const std::string& name) const;
        std::string get(const std::string& name, const std::string& def = "") const;
        long long get_int(const std::string& name, long long def = 0) const;
    };

    // 执行无返回 SQL
    void exec(const std::string& sql);
    // 参数化查询（? 占位符）
    std::vector<Row> query(const std::string& sql,
                           const std::vector<std::string>& params = {});
    std::optional<Row> query_one(const std::string& sql,
                                 const std::vector<std::string>& params = {});
    long long last_insert_rowid() const;
    int changes() const;

    void begin();
    void commit();
    void rollback();

    // 全局互斥：所有访问串行化（本工具单进程本地使用，足够安全）
    static std::recursive_mutex& mutex();

    bool ok() const { return db_ != nullptr; }
    const std::string& error() const { return last_error_; }
    const std::string& path() const { return path_; }

    // WAL 落盘（迁移/备份前调用，保证主文件完整）
    bool checkpoint();
    // 关闭当前库并在新路径重开（迁移用）。成功返回 true。
    bool reopen(const std::string& path);

private:
    void* db_ = nullptr;   // sqlite3*
    mutable std::string last_error_;
    std::string path_;

    bool prepare(sqlite3_stmt** stmt, const std::string& sql,
                 const std::vector<std::string>& params) const;
};

// 初始化数据库（建表 + 索引 + 默认设置），幂等
void init_schema(Db& db);

// 建表 SQL（与 db.py 原型同构，扩展 repeat_rule 为 JSON）
extern const char* kSchemaSql;

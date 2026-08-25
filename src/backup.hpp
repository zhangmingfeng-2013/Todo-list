// backup.hpp — 数据库自动备份（每日一份，保留最近 N 份）
#pragma once
#include <string>
#include <vector>
#include "db.hpp"

namespace backup {

struct BackupFile {
    std::string path;        // 完整路径
    std::string name;        // 文件名 todo-YYYYMMDD-HHMMSS.db
    long long sizeBytes = 0;
};

// 立即备份（checkpoint + 复制主文件到 <库目录>/backups/）。
// keep: 保留最近 N 份，0 表示不清理。
// 返回错误信息，空串表示成功。
std::string backup_now(Db& db, int keep = 10);

// 列出 <库目录>/backups/ 下的备份（新→旧）
std::vector<BackupFile> list_backups(Db& db);

// 每日自动备份：若今天尚无备份则执行（serve 启动时调用）
std::string auto_daily(Db& db);

} // namespace backup

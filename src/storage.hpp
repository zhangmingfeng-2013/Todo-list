// storage.hpp — 存储位置管理：默认库路径配置、可移动卷枚举、数据库迁移
#pragma once

#include <string>
#include <vector>

#include "db.hpp"

namespace storage {

struct Volume {
    std::string name;         // 卷名（挂载点目录名）
    std::string path;         // 挂载点绝对路径
    long long totalBytes = 0;
    long long freeBytes = 0;
    bool writable = false;
    bool removable = false;   // 疑似可移动盘（U盘 / 移动硬盘 / SD 卡）
};

// 默认库路径配置文件（~/.cpp-todo.conf，key=value 一行）
std::string config_path();
// 读取配置的默认库路径；未配置返回空串
std::string load_default_db();
// 写入/清除配置（异常时静默失败，配置仅影响默认值，不致命）
void save_default_db(const std::string& path);
void clear_default_db();

// 决定本次使用的数据库路径：--db 显式参数 > 配置文件 > ./data/todo.db
std::string resolve_db_path(const std::string& flag_db);

// 枚举本机可用卷（macOS: /Volumes；Linux: /media 与 /mnt）
std::vector<Volume> list_volumes();

// 迁移数据库到目标位置（U盘目录或完整 .db 文件路径均可）。
// 流程：checkpoint → 复制到临时文件 → 原子改名 → 原地重开 → 持久化默认路径。
// 成功时 out_new_path 为新路径；失败返回 false 并填充 err。
// 原数据库文件保留为备份，不会被删除。
bool migrate_db(Db& db, const std::string& target,
                std::string& out_new_path, std::string& err,
                bool overwrite = false);

// 数据库文件大小（字节）；失败返回 0
long long db_file_size(const std::string& path);

} // namespace storage

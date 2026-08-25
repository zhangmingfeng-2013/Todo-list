// backup.cpp — 备份实现
#include "backup.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

std::string now_stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[24];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string today_stamp() { return now_stamp().substr(0, 8); }

fs::path backup_dir_of(const std::string& db_path) {
    fs::path p(db_path);
    return p.has_parent_path() ? p.parent_path() / "backups" : fs::path("backups");
}

bool copy_db_file(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

} // namespace

namespace backup {

std::vector<BackupFile> list_backups(Db& db) {
    std::vector<BackupFile> out;
    fs::path dir = backup_dir_of(db.path());
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        std::string name = e.path().filename().string();
        if (name.rfind("todo-", 0) != 0 || e.path().extension() != ".db") continue;
        BackupFile bf;
        bf.path = e.path().string();
        bf.name = name;
        bf.sizeBytes = static_cast<long long>(e.file_size(ec));
        if (ec) bf.sizeBytes = 0;
        out.push_back(bf);
    }
    std::sort(out.begin(), out.end(),
              [](const BackupFile& a, const BackupFile& b) { return a.name > b.name; });
    return out;
}

std::string backup_now(Db& db, int keep) {
    if (!db.ok()) return "数据库未打开";
    if (!db.checkpoint()) return "WAL 落盘失败";

    std::error_code ec;
    fs::path dir = backup_dir_of(db.path());
    fs::create_directories(dir, ec);
    if (ec) return "无法创建备份目录: " + dir.string();

    fs::path dst = dir / ("todo-" + now_stamp() + ".db");
    if (!copy_db_file(fs::path(db.path()), dst)) return "复制数据库失败";
    if (fs::exists(fs::path(db.path() + "-wal"), ec))
        copy_db_file(fs::path(db.path() + "-wal"), dst.replace_extension(".db-wal"));

    if (keep > 0) {
        auto files = list_backups(db);
        for (size_t i = static_cast<size_t>(keep); i < files.size(); ++i)
            fs::remove(fs::path(files[i].path), ec);
    }
    return "";
}

std::string auto_daily(Db& db) {
    std::string today = today_stamp();
    for (auto& b : list_backups(db)) {
        // 文件名形如 todo-YYYYMMDD-HHMMSS.db
        if (b.name.size() >= 14 && b.name.substr(5, 8) == today) return ""; // 今天已备份
        break;  // 列表按新→旧，只看最新一份
    }
    return backup_now(db);
}

} // namespace backup

// storage.cpp — 存储位置管理实现
#include "storage.hpp"

#include <sys/param.h>
#include <sys/mount.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace storage {

// ---- 配置文件 ----

static std::string home_dir() {
    const char* h = ::getenv("HOME");
    return h ? std::string(h) : ".";
}

std::string config_path() { return home_dir() + "/.cpp-todo.conf"; }

std::string load_default_db() {
    std::ifstream f(config_path());
    if (!f) return "";
    std::string line;
    while (std::getline(f, line)) {
        const std::string key = "db=";
        if (line.rfind(key, 0) == 0 && line.size() > key.size()) {
            std::string v = line.substr(key.size());
            // 去尾部空白
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\t'))
                v.pop_back();
            return v;
        }
    }
    return "";
}

void save_default_db(const std::string& path) {
    std::error_code ec;
    fs::path p(config_path());
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(config_path(), std::ios::trunc);
    if (!f) return; // 配置写失败不致命：本次会话仍已切换
    f << "db=" << path << "\n";
}

void clear_default_db() { ::remove(config_path().c_str()); }

std::string resolve_db_path(const std::string& flag_db) {
    if (!flag_db.empty()) return flag_db;
    std::string conf = load_default_db();
    if (!conf.empty()) return conf;
    return "data/todo.db";
}

// ---- 卷枚举 ----

// 从挂载设备名提取物理磁盘标识（"/dev/disk3s1s1" → "disk3"），用于识别系统盘
static std::string physical_disk(const std::string& mntfrom) {
    std::smatch m;
    if (std::regex_search(mntfrom, m, std::regex("(disk\\d+)"))) return m[1].str();
    return mntfrom;
}

static void fill_stat(Volume& v) {
    struct statfs st;
    if (::statfs(v.path.c_str(), &st) == 0) {
        v.totalBytes = static_cast<long long>(st.f_blocks) * st.f_bsize;
        v.freeBytes = static_cast<long long>(st.f_bavail) * st.f_bsize;
    }
}

std::vector<Volume> list_volumes() {
    std::vector<Volume> out;

    // 系统根所在物理盘（非可移动）
    struct statfs root_st;
    std::string root_disk;
    if (::statfs("/", &root_st) == 0)
        root_disk = physical_disk(root_st.f_mntfromname);

    std::vector<std::string> bases;
#ifdef __APPLE__
    bases.push_back("/Volumes");
#else
    bases.push_back("/media");
    bases.push_back("/mnt");
#endif

    std::set<std::string> seen; // 去重
    for (auto& base : bases) {
        std::error_code ec;
        if (!fs::exists(base, ec) || !fs::is_directory(base, ec)) continue;
        for (fs::directory_iterator it(base, ec), end; it != end && !ec; it.increment(ec)) {
            std::error_code ec2;
            if (!it->is_directory(ec2)) continue;
            Volume v;
            v.path = it->path().string();
            v.name = it->path().filename().string();
            if (v.name.empty() || v.name[0] == '.') continue;
            if (!seen.insert(v.path).second) continue;

            struct statfs st;
            if (::statfs(v.path.c_str(), &st) == 0) {
                std::string disk = physical_disk(st.f_mntfromname);
                v.removable = !root_disk.empty() && disk != root_disk;
            }
            fill_stat(v);
            v.writable = ::access(v.path.c_str(), W_OK) == 0;
            out.push_back(std::move(v));
        }
    }
    std::sort(out.begin(), out.end(), [](const Volume& a, const Volume& b) {
        if (a.removable != b.removable) return a.removable; // 可移动盘排前
        return a.name < b.name;
    });
    return out;
}

// ---- 迁移 ----

long long db_file_size(const std::string& path) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec) return 0;
    long long total = static_cast<long long>(sz);
    // WAL 模式下未落盘数据在 -wal 文件中，一并计入
    auto wal = fs::file_size(path + "-wal", ec);
    if (!ec) total += static_cast<long long>(wal);
    return total;
}

static bool copy_file_hardened(const std::string& from, const std::string& to,
                               std::string& err) {
    std::ifstream in(from, std::ios::binary);
    if (!in) { err = "无法读取源数据库: " + from; return false; }
    std::string tmp = to + ".migrating";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { err = "目标位置不可写: " + to; return false; }
        char buf[65536];
        while (in.read(buf, sizeof buf) || in.gcount() > 0)
            out.write(buf, in.gcount());
        if (!out.good()) { err = "写入目标文件失败（磁盘空间不足或已拔出）"; return false; }
    }
    // 原子改名
    std::error_code ec;
    fs::rename(tmp, to, ec);
    if (ec) { ::remove(tmp.c_str()); err = "落盘失败: " + ec.message(); return false; }
    // 清理目标位置可能残留的旧 WAL/SHM（rename 已替换主文件，旧 sidecar 必须删）
    ::remove((to + "-wal").c_str());
    ::remove((to + "-shm").c_str());
    return true;
}

bool migrate_db(Db& db, const std::string& target,
                std::string& out_new_path, std::string& err, bool overwrite) {
    if (target.empty()) { err = "目标路径为空"; return false; }

    // 目标可以是目录（U盘挂载点）或完整 .db 文件路径
    fs::path tp = fs::absolute(fs::path(target));
    if (tp.extension() != ".db") tp /= "todo.db";
    std::string dst = tp.string();

    // 确保父目录存在
    std::error_code ec;
    if (tp.has_parent_path()) {
        fs::create_directories(tp.parent_path(), ec);
        if (ec) { err = "无法创建目标目录: " + ec.message(); return false; }
    }
    if (::access(tp.parent_path().string().c_str(), W_OK) != 0) {
        err = "目标位置不可写: " + tp.parent_path().string();
        return false;
    }

    // 当前路径（绝对化后比较，避免 ./data/todo.db 与 data/todo.db 误判为不同）
    fs::path cur = fs::absolute(fs::path(db.path()));
    std::error_code ec2;
    if (fs::equivalent(cur, tp, ec2) || (!ec2 && cur == tp)) {
        out_new_path = dst;
        err = "";
        return true; // 已在该位置，无需迁移
    }

    // 目标已存在且非空 → 默认拒绝，需显式 overwrite
    if (fs::exists(tp, ec) && db_file_size(dst) > 0) {
        if (!overwrite) {
            err = "目标已存在数据库文件: " + dst + "（需确认覆盖）";
            return false;
        }
    }

    // 1) WAL 落盘，保证主文件完整
    if (!db.checkpoint()) { err = "数据库落盘失败（checkpoint）"; return false; }

    // 2) 复制（临时文件 + 原子改名）
    if (!copy_file_hardened(cur.string(), dst, err)) return false;

    // 3) 原地重开
    if (!db.reopen(dst)) {
        err = "新库打开失败: " + db.error() + "（数据已复制到 " + dst + "，原文件未动）";
        return false;
    }
    init_schema(db);

    // 4) 持久化为默认路径
    save_default_db(dst);

    out_new_path = dst;
    return true;
}

} // namespace storage

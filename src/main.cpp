// main.cpp — cpp-todo 入口：CLI 与本地服务两种模式
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>

#include "db.hpp"
#include "api.hpp"
#include "http.hpp"
#include "lunar.hpp"
#include "recurrence.hpp"
#include "importer.hpp"
#include "json.hpp"
#include "storage.hpp"
#include "exporter.hpp"
#include "backup.hpp"

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

namespace fs = std::filesystem;

static const char* kVersion = "1.0.0";
static const char* kDefaultPort = "8931";

struct Options {
    std::string db_path;
    int port = 8931;
    bool open_browser = false;
};

static void ensure_data_dir(const std::string& db_path) {
    fs::path p(db_path);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }
}

static void print_help() {
    std::cout <<
        "todo " << kVersion << " — 本机 C++ 待办应用（SQLite，零账号零安装）\n\n"
        "用法: todo <命令> [选项]\n\n"
        "服务:\n"
        "  serve [--port N] [--db PATH] [--open]   启动本地服务并打开 Web 界面\n"
        "  open  [--port N]                        启动服务并自动打开浏览器(macOS)\n\n"
        "任务:\n"
        "  add \"标题\" [--due YYYY-MM-DD] [--start YYYY-MM-DD] [--time HH:MM]\n"
        "             [--prio high|med|low] [--project 名称] [--tag 标签]...\n"
        "             [--parent ID] [--notes 文本] [--lunar M-D] [--repeat daily|weekly|monthly|custom]\n"
        "  list [--today] [--project 名称] [--tag 标签] [--status all|todo|doing|done] [--due N天内]\n"
        "  tree                             查看任务树（子任务缩进）\n"
        "  done <id>                         完成（重复任务自动生成下一次）\n"
        "  undo <id>                         恢复\n"
        "  rm <id> [--purge]                 删除（默认进回收站，--purge 彻底删除）\n"
        "  restore <id>                      从回收站恢复任务\n"
        "  trash [clear]                     回收站列表 / 清空\n"
        "  dep <id> <依赖id>                 设置依赖（依赖完成后才能开始）\n"
        "  undep <id> <依赖id>               移除依赖\n\n"
        "组织:\n"
        "  projects                         项目列表\n"
        "  calendar [--month YYYY-MM]       月历视图\n"
        "  tags                             标签列表\n"
        "  stats                            统计概览（完成趋势/逾期/番茄钟）\n"
        "  digest                           每日摘要（农历/节气/今日任务）\n"
        "  filter add \"名称\" \"条件JSON\"    保存筛选\n"
        "  filter list / filter rm <id>\n\n"
        "导入 (滴答/Todoist/Todo.txt):\n"
        "  import <文件> [--format todotxt|todoist|ticktick|csv] [--db PATH]\n\n"
        "导出:\n"
        "  export [--format todotxt|json|csv] [--out 文件]\n\n"
        "备份:\n"
        "  backup [now|list|restore <文件>] 立即备份 / 列出 / 从备份恢复（serve 每日自动备份）\n\n"
        "节假日 (重复任务跳过):\n"
        "  holiday add YYYY-MM-DD / holiday list / holiday rm YYYY-MM-DD\n"
        "  holiday auto [--year N]          自动生成某年法定节假日（含农历/节气推导）\n\n"
        "存储 (U盘 / 可移动盘):\n"
        "  storage                        查看当前存储位置\n"
        "  storage list                   列出可用卷（U盘会标记 ◈）\n"
        "  storage move <卷路径|文件路径> 迁移数据库到 U盘等位置（原文件保留备份）\n"
        "  storage reset                  恢复默认位置（./data/todo.db）\n\n"
        "全局:\n"
        "  --db PATH     数据库文件位置（优先级: --db > storage 配置 > ./data/todo.db）\n"
        "  help          显示帮助\n";
}

static bool arg_has(const std::vector<std::string>& args, const std::string& key,
                    std::string& val) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == key && i + 1 < args.size()) {
            val = args[i + 1];
            return true;
        }
    }
    return false;
}

static bool arg_flag(const std::vector<std::string>& args, const std::string& key) {
    for (auto& a : args) if (a == key) return true;
    return false;
}

// ---- 显示辅助 ----
static std::string prio_str(int p) {
    return p == 2 ? "[高]" : p == 0 ? "[低]" : "[中]";
}
static std::string status_mark(const std::string& s) {
    return s == "done" ? "✓" : s == "doing" ? "▶" : "☐";
}
static std::string color_status(const std::string& s) {
    // 终端彩色（仅在有 TTY 时生效）
    if (s == "done") return "\033[90m";
    return "";
}
static const char* kReset = "\033[0m";

static void print_task_line(Db& db, long long id, int depth, bool show_id) {
    auto row = db.query_one("SELECT * FROM tasks WHERE id=? AND deleted_at IS NULL",
                            {std::to_string(id)});
    if (!row) return;
    std::string indent(static_cast<size_t>(depth) * 2, ' ');
    std::string due = row->get("due_date");
    std::string due_str;
    if (!due.empty()) {
        std::string today = lunar::today_iso();
        if (due < today && row->get("status") != "done") due_str = " (逾期" + due + ")";
        else due_str = " (" + due + ")";
    }
    std::string proj;
    long long pid = row->get_int("project_id");
    if (pid) {
        if (auto p = db.query_one("SELECT name FROM projects WHERE id=?",
                                  {std::to_string(pid)}))
            proj = " 「" + p->get("name") + "」";
    }
    std::string tags;
    for (auto& t : db.query(
             "SELECT t.name FROM tags t JOIN task_tags tt ON t.id=tt.tag_id "
             "WHERE tt.task_id=?", {std::to_string(id)}))
        tags += " #" + t.get("name");
    std::string rr = row->get("repeat_rule");
    std::string rep = rr.empty() ? "" : " ↻";
    std::string blocked = "";
    for (auto& d : db.query(
             "SELECT t.status FROM task_dependencies td JOIN tasks t ON t.id=td.depends_on "
             "WHERE td.task_id=? AND t.status!='done'", {std::to_string(id)}))
        blocked = " ⛔";
    std::string id_s = show_id ? ("#" + std::to_string(id) + " ") : "";
    std::cout << indent << color_status(row->get("status")) << status_mark(row->get("status"))
              << " " << id_s << prio_str(static_cast<int>(row->get_int("priority")))
              << " " << row->get("title") << due_str << proj << tags << rep << blocked
              << kReset << "\n";
}

static void print_tree_rec(Db& db, long long parent, int depth) {
    std::vector<long long> kids;
    for (auto& r : db.query("SELECT id FROM tasks WHERE deleted_at IS NULL AND parent_id=? ORDER BY sort_order, id",
                            {parent == 0 ? "" : std::to_string(parent)})) {
        // parent==0 → 根任务
        kids.push_back(r.get_int("id"));
    }
    // 处理根任务（parent_id IS NULL）
    if (parent == 0) {
        for (auto& r : db.query(
                 "SELECT id FROM tasks WHERE deleted_at IS NULL AND parent_id IS NULL "
                 "ORDER BY CASE status WHEN 'done' THEN 1 ELSE 0 END, priority DESC, "
                 "COALESCE(due_date,'9999-12-31'), sort_order, id")) {
            kids.push_back(r.get_int("id"));
        }
    }
    for (long long id : kids) {
        print_task_line(db, id, depth, true);
        print_tree_rec(db, id, depth + 1);
    }
}

// ---- 命令实现 ----
static int cmd_add(Db& db, const std::vector<std::string>& args) {
    std::string title = args.empty() ? "" : args[0];
    if (title.empty()) { std::cerr << "错误: 缺少标题\n"; return 1; }
    std::string due, start, time, prio_s, proj, parent_s, notes, lunar_date, repeat;
    std::vector<std::string> tags;
    for (size_t i = 1; i < args.size(); ++i) {
        std::string a = args[i];
        auto next = [&](const char* msg) -> std::string {
            if (i + 1 < args.size()) { ++i; return args[i]; }
            std::cerr << "错误: " << msg << " 缺少参数\n";
            return "";
        };
        if (a == "--due") due = next("--due");
        else if (a == "--start") start = next("--start");
        else if (a == "--time") time = next("--time");
        else if (a == "--prio") prio_s = next("--prio");
        else if (a == "--project") proj = next("--project");
        else if (a == "--parent") parent_s = next("--parent");
        else if (a == "--notes") notes = next("--notes");
        else if (a == "--lunar") lunar_date = next("--lunar");
        else if (a == "--repeat") repeat = next("--repeat");
        else if (a == "--tag") tags.push_back(next("--tag"));
        else if (a.rfind("#", 0) == 0 && a.size() > 1) tags.push_back(a.substr(1));
        else { std::cerr << "错误: 未知参数 " << a << "\n"; return 1; }
    }
    int prio = 1;
    if (prio_s == "high") prio = 2;
    else if (prio_s == "low") prio = 0;
    else if (!prio_s.empty() && prio_s != "med") { std::cerr << "错误: 优先级应为 high/med/low\n"; return 1; }

    long long pid = 0;
    if (!proj.empty()) {
        auto row = db.query_one("SELECT id FROM projects WHERE name=?", {proj});
        if (row) pid = row->get_int("id");
        else {
            std::string sql = "INSERT INTO projects(name) VALUES('" + std::string(proj) + "')";
            // 转义
            std::string esc;
            for (char c : proj) esc += (c == '\'') ? "''" : std::string(1, c);
            db.exec("INSERT INTO projects(name) VALUES('" + esc + "')");
            pid = db.last_insert_rowid();
        }
    }
    long long parent = parent_s.empty() ? 0 : std::atoll(parent_s.c_str());
    std::string repeat_rule;
    if (!repeat.empty()) {
        RepeatRule rr;
        if (repeat == "daily") rr.freq = "daily";
        else if (repeat == "weekly") rr.freq = "weekly";
        else if (repeat == "monthly") rr.freq = "monthly";
        else if (repeat == "custom") rr.freq = "custom";
        else { std::cerr << "错误: --repeat 应为 daily|weekly|monthly|custom\n"; return 1; }
        repeat_rule = rr.to_json().dump();
    }
    int has_rem = time.empty() ? 0 : 1;

    std::string esc_t = title, esc_n = notes, esc_l = lunar_date, esc_r = repeat_rule;
    for (auto& p : {&esc_t, &esc_n, &esc_l, &esc_r}) {
        std::string o = *p, n;
        for (char c : o) n += (c == '\'') ? "''" : std::string(1, c);
        *p = n;
    }
    std::string sql = "INSERT INTO tasks(title,notes,priority,start_date,due_date,remind_time,"
                      "has_reminder,lunar_remind,lunar_date,project_id,parent_id,repeat_rule) "
                      "VALUES('" + esc_t + "','" + esc_n + "'," + std::to_string(prio) + "," +
                      (start.empty() ? "NULL" : "'" + start + "'") + "," +
                      (due.empty() ? "NULL" : "'" + due + "'") + "," +
                      (time.empty() ? "NULL" : "'" + time + "'") + "," +
                      std::to_string(has_rem) + "," +
                      (lunar_date.empty() ? "0" : "1") + "," +
                      (lunar_date.empty() ? "NULL" : "'" + esc_l + "'") + "," +
                      (pid ? std::to_string(pid) : "NULL") + "," +
                      (parent ? std::to_string(parent) : "NULL") + ",'" + esc_r + "')";
    db.exec(sql);
    long long id = db.last_insert_rowid();
    for (auto& t : tags) {
        std::string esc;
        for (char c : t) esc += (c == '\'') ? "''" : std::string(1, c);
        auto row = db.query_one("SELECT id FROM tags WHERE name=?", {t});
        long long tid;
        if (row) tid = row->get_int("id");
        else { db.exec("INSERT INTO tags(name) VALUES('" + esc + "')"); tid = db.last_insert_rowid(); }
        db.exec("INSERT OR IGNORE INTO task_tags(task_id,tag_id) VALUES(" +
                std::to_string(id) + "," + std::to_string(tid) + ")");
    }
    std::cout << "已创建任务 #" << id << ": " << title << "\n";
    return 0;
}

static int cmd_list(Db& db, const std::vector<std::string>& args) {
    bool today = arg_flag(args, "--today");
    std::string proj, tag, status = "todo";
    std::string due_n;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--project" && i + 1 < args.size()) proj = args[++i];
        else if (args[i] == "--tag" && i + 1 < args.size()) tag = args[++i];
        else if (args[i] == "--status" && i + 1 < args.size()) status = args[++i];
        else if (args[i] == "--due" && i + 1 < args.size()) due_n = args[++i];
        else if (args[i] == "--all") status = "all";
    }
    std::string sql = "SELECT * FROM tasks WHERE deleted_at IS NULL";
    if (status != "all") sql += " AND status='" + status + "'";
    if (today) {
        std::string t = lunar::today_iso();
        sql += " AND ((due_date='" + t + "') OR (start_date='" + t + "') OR "
               "(due_date IS NOT NULL AND due_date<'" + t + "' AND status!='done'))";
    }
    if (!proj.empty()) {
        auto row = db.query_one("SELECT id FROM projects WHERE name=?", {proj});
        if (!row) { std::cout << "项目不存在: " << proj << "\n"; return 0; }
        sql += " AND project_id=" + std::to_string(row->get_int("id"));
    }
    if (!tag.empty())
        sql += " AND EXISTS(SELECT 1 FROM task_tags tt JOIN tags t ON t.id=tt.tag_id "
               "WHERE tt.task_id=tasks.id AND t.name='" + tag + "')";
    if (!due_n.empty()) {
        int n = std::max(0, std::atoi(due_n.c_str()));
        sql += " AND due_date IS NOT NULL AND due_date>='" + lunar::today_iso() +
               "' AND due_date<='" + lunar::add_days_iso(lunar::today_iso(), n) + "'";
    }
    sql += " ORDER BY CASE status WHEN 'done' THEN 1 ELSE 0 END, priority DESC, "
           "COALESCE(due_date,'9999-12-31'), sort_order, id";
    auto rows = db.query(sql);
    if (rows.empty()) { std::cout << "（无任务）\n"; return 0; }
    int n = 0;
    for (auto& r : rows) { print_task_line(db, r.get_int("id"), 0, true); ++n; }
    std::cout << "共 " << n << " 条\n";
    return 0;
}

static int cmd_done_undo(Db& db, const std::vector<std::string>& args, bool done) {
    if (args.empty()) { std::cerr << "错误: 缺少任务 ID\n"; return 1; }
    long long id = std::atoll(args[0].c_str());
    auto row = db.query_one("SELECT * FROM tasks WHERE id=? AND deleted_at IS NULL", {std::to_string(id)});
    if (!row) { std::cerr << "任务不存在 #" << id << "\n"; return 1; }
    if (done) {
        db.exec("UPDATE tasks SET status='done', completed_at=datetime('now','localtime') "
                "WHERE id=" + std::to_string(id));
        std::string rr = row->get("repeat_rule");
        if (!rr.empty()) {
            try {
                RepeatRule rule = RepeatRule::from_json(Json::parse(rr));
                if (rule.enabled()) {
                    auto holiday = [&](const std::string& d) -> bool {
                        return db.query_one("SELECT 1 FROM holidays WHERE date=?", {d}).has_value();
                    };
                    std::string next = recurrence::next_after(rule, row->get("due_date"), holiday);
                    if (!next.empty() && (rule.end_date.empty() || next <= rule.end_date)) {
                        std::string t = row->get("title");
                        std::string esc;
                        for (char c : t) esc += (c == '\'') ? "''" : std::string(1, c);
                        db.exec("INSERT INTO tasks(title,notes,priority,due_date,remind_time,"
                                "has_reminder,lunar_remind,lunar_date,project_id,repeat_rule) "
                                "VALUES('" + esc + "','" + row->get("notes") + "'," +
                                std::to_string(row->get_int("priority")) + ",'" + next + "','" +
                                row->get("remind_time") + "'," +
                                std::to_string(row->get_int("has_reminder")) + "," +
                                std::to_string(row->get_int("lunar_remind")) + ",'" +
                                row->get("lunar_date") + "'," +
                                (row->get_int("project_id") ? std::to_string(row->get_int("project_id")) : "NULL") +
                                ",'" + rr + "')");
                        long long nid = db.last_insert_rowid();
                        for (auto& t2 : db.query(
                                 "SELECT tag_id FROM task_tags WHERE task_id=?",
                                 {std::to_string(id)}))
                            db.exec("INSERT OR IGNORE INTO task_tags(task_id,tag_id) VALUES(" +
                                    std::to_string(nid) + "," +
                                    std::to_string(t2.get_int("tag_id")) + ")");
                        std::cout << "完成 #" << id << "，已生成下一次实例 #" << nid
                                  << "（" << next << "）\n";
                        return 0;
                    }
                }
            } catch (...) {}
        }
        std::cout << "已完成 #" << id << "\n";
    } else {
        db.exec("UPDATE tasks SET status='todo', completed_at=NULL WHERE id=" +
                std::to_string(id));
        std::cout << "已恢复 #" << id << "\n";
    }
    return 0;
}

static int cmd_tree(Db& db) {
    print_tree_rec(db, 0, 0);
    return 0;
}

static int cmd_calendar(Db& db, const std::vector<std::string>& args) {
    std::string month;
    arg_has(args, "--month", month);
    int year = 0, m = 0;
    if (!month.empty() && std::sscanf(month.c_str(), "%d-%d", &year, &m) != 2) {
        std::cerr << "错误: --month 格式应为 YYYY-MM\n";
        return 1;
    }
    if (year == 0) {
        int y2 = 0, m2 = 0, d2 = 0;
        std::sscanf(lunar::today_iso().c_str(), "%d-%d-%d", &y2, &m2, &d2);
        year = y2; m = m2;
    }
    char first[16];
    std::snprintf(first, sizeof first, "%04d-%02d-01", year, m);
    std::string next_first = (m == 12)
        ? (std::to_string(year + 1) + "-01-01")
        : ([&]() { char b[16]; std::snprintf(b, sizeof b, "%04d-%02d-01", year, m + 1); return std::string(b); })();
    std::string last = lunar::add_days_iso(next_first, -1);
    std::string today = lunar::today_iso();

    auto holiday = [&](const std::string& d) -> bool {
        return db.query_one("SELECT 1 FROM holidays WHERE date=?", {d}).has_value();
    };

    std::cout << "—— " << year << " 年 " << m << " 月 ——\n";
    std::cout << "一 二 三 四 五 六 日\n";
    // 当月1日星期：1=周一..7=周日 → 前置空格
    int w1 = lunar::weekday_of_iso(first);
    for (int i = 1; i < w1; ++i) std::cout << "   ";
    std::string d = first;
    int guard = 0;
    while (d <= last && guard++ < 31) {
        int dd = std::atoi(d.c_str() + 8);
        bool is_today = d == today;
        // 标记到期任务
        auto cnt = db.query_one(
            "SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND status!='done' AND "
            "(due_date=? OR start_date=? OR "
            "(repeat_rule!='' AND due_date IS NULL))", {d, d});
        long long c = cnt ? cnt->get_int("c") : 0;
        // 重复实例粗略统计
        long long rep_c = 0;
        for (auto& r : db.query("SELECT due_date,repeat_rule FROM tasks WHERE deleted_at IS NULL AND repeat_rule!='' AND status!='done'")) {
            RepeatRule rr = RepeatRule::from_json_str(r.get("repeat_rule"));
            if (!rr.enabled()) continue;
            auto occ = recurrence::occurrences_in_range(rr, d, d, holiday);
            rep_c += static_cast<long long>(occ.size());
        }
        std::string mark = (c > 0 || rep_c > 0) ? "•" : " ";
        if (is_today) std::cout << "\033[7m" << dd << mark << "\033[0m";
        else std::cout << (dd < 10 ? " " : "") << dd << mark;
        std::cout << " ";
        int w = lunar::weekday_of_iso(d);
        if (w == 7) std::cout << "\n";
        d = lunar::add_days_iso(d, 1);
    }
    std::cout << "\n";
    // 当日任务
    auto today_rows = db.query(
        "SELECT * FROM tasks WHERE deleted_at IS NULL AND status!='done' AND (due_date=? OR start_date=?) "
        "ORDER BY priority DESC", {today, today});
    if (!today_rows.empty()) {
        std::cout << "今日任务:\n";
        for (auto& r : today_rows) print_task_line(db, r.get_int("id"), 1, true);
    }
    return 0;
}

static int cmd_import(Db& db, const std::vector<std::string>& args) {
    if (args.empty()) { std::cerr << "错误: 缺少文件路径\n"; return 1; }
    std::string format;
    arg_has(args, "--format", format);
    std::ifstream f(args[0], std::ios::binary);
    if (!f) { std::cerr << "无法打开文件: " << args[0] << "\n"; return 1; }
    std::ostringstream ss;
    ss << f.rdbuf();
    ImportResult res = importer::import_text(db, format, ss.str());
    std::cout << res.summary() << "\n";
    for (auto& e : res.errors) std::cerr << "  ! " << e << "\n";
    return res.errors.empty() ? 0 : 1;
}

static int cmd_dep(Db& db, const std::vector<std::string>& args, bool add) {
    if (args.size() < 2) { std::cerr << "用法: dep <id> <依赖id>\n"; return 1; }
    long long id = std::atoll(args[0].c_str()), dep = std::atoll(args[1].c_str());
    if (add)
        db.exec("INSERT OR IGNORE INTO task_dependencies(task_id,depends_on) VALUES(" +
                std::to_string(id) + "," + std::to_string(dep) + ")");
    else
        db.exec("DELETE FROM task_dependencies WHERE task_id=" + std::to_string(id) +
                " AND depends_on=" + std::to_string(dep));
    std::cout << (add ? "已设置依赖" : "已移除依赖") << " #" << id << " ← #" << dep << "\n";
    return 0;
}

static int cmd_holiday(Db& db, const std::vector<std::string>& args) {
    if (args.empty()) { std::cerr << "用法: holiday add|list|rm|auto [YYYY-MM-DD|--year N]\n"; return 1; }
    std::string op = args[0];
    if (op == "auto") {
        std::string ys;
        arg_has(args, "--year", ys);
        int year = ys.empty() ? 0 : std::atoi(ys.c_str());
        if (year == 0) {
            int y = 0, m = 0, d = 0;
            std::sscanf(lunar::today_iso().c_str(), "%d-%d-%d", &y, &m, &d);
            year = y;
        }
        if (year < 1901 || year > 2099) {
            std::cerr << "错误: 年份应在 1901-2099（农历推导范围）\n";
            return 1;
        }
        int added = 0;
        std::cout << "—— " << year << " 年法定节假日 ——\n";
        for (int m = 1; m <= 12; ++m) {
            int dim = 31;
            if (m == 4 || m == 6 || m == 9 || m == 11) dim = 30;
            if (m == 2) dim = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29 : 28;
            for (int d = 1; d <= dim; ++d) {
                std::string name = lunar::statutory_holiday(year, m, d);
                if (name.empty()) continue;
                char iso[16];
                std::snprintf(iso, sizeof iso, "%04d-%02d-%02d", year, m, d);
                db.exec("INSERT OR IGNORE INTO holidays(date) VALUES('" + std::string(iso) + "')");
                std::cout << "  " << iso << "  " << name << "\n";
                ++added;
            }
        }
        std::cout << "共 " << added << " 天已写入节假日表（重复任务将自动跳过）\n";
        return 0;
    }
    if (op == "list") {
        for (auto& h : db.query("SELECT date FROM holidays ORDER BY date"))
            std::cout << h.get("date") << "\n";
        return 0;
    }
    if (args.size() < 2) { std::cerr << "错误: 缺少日期\n"; return 1; }
    std::string date = args[1];
    int y = 0, m = 0, d = 0;
    if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
        std::cerr << "错误: 日期格式应为 YYYY-MM-DD\n";
        return 1;
    }
    if (op == "add") db.exec("INSERT OR IGNORE INTO holidays(date) VALUES('" + date + "')");
    else if (op == "rm" || op == "del") db.exec("DELETE FROM holidays WHERE date='" + date + "'");
    else { std::cerr << "错误: 未知操作 " << op << "\n"; return 1; }
    std::cout << (op == "add" ? "已添加节假日 " : "已移除节假日 ") << date << "\n";
    return 0;
}

static int cmd_projects(Db& db) {
    for (auto& p : db.query("SELECT * FROM projects ORDER BY sort_order, name")) {
        auto cnt = db.query_one(
            "SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND project_id=? AND status!='done'",
            {std::to_string(p.get_int("id"))});
        std::cout << "#" << p.get_int("id") << " " << p.get("name")
                  << " (" << (cnt ? cnt->get_int("c") : 0) << " 个未完成任务)\n";
    }
    return 0;
}

static int cmd_tags(Db& db) {
    for (auto& t : db.query(
             "SELECT t.name,COUNT(tt.task_id) c FROM tags t "
             "LEFT JOIN task_tags tt ON tt.tag_id=t.id "
             "LEFT JOIN tasks ts ON ts.id=tt.task_id AND ts.status!='done' "
             "GROUP BY t.id ORDER BY c DESC")) {
        std::cout << t.get("name") << " (" << t.get_int("c") << ")\n";
    }
    return 0;
}

static std::string human_size(long long bytes) {
    char buf[32];
    if (bytes >= 1LL << 30) std::snprintf(buf, sizeof buf, "%.1f GB", bytes / 1073741824.0);
    else if (bytes >= 1LL << 20) std::snprintf(buf, sizeof buf, "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
    else std::snprintf(buf, sizeof buf, "%lld B", bytes);
    return buf;
}

// ---- 回收站 ----
static int cmd_trash(Db& db, const std::vector<std::string>& args) {
    if (!args.empty() && (args[0] == "clear" || args[0] == "empty")) {
        db.exec("DELETE FROM tasks WHERE deleted_at IS NOT NULL");
        std::cout << "回收站已清空\n";
        return 0;
    }
    auto rows = db.query(
        "SELECT * FROM tasks WHERE deleted_at IS NOT NULL ORDER BY deleted_at DESC");
    if (rows.empty()) { std::cout << "（回收站为空）\n"; return 0; }
    for (auto& r : rows)
        std::cout << "#" << r.get_int("id") << " " << status_mark(r.get("status"))
                  << " " << r.get("title")
                  << "  (删除于 " << r.get("deleted_at") << ")\n";
    std::cout << "共 " << rows.size() << " 条 · 恢复: todo restore <id> · 清空: todo trash clear\n";
    return 0;
}

static int cmd_restore(Db& db, const std::vector<std::string>& args) {
    if (args.empty()) { std::cerr << "错误: 缺少任务 ID\n"; return 1; }
    long long id = std::atoll(args[0].c_str());
    auto row = db.query_one("SELECT title FROM tasks WHERE id=? AND deleted_at IS NOT NULL",
                            {std::to_string(id)});
    if (!row) { std::cerr << "回收站中不存在 #" << id << "\n"; return 1; }
    db.exec("UPDATE tasks SET deleted_at=NULL, updated_at=datetime('now','localtime') "
            "WHERE id=" + std::to_string(id));
    std::cout << "✓ 已从回收站恢复 #" << id << ": " << row->get("title") << "\n";
    return 0;
}

// ---- 导出 ----
static int cmd_export(Db& db, const std::vector<std::string>& args) {
    std::string format = "todotxt", out;
    arg_has(args, "--format", format);
    arg_has(args, "--out", out);
    if (format != "todotxt" && format != "json" && format != "csv") {
        std::cerr << "错误: --format 应为 todotxt|json|csv\n";
        return 1;
    }
    std::string data = exporter::export_tasks(db, format);
    if (out.empty()) out = "todo-export-" + lunar::today_iso() + "." + format;
    std::ofstream f(out, std::ios::binary);
    if (!f) { std::cerr << "无法写入文件: " << out << "\n"; return 1; }
    f << data;
    f.close();
    auto cnt = db.query_one("SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL");
    std::cout << "✓ 已导出 " << (cnt ? cnt->get_int("c") : 0) << " 个任务 → " << out
              << "（" << human_size(static_cast<long long>(data.size())) << "）\n";
    return 0;
}

// ---- 每日摘要 ----
static int cmd_digest(Db& db) {
    std::cout << exporter::daily_digest(db) << "\n";
    return 0;
}

// ---- 统计概览 ----
static int cmd_stats(Db& db) {
    std::string today = lunar::today_iso();
    auto cnt = [&](const std::string& cond) -> long long {
        auto r = db.query_one("SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND " + cond);
        return r ? r->get_int("c") : 0;
    };
    std::cout << "—— 统计概览（截至 " << today << "）——\n";
    std::cout << "待办 " << cnt("status='todo'")
              << " · 进行中 " << cnt("status='doing'")
              << " · 已完成 " << cnt("status='done'")
              << " · 逾期 " << cnt("status!='done' AND due_date IS NOT NULL AND due_date<'" + today + "'")
              << " · 今日到期 " << cnt("status!='done' AND due_date='" + today + "'") << "\n";
    // 最近 7 天完成趋势
    std::cout << "近 7 天完成: ";
    for (int i = 6; i >= 0; --i) {
        std::string d = lunar::add_days_iso(today, -i);
        std::cout << d.substr(5) << "=" << cnt("status='done' AND completed_at LIKE '" + d + "%' ")
                  << " ";
    }
    std::cout << "\n";
    // 连续打卡
    int streak = 0;
    for (int i = 0; i < 3650; ++i) {
        std::string d = lunar::add_days_iso(today, -i);
        if (cnt("status='done' AND completed_at LIKE '" + d + "%' ") > 0) ++streak;
        else if (i > 0) break;
    }
    auto pomo = db.query_one(
        "SELECT COALESCE(SUM(pomodoros),0) s FROM tasks WHERE deleted_at IS NULL");
    std::cout << "连续打卡 " << streak << " 天 · 番茄钟累计 "
              << (pomo ? pomo->get_int("s") : 0) << " 个\n";
    // 项目进度
    auto projs = db.query(
        "SELECT p.name, "
        "SUM(CASE WHEN t.status!='done' THEN 1 ELSE 0 END) open, "
        "SUM(CASE WHEN t.status='done' THEN 1 ELSE 0 END) done "
        "FROM projects p LEFT JOIN tasks t ON t.project_id=p.id AND t.deleted_at IS NULL "
        "GROUP BY p.id ORDER BY open DESC LIMIT 10");
    if (!projs.empty()) {
        std::cout << "项目进度:\n";
        for (auto& p : projs) {
            long long open = p.get_int("open"), done = p.get_int("done");
            int total = static_cast<int>(open + done);
            int pct = total ? static_cast<int>(done * 100 / total) : 0;
            int filled = pct / 10;
            std::cout << "  " << p.get("name") << "  [";
            for (int i = 0; i < 10; ++i) std::cout << (i < filled ? "█" : "░");
            std::cout << "] " << pct << "% (" << done << "/" << total << ")\n";
        }
    }
    return 0;
}

// ---- 备份 ----
static int cmd_backup(Db& db, const std::vector<std::string>& args) {
    std::string op = args.empty() ? "now" : args[0];
    if (op == "list") {
        auto files = backup::list_backups(db);
        if (files.empty()) { std::cout << "（暂无备份）\n"; return 0; }
        for (auto& b : files)
            std::cout << "  " << b.name << "  " << human_size(b.sizeBytes)
                      << "  " << b.path << "\n";
        return 0;
    }
    if (op == "restore") {
        if (args.size() < 2) {
            std::cerr << "用法: backup restore <备份文件名或完整路径>\n";
            return 1;
        }
        fs::path src = args[1];
        if (src.is_relative()) {
            fs::path dir = fs::path(db.path()).parent_path() / "backups";
            if (fs::exists(dir / src)) src = dir / src;
        }
        if (!fs::exists(src)) { std::cerr << "备份文件不存在: " << args[1] << "\n"; return 1; }
        // 当前状态先留底，防止恢复错了
        std::string err = backup::backup_now(db, 0);
        if (!err.empty()) std::cerr << "警告: 恢复前备份失败(" << err << ")，继续恢复\n";
        db.checkpoint();
        std::error_code ec;
        fs::copy_file(src, fs::path(db.path()), fs::copy_options::overwrite_existing, ec);
        if (ec) { std::cerr << "恢复失败: " << ec.message() << "\n"; return 1; }
        db.reopen(db.path());
        init_schema(db);
        std::cout << "✓ 已从备份恢复: " << src.string() << "\n"
                  << "  （恢复前的状态已另存一份备份）\n";
        return 0;
    }
    if (op == "now") {
        std::string err = backup::backup_now(db);
        if (!err.empty()) { std::cerr << "备份失败: " << err << "\n"; return 1; }
        std::cout << "✓ 备份完成（<库目录>/backups/，保留最近 10 份）\n";
        return 0;
    }
    std::cerr << "未知操作: " << op << "（可用: now / list / restore）\n";
    return 1;
}

// ---- 系统通知（macOS） ----
static void send_notification(const std::string& title, const std::string& msg) {
#ifdef __APPLE__
    // AppleScript 字符串内转义双引号与反斜杠
    auto esc = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '"' || c == '\\') r += '\\';
            r += c;
        }
        return r;
    };
    std::string cmd = "osascript -e 'display notification \"" + esc(msg) +
                      "\" with title \"" + esc(title) + "\"' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#else
    (void)title; (void)msg;
#endif
}

// 提醒轮询线程：每 20 秒检查当日到期任务的 remind_time
static void reminder_thread(Db* pdb, std::atomic<bool>* running) {
    std::set<std::string> notified;   // key: 日期|id|时间
    // 记录启动时刻 HH:MM，只通知启动之后到期的提醒（避免重启时轰炸历史提醒）
    std::time_t st = std::time(nullptr);
    std::tm stm{};
    localtime_r(&st, &stm);
    char start_hhmm[8];
    std::snprintf(start_hhmm, sizeof start_hhmm, "%02d:%02d", stm.tm_hour, stm.tm_min);
    while (running->load()) {
        try {
            std::time_t now = std::time(nullptr);
            std::tm tmv{};
            localtime_r(&now, &tmv);
            char hhmm[8];
            std::snprintf(hhmm, sizeof hhmm, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            std::string today = lunar::today_iso();
            for (auto& r : pdb->query(
                     "SELECT id,title,remind_time FROM tasks WHERE deleted_at IS NULL "
                     "AND has_reminder=1 AND remind_time IS NOT NULL "
                     "AND remind_time<='" + std::string(hhmm) + "' "
                     "AND remind_time>='" + std::string(start_hhmm) + "' "
                     "AND (due_date='" + today + "' OR start_date='" + today + "')")) {
                std::string key = today + "|" + r.get("id") + "|" + r.get("remind_time");
                if (notified.count(key)) continue;
                notified.insert(key);
                send_notification("todo 提醒",
                                  r.get("title") + " ⏰ " + r.get("remind_time"));
            }
        } catch (...) {}
        for (int i = 0; i < 20 && running->load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ---- 存储位置管理 ----
static int cmd_storage(Db& db, const std::vector<std::string>& args) {
    std::string op = args.empty() ? "info" : args[0];

    if (op == "list") {
        auto vols = storage::list_volumes();
        if (vols.empty()) { std::cout << "未发现可用卷（/Volumes 为空）\n"; return 0; }
        std::cout << "可用卷（◈ = 疑似可移动盘/U盘）:\n";
        for (auto& v : vols) {
            std::cout << (v.removable ? "  ◈ " : "    ") << v.name
                      << "  " << v.path
                      << "  总 " << human_size(v.totalBytes)
                      << " / 剩余 " << human_size(v.freeBytes)
                      << (v.writable ? "  [可写]" : "  [只读]")
                      << "\n";
        }
        std::cout << "\n迁移: todo storage move <卷路径>   例: todo storage move /Volumes/"
                  << vols.front().name << "\n";
        return 0;
    }
    if (op == "move") {
        if (args.size() < 2) {
            std::cerr << "用法: storage move <卷路径|完整.db路径> [--overwrite]\n";
            return 1;
        }
        bool overwrite = false;
        for (size_t i = 2; i < args.size(); ++i) if (args[i] == "--overwrite") overwrite = true;
        std::string new_path, err;
        if (!storage::migrate_db(db, args[1], new_path, err, overwrite)) {
            std::cerr << "迁移失败: " << err << "\n";
            if (err.find("需确认覆盖") != std::string::npos)
                std::cerr << "确认覆盖请加 --overwrite\n";
            return 1;
        }
        std::cout << "✓ 已迁移到: " << new_path << "\n"
                  << "  原数据库文件保留为备份，可手动删除\n"
                  << "  已写入默认路径配置（" << storage::config_path() << "），下次启动自动使用新位置\n";
        return 0;
    }
    if (op == "info" || op == "status") {
        std::cout << "当前数据库: " << db.path()
                  << "  (" << human_size(storage::db_file_size(db.path())) << ")\n";
        std::string conf = storage::load_default_db();
        std::cout << "默认路径配置: " << (conf.empty() ? "（未设置，使用 ./data/todo.db）" : conf)
                  << "  [" << storage::config_path() << "]\n";
        return 0;
    }
    std::cerr << "未知操作: " << op << "（可用: list / move / reset）\n";
    return 1;
}

// ---- 服务模式 ----
static int cmd_serve(Db& db, const Options& opt) {
    init_schema(db);
    // 启动时：每日自动备份 + 回收站 30 天清理
    {
        std::string err = backup::auto_daily(db);
        if (!err.empty()) std::cerr << "  自动备份失败: " << err << "\n";
        else std::cout << "✓ 每日备份完成（<库目录>/backups/）\n";
        db.exec("DELETE FROM tasks WHERE deleted_at IS NOT NULL "
                "AND deleted_at < datetime('now','localtime','-30 day')");
    }
    std::cout << "\n" << exporter::daily_digest(db) << "\n\n";

    HttpServer srv("web");
    Api api(db, "web");
    api.register_routes(srv);
    if (!srv.start(opt.port)) {
        std::cerr << "启动失败：端口 " << opt.port << " 被占用或不可用\n";
        return 1;
    }
    std::cout << "✓ todo 服务已启动（零账号 · 数据仅存本机）\n";
    std::cout << "  地址: http://127.0.0.1:" << opt.port << "/\n";
    std::cout << "  数据库: " << opt.db_path << "\n";
    std::cout << "  按 Ctrl+C 停止\n\n";
    if (opt.open_browser) {
#ifdef __APPLE__
        std::string cmd = "open http://127.0.0.1:" + std::to_string(opt.port) + "/";
        std::system(cmd.c_str());
#endif
    }
    // 提醒通知线程（20s 轮询 remind_time）
    std::atomic<bool> running{true};
    std::thread rt(reminder_thread, &db, &running);
    rt.detach();
    srv.run_loop();
    running = false;
    return 0;
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        print_help();
        return 0;
    }
    std::string cmd = args[0];
    std::vector<std::string> rest;
    // 剔除全局 flag（--db X / --port X / --open），它们不属于任何子命令
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--db" || a == "--port") { ++i; continue; } // 连同其值
        if (a == "--open") continue;
        rest.push_back(a);
    }

    Options opt;
    std::string db_flag;
    if (arg_has(args, "--db", db_flag)) opt.db_path = db_flag;
    else opt.db_path = storage::resolve_db_path(""); // 配置文件 > ./data/todo.db
    std::string port_s;
    if (arg_has(args, "--port", port_s)) opt.port = std::max(1, std::atoi(port_s.c_str()));
    opt.open_browser = arg_flag(args, "--open") || cmd == "open";

    if (cmd == "help" || cmd == "-h" || cmd == "--help") { print_help(); return 0; }

    // storage reset 只清配置，不触碰任何数据文件
    if (cmd == "storage" && !rest.empty() && rest[0] == "reset") {
        storage::clear_default_db();
        std::cout << "已清除默认路径配置，下次启动将使用 ./data/todo.db\n"
                     "（提示：可加 --db PATH 临时指定任意数据库文件）\n";
        return 0;
    }

    ensure_data_dir(opt.db_path);
    Db db(opt.db_path);
    if (!db.ok()) {
        std::cerr << "无法打开数据库: " << opt.db_path << " → " << db.error() << "\n";
        return 1;
    }
    init_schema(db);

    if (cmd == "serve" || cmd == "open") return cmd_serve(db, opt);
    if (cmd == "add") return cmd_add(db, rest);
    if (cmd == "list" || cmd == "ls") return cmd_list(db, rest);
    if (cmd == "done") return cmd_done_undo(db, rest, true);
    if (cmd == "undo") return cmd_done_undo(db, rest, false);
    if (cmd == "rm" || cmd == "del") {
        if (rest.empty()) { std::cerr << "错误: 缺少任务 ID\n"; return 1; }
        long long id = std::atoll(rest[0].c_str());
        if (arg_flag(rest, "--purge"))
            db.exec("DELETE FROM tasks WHERE id=" + std::to_string(id));
        else
            db.exec("UPDATE tasks SET deleted_at=datetime('now','localtime') "
                    "WHERE id=" + std::to_string(id));
        std::cout << (arg_flag(rest, "--purge") ? "已彻底删除 #" : "已移入回收站 #")
                  << id << "\n";
        return 0;
    }
    if (cmd == "restore") return cmd_restore(db, rest);
    if (cmd == "trash") return cmd_trash(db, rest);
    if (cmd == "export") return cmd_export(db, rest);
    if (cmd == "digest") return cmd_digest(db);
    if (cmd == "stats") return cmd_stats(db);
    if (cmd == "backup") return cmd_backup(db, rest);
    if (cmd == "tree") return cmd_tree(db);
    if (cmd == "calendar" || cmd == "cal") return cmd_calendar(db, rest);
    if (cmd == "projects") return cmd_projects(db);
    if (cmd == "tags") return cmd_tags(db);
    if (cmd == "import") return cmd_import(db, rest);
    if (cmd == "dep") return cmd_dep(db, rest, true);
    if (cmd == "undep") return cmd_dep(db, rest, false);
    if (cmd == "holiday") return cmd_holiday(db, rest);
    if (cmd == "storage") return cmd_storage(db, rest);

    std::cerr << "未知命令: " << cmd << "\n\n";
    print_help();
    return 1;
}

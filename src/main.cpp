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
        "cpp-todo " << kVersion << " — 本机 C++ 待办应用（SQLite，零账号零安装）\n\n"
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
        "  rm <id>                           删除\n"
        "  dep <id> <依赖id>                 设置依赖（依赖完成后才能开始）\n"
        "  undep <id> <依赖id>               移除依赖\n\n"
        "组织:\n"
        "  projects                         项目列表\n"
        "  calendar [--month YYYY-MM]       月历视图\n"
        "  tags                             标签列表\n"
        "  filter add \"名称\" \"条件JSON\"    保存筛选\n"
        "  filter list / filter rm <id>\n\n"
        "导入 (滴答/Todoist/Todo.txt):\n"
        "  import <文件> [--format todotxt|todoist|ticktick|csv] [--db PATH]\n\n"
        "节假日 (重复任务跳过):\n"
        "  holiday add YYYY-MM-DD / holiday list / holiday rm YYYY-MM-DD\n\n"
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
    auto row = db.query_one("SELECT * FROM tasks WHERE id=?", {std::to_string(id)});
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
    for (auto& r : db.query("SELECT id FROM tasks WHERE parent_id=? ORDER BY sort_order, id",
                            {parent == 0 ? "" : std::to_string(parent)})) {
        // parent==0 → 根任务
        kids.push_back(r.get_int("id"));
    }
    // 处理根任务（parent_id IS NULL）
    if (parent == 0) {
        for (auto& r : db.query(
                 "SELECT id FROM tasks WHERE parent_id IS NULL "
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
    std::string sql = "SELECT * FROM tasks WHERE 1=1";
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
    auto row = db.query_one("SELECT * FROM tasks WHERE id=?", {std::to_string(id)});
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
            "SELECT COUNT(*) c FROM tasks WHERE status!='done' AND "
            "(due_date=? OR start_date=? OR "
            "(repeat_rule!='' AND due_date IS NULL))", {d, d});
        long long c = cnt ? cnt->get_int("c") : 0;
        // 重复实例粗略统计
        long long rep_c = 0;
        for (auto& r : db.query("SELECT due_date,repeat_rule FROM tasks WHERE repeat_rule!='' AND status!='done'")) {
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
        "SELECT * FROM tasks WHERE status!='done' AND (due_date=? OR start_date=?) "
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
    if (args.empty()) { std::cerr << "用法: holiday add|list|rm <YYYY-MM-DD>\n"; return 1; }
    std::string op = args[0];
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
            "SELECT COUNT(*) c FROM tasks WHERE project_id=? AND status!='done'",
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

// ---- 存储位置管理 ----
static std::string human_size(long long bytes) {
    char buf[32];
    if (bytes >= 1LL << 30) std::snprintf(buf, sizeof buf, "%.1f GB", bytes / 1073741824.0);
    else if (bytes >= 1LL << 20) std::snprintf(buf, sizeof buf, "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
    else std::snprintf(buf, sizeof buf, "%lld B", bytes);
    return buf;
}

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
    HttpServer srv("web");
    Api api(db, "web");
    api.register_routes(srv);
    if (!srv.start(opt.port)) {
        std::cerr << "启动失败：端口 " << opt.port << " 被占用或不可用\n";
        return 1;
    }
    std::cout << "\n✓ cpp-todo 服务已启动（零账号 · 数据仅存本机）\n";
    std::cout << "  地址: http://127.0.0.1:" << opt.port << "/\n";
    std::cout << "  数据库: " << opt.db_path << "\n";
    std::cout << "  按 Ctrl+C 停止\n\n";
    if (opt.open_browser) {
#ifdef __APPLE__
        std::string cmd = "open http://127.0.0.1:" + std::to_string(opt.port) + "/";
        std::system(cmd.c_str());
#endif
    }
    srv.run_loop();
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
        db.exec("DELETE FROM tasks WHERE id=" + std::to_string(id));
        std::cout << "已删除 #" << id << "\n";
        return 0;
    }
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

// exporter.cpp — 导出与每日摘要实现
#include "exporter.hpp"
#include "json.hpp"
#include "lunar.hpp"

#include <cstdio>
#include <sstream>

namespace {

// CSV 字段转义
std::string csv_esc(const std::string& s) {
    bool need = s.find_first_of(",\"\n") != std::string::npos;
    std::string r;
    for (char c : s) {
        if (c == '"') r += "\"\"";
        else r += c;
    }
    return need ? "\"" + r + "\"" : r;
}

} // namespace

namespace exporter {

std::string export_tasks(Db& db, const std::string& format) {
    auto rows = db.query(
        "SELECT * FROM tasks WHERE deleted_at IS NULL "
        "ORDER BY CASE status WHEN 'done' THEN 1 ELSE 0 END, "
        "priority DESC, COALESCE(due_date,'9999-12-31'), sort_order, id");

    // 取任务标签
    auto tags_of = [&](long long id) {
        std::string r;
        for (auto& t : db.query(
                 "SELECT t.name FROM tags t JOIN task_tags tt ON t.id=tt.tag_id "
                 "WHERE tt.task_id=? ORDER BY t.name", {std::to_string(id)})) {
            std::string n = t.get("name");
            // Todo.txt 标签不能含空格
            for (auto& c : n) if (c == ' ' || c == '\t') c = '_';
            r += " @" + n;
        }
        return r;
    };
    auto project_of = [&](long long pid) {
        if (!pid) return std::string("");
        auto p = db.query_one("SELECT name FROM projects WHERE id=?",
                              {std::to_string(pid)});
        if (!p) return std::string("");
        std::string n = p->get("name");
        for (auto& c : n) if (c == ' ' || c == '\t') c = '_';
        return " +" + n;
    };

    if (format == "csv") {
        std::ostringstream ss;
        ss << "id,title,notes,priority,start_date,due_date,remind_time,status,"
           << "completed_at,project,tags,lunar_remind,lunar_date,repeat_rule,pomodoros\n";
        for (auto& r : rows) {
            std::string tags;
            for (auto& t : db.query(
                     "SELECT t.name FROM tags t JOIN task_tags tt ON t.id=tt.tag_id "
                     "WHERE tt.task_id=? ORDER BY t.name",
                     {std::to_string(r.get_int("id"))}))
                tags += (tags.empty() ? "" : ";") + t.get("name");
            auto p = r.get_int("project_id")
                ? db.query_one("SELECT name FROM projects WHERE id=?",
                               {std::to_string(r.get_int("project_id"))})
                : std::nullopt;
            ss << r.get_int("id") << ","
               << csv_esc(r.get("title")) << ","
               << csv_esc(r.get("notes")) << ","
               << r.get_int("priority") << ","
               << csv_esc(r.get("start_date")) << ","
               << csv_esc(r.get("due_date")) << ","
               << csv_esc(r.get("remind_time")) << ","
               << csv_esc(r.get("status")) << ","
               << csv_esc(r.get("completed_at")) << ","
               << csv_esc(p ? p->get("name") : "") << ","
               << csv_esc(tags) << ","
               << r.get_int("lunar_remind") << ","
               << csv_esc(r.get("lunar_date")) << ","
               << csv_esc(r.get("repeat_rule")) << ","
               << r.get_int("pomodoros") << "\n";
        }
        return ss.str();
    }

    if (format == "json") {
        // 复用 api.cpp 的结构（这里独立组装，避免循环依赖）
        std::ostringstream ss;
        ss << "{\"ok\":true,\"exportedAt\":\"" << lunar::today_iso() << "\",\"tasks\":[";
        bool first = true;
        for (auto& r : rows) {
            if (!first) ss << ",";
            first = false;
            auto p = r.get_int("project_id")
                ? db.query_one("SELECT name,color FROM projects WHERE id=?",
                               {std::to_string(r.get_int("project_id"))})
                : std::nullopt;
            Json j = Json::object();
            j["id"] = r.get_int("id");
            j["title"] = r.get("title");
            j["notes"] = r.get("notes");
            j["priority"] = r.get_int("priority");
            j["startDate"] = r.get("start_date");
            j["dueDate"] = r.get("due_date");
            j["remindTime"] = r.get("remind_time");
            j["status"] = r.get("status");
            j["completedAt"] = r.get("completed_at");
            j["projectId"] = r.get_int("project_id");
            j["projectName"] = p ? p->get("name") : "";
            j["lunarRemind"] = r.get_int("lunar_remind") != 0;
            j["lunarDate"] = r.get("lunar_date");
            j["repeatRule"] = r.get("repeat_rule");
            j["pomodoros"] = r.get_int("pomodoros");
            Json tags = Json::array();
            for (auto& t : db.query(
                     "SELECT t.name FROM tags t JOIN task_tags tt ON t.id=tt.tag_id "
                     "WHERE tt.task_id=? ORDER BY t.name",
                     {std::to_string(r.get_int("id"))}))
                tags.push_back(t.get("name"));
            j["tags"] = tags;
            ss << j.dump();
        }
        ss << "]}";
        return ss.str();
    }

    // 默认 Todo.txt:
    //   x 2026-01-15 2026-01-01 2026-01-15 标题 +项目 @标签
    //   （A/B/C 优先级、开始/截止日期、完成行以 x 开头）
    std::ostringstream ss;
    ss << "# cpp-todo 导出 " << lunar::today_iso() << "\n";
    for (auto& r : rows) {
        std::string line;
        std::string st = r.get("status");
        if (st == "done")
            line += "x " + r.get("completed_at", "").substr(0, 10) + " ";
        char prio = r.get_int("priority") == 2 ? 'A' : r.get_int("priority") == 0 ? 'C' : 'B';
        line += std::string("(") + prio + ") ";
        if (!r.get("start_date").empty()) line += r.get("start_date") + " ";
        if (!r.get("due_date").empty()) line += r.get("due_date") + " ";
        line += r.get("title");
        line += project_of(r.get_int("project_id"));
        line += tags_of(r.get_int("id"));
        ss << line << "\n";
    }
    return ss.str();
}

std::string daily_digest(Db& db) {
    std::string today = lunar::today_iso();
    auto one = [&](const std::string& cond) -> long long {
        auto r = db.query_one(
            "SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND status!='done' AND " + cond);
        return r ? r->get_int("c") : 0;
    };
    long long overdue = one("due_date IS NOT NULL AND due_date<'" + today + "'");
    long long due = one("due_date='" + today + "'");
    long long started = one("start_date='" + today + "'");
    auto done = db.query_one(
        "SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND status='done' "
        "AND completed_at LIKE '" + today + "%'");
    long long done_n = done ? done->get_int("c") : 0;

    int y = 0, m = 0, d = 0;
    std::sscanf(today.c_str(), "%d-%d-%d", &y, &m, &d);
    std::string lunar_str = lunar::solar_to_lunar(y, m, d).chinese;
    std::string term = lunar::solar_term(y, m, d);
    std::string holiday = lunar::statutory_holiday(y, m, d);

    std::ostringstream ss;
    ss << "今日概览 " << today << "（农历" << lunar_str << "）";
    if (!term.empty()) ss << " · " << term;
    if (!holiday.empty()) ss << " · " << holiday;
    ss << "\n  逾期 " << overdue << " · 今日到期 " << due
       << " · 今日开始 " << started << " · 今日已完成 " << done_n << "\n";
    auto rows = db.query(
        "SELECT title, priority, remind_time FROM tasks WHERE deleted_at IS NULL "
        "AND status!='done' AND (due_date='" + today + "' OR start_date='" + today +
        "') ORDER BY remind_time, priority DESC");
    if (!rows.empty()) {
        ss << "今日任务:\n";
        for (auto& r : rows) {
            ss << "  ☐ [" << (r.get_int("priority") == 2 ? "高" :
                              r.get_int("priority") == 0 ? "低" : "中") << "] "
               << r.get("title");
            if (!r.get("remind_time").empty()) ss << " ⏰" << r.get("remind_time");
            ss << "\n";
        }
    }
    return ss.str();
}

} // namespace exporter

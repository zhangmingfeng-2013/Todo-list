// api.cpp — REST API 实现
#include "api.hpp"
#include "json.hpp"
#include "lunar.hpp"
#include "recurrence.hpp"
#include "importer.hpp"
#include "storage.hpp"
#include "exporter.hpp"
#include "backup.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string esc_sql(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '\'') r += "''";
        else r += c;
    }
    return r;
}
std::string qstr(const std::string& s) { return "'" + esc_sql(s) + "'"; }

std::string now_local() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// 文件名友好的今日日期串（导出文件名用）
static std::string today_str_for_file() {
    std::string t = lunar::today_iso();
    std::string r;
    for (char c : t) if (c != '-') r += c;
    return r.empty() ? "today" : r;
}

// 农历日期 "M-D" -> 中文描述
std::string lunar_text(const std::string& lunar_date) {
    if (lunar_date.empty()) return "";
    int m = 0, d = 0;
    if (std::sscanf(lunar_date.c_str(), "%d-%d", &m, &d) != 2) return lunar_date;
    return lunar::month_name(m, false) + lunar::day_name(d);
}

// 是否为节假日（查 holidays 表）
class HolidayChecker {
public:
    explicit HolidayChecker(Db& db) : db_(db) {}
    bool operator()(const std::string& iso) const {
        return db_.query_one("SELECT 1 FROM holidays WHERE date=?", {iso}).has_value();
    }
private:
    Db& db_;
};

// ---- 撤销系统与同步的辅助函数（定义见下文） ----
Json task_snapshot_json(Db& db, long long id);
void record_undo(Db& db, const std::string& action, long long task_id,
                 const Json& before, long long after_id, const std::string& label);
Json build_full_snapshot(Db& db);

// ---- 任务 JSON 组装 ----
Json task_basic_json(Db& db, const Db::Row& r) {
    Json j = Json::object();
    long long id = r.get_int("id");
    j["id"] = id;
    j["title"] = r.get("title");
    j["notes"] = r.get("notes");
    j["priority"] = r.get_int("priority");
    j["startDate"] = r.get("start_date");
    j["dueDate"] = r.get("due_date");
    j["remindTime"] = r.get("remind_time");
    j["hasReminder"] = r.get_int("has_reminder") != 0;
    j["lunarRemind"] = r.get_int("lunar_remind") != 0;
    j["lunarDate"] = r.get("lunar_date");
    j["lunarText"] = lunar_text(r.get("lunar_date"));
    j["projectId"] = r.get_int("project_id");
    j["parentId"] = r.get_int("parent_id");
    j["sortOrder"] = r.get_int("sort_order");
    j["status"] = r.get("status");
    j["completedAt"] = r.get("completed_at");
    j["pomodoros"] = r.get_int("pomodoros");
    j["deletedAt"] = r.get("deleted_at");
    j["mood"] = r.get("mood");
    j["estMinutes"] = r.get_int("est_minutes");
    j["gaveUpAt"] = r.get("gave_up_at");
    std::string rr = r.get("repeat_rule");
    if (!rr.empty()) {
        try { j["repeatRule"] = Json::parse(rr); }
        catch (...) { j["repeatRule"] = Json::object(); }
    } else {
        j["repeatRule"] = Json::object();
    }
    j["createdAt"] = r.get("created_at");
    j["updatedAt"] = r.get("updated_at");

    long long pid = r.get_int("project_id");
    if (pid) {
        if (auto p = db.query_one("SELECT id,name,color FROM projects WHERE id=?",
                                  {std::to_string(pid)})) {
            Json pj = Json::object();
            pj["id"] = pid;
            pj["name"] = p->get("name");
            pj["color"] = p->get("color", "#4A90D9");
            j["project"] = pj;
        }
    }
    Json tags = Json::array();
    for (auto& t : db.query(
             "SELECT t.id,t.name,t.color FROM tags t JOIN task_tags tt ON t.id=tt.tag_id "
             "WHERE tt.task_id=? ORDER BY t.name",
             {std::to_string(id)})) {
        Json tj = Json::object();
        tj["id"] = t.get_int("id");
        tj["name"] = t.get("name");
        tj["color"] = t.get("color", "#8E8E93");
        tags.push_back(tj);
    }
    j["tags"] = tags;

    Json deps = Json::array();
    Json blockers = Json::array();
    bool blocked = false;
    for (auto& d : db.query(
             "SELECT t.id,t.title,t.status FROM task_dependencies td "
             "JOIN tasks t ON t.id=td.depends_on WHERE td.task_id=?",
             {std::to_string(id)})) {
        Json dj = Json::object();
        dj["id"] = d.get_int("id");
        dj["title"] = d.get("title");
        dj["status"] = d.get("status");
        deps.push_back(dj);
        if (d.get("status") != "done") {
            blocked = true;
            blockers.push_back(dj);
        }
    }
    j["dependsOn"] = deps;
    j["blockers"] = blockers;
    j["blocked"] = blocked;
    return j;
}

Json task_children_json(Db& db, long long parent_id) {
    Json arr = Json::array();
    for (auto& c : db.query(
             "SELECT * FROM tasks WHERE parent_id=? AND deleted_at IS NULL "
             "ORDER BY sort_order, id",
             {std::to_string(parent_id)})) {
        Json cj = task_basic_json(db, c);
        cj["children"] = task_children_json(db, c.get_int("id"));
        arr.push_back(cj);
    }
    return arr;
}

Json task_full_json(Db& db, long long id) {
    auto row = db.query_one("SELECT * FROM tasks WHERE id=?", {std::to_string(id)});
    if (!row) return Json();
    Json j = task_basic_json(db, *row);
    j["children"] = task_children_json(db, id);
    return j;
}

// 判断添加依赖是否成环（DFS）
// 添加 task_id 依赖 dep_id：若 dep_id 已（直接或间接）依赖 task_id，则成环
bool creates_cycle(Db& db, long long task_id, long long dep_id) {
    if (task_id == dep_id) return true;
    std::set<long long> visited;
    std::vector<long long> stack{dep_id};
    while (!stack.empty()) {
        long long cur = stack.back();
        stack.pop_back();
        for (auto& d : db.query(
                 "SELECT depends_on FROM task_dependencies WHERE task_id=?",
                 {std::to_string(cur)})) {
            long long nxt = d.get_int("depends_on");
            if (nxt == task_id) return true;
            if (!visited.count(nxt)) {
                visited.insert(nxt);
                stack.push_back(nxt);
            }
        }
    }
    return false;
}

// 解析任务体 JSON 并写入数据库（创建或更新）
Json apply_task_body(Db& db, const Json& body, long long id, bool& ok, std::string& err) {
    ok = true;
    auto upd = [&](const std::string& col, const Json& v) {
        if (id <= 0) return;
        std::string val;
        if (v.is_null()) val = "NULL";
        else if (v.is_number()) val = std::to_string(v.as_int());
        else if (v.is_bool()) val = v.as_bool() ? "1" : "0";
        else val = qstr(v.as_string());
        db.exec("UPDATE tasks SET " + col + "=" + val + " WHERE id=" + std::to_string(id));
    };

    if (id > 0) {
        // 撤销埋点：更新前先拍快照
        Json undo_before = task_snapshot_json(db, id);
        if (body.has("title")) upd("title", body["title"]);
        if (body.has("notes")) upd("notes", body["notes"]);
        if (body.has("priority")) upd("priority", body["priority"]);
        if (body.has("startDate")) {
            std::string v = body["startDate"].as_string_or("");
            upd("start_date", v.empty() ? Json(nullptr) : Json(v));
        }
        if (body.has("dueDate")) {
            std::string v = body["dueDate"].as_string_or("");
            upd("due_date", v.empty() ? Json(nullptr) : Json(v));
        }
        if (body.has("remindTime")) {
            std::string v = body["remindTime"].as_string_or("");
            upd("remind_time", v.empty() ? Json(nullptr) : Json(v));
        }
        if (body.has("hasReminder")) upd("has_reminder", body["hasReminder"]);
        if (body.has("lunarRemind")) upd("lunar_remind", body["lunarRemind"]);
        if (body.has("lunarDate")) {
            std::string v = body["lunarDate"].as_string_or("");
            upd("lunar_date", v.empty() ? Json(nullptr) : Json(v));
        }
        if (body.has("projectId")) {
            long long p = body["projectId"].as_int_or(0);
            upd("project_id", p ? Json(p) : Json(nullptr));
        }
        if (body.has("parentId")) {
            long long p = body["parentId"].as_int_or(0);
            upd("parent_id", p ? Json(p) : Json(nullptr));
        }
        if (body.has("status")) upd("status", body["status"]);
        if (body.has("sortOrder")) upd("sort_order", body["sortOrder"]);
        if (body.has("mood")) {
            std::string v = body["mood"].as_string_or("");
            upd("mood", v.empty() ? Json("") : Json(v));
        }
        if (body.has("estMinutes"))
            upd("est_minutes", Json(body["estMinutes"].as_int_or(0)));
        if (body.has("repeatRule")) {
            std::string v = body["repeatRule"].dump();
            upd("repeat_rule", v == "{}" ? Json("") : Json(v));
        }
        db.exec("UPDATE tasks SET updated_at=datetime('now','localtime') WHERE id=" +
                std::to_string(id));
        // 撤销埋点：更新 → 撤销即恢复快照（仅当实际有字段变更时记录）
        if (!undo_before.empty())
            record_undo(db, "update", id, undo_before, 0,
                        undo_before["title"].as_string_or(""));
    } else {
        // 创建
        std::string title = body["title"].as_string_or("");
        if (title.empty()) {
            ok = false;
            err = "标题不能为空";
            return Json();
        }
        std::string notes = body["notes"].as_string_or("");
        int prio = static_cast<int>(body["priority"].as_int_or(1));
        std::string start = body["startDate"].as_string_or("");
        std::string due = body["dueDate"].as_string_or("");
        std::string rtime = body["remindTime"].as_string_or("");
        int has_rem = body["hasReminder"].as_bool_or(!rtime.empty()) ? 1 : 0;
        int lunar_rem = body["lunarRemind"].as_bool_or(false) ? 1 : 0;
        std::string lunar_date = body["lunarDate"].as_string_or("");
        long long pid = body["projectId"].as_int_or(0);
        long long parent = body["parentId"].as_int_or(0);
        std::string status = body["status"].as_string_or("todo");
        std::string mood = body["mood"].as_string_or("");
        long long est_min = body["estMinutes"].as_int_or(0);
        std::string repeat_rule;
        if (body["repeatRule"].is_object() && !body["repeatRule"].empty()) {
            repeat_rule = body["repeatRule"].dump();
        }
        std::string sql = "INSERT INTO tasks(title,notes,priority,start_date,due_date,"
                          "remind_time,has_reminder,lunar_remind,lunar_date,project_id,"
                          "parent_id,status,mood,est_minutes,repeat_rule) VALUES(" +
                          qstr(title) + "," + qstr(notes) + "," + std::to_string(prio) + "," +
                          (start.empty() ? "NULL" : qstr(start)) + "," +
                          (due.empty() ? "NULL" : qstr(due)) + "," +
                          (rtime.empty() ? "NULL" : qstr(rtime)) + "," +
                          std::to_string(has_rem) + "," + std::to_string(lunar_rem) + "," +
                          (lunar_date.empty() ? "NULL" : qstr(lunar_date)) + "," +
                          (pid ? std::to_string(pid) : "NULL") + "," +
                          (parent ? std::to_string(parent) : "NULL") + "," +
                          qstr(status) + "," + qstr(mood) + "," +
                          std::to_string(est_min) + "," +
                          (repeat_rule.empty() ? "''" : qstr(repeat_rule)) + ")";
        db.exec(sql);
        id = db.last_insert_rowid();
        // 撤销埋点：新建任务 → 撤销即删除
        record_undo(db, "create", 0, Json::object(), id, title);
    }
    // 标签处理
    if (body["tags"].is_array()) {
        if (id > 0) db.exec("DELETE FROM task_tags WHERE task_id=" + std::to_string(id));
        for (auto& t : body["tags"]) {
            std::string name = t.as_string_or("");
            if (name.empty()) continue;
            auto row = db.query_one("SELECT id FROM tags WHERE name=?", {name});
            long long tid = 0;
            if (row) tid = row->get_int("id");
            else {
                db.exec("INSERT INTO tags(name) VALUES(" + qstr(name) + ")");
                tid = db.last_insert_rowid();
            }
            db.exec("INSERT OR IGNORE INTO task_tags(task_id,tag_id) VALUES(" +
                    std::to_string(id) + "," + std::to_string(tid) + ")");
        }
    }
    return task_full_json(db, id);
}

Json error_json(const std::string& msg) {
    Json j = Json::object();
    j["ok"] = false;
    j["error"] = msg;
    return j;
}

// ==================== 撤销系统 ====================

// 任务完整快照（含标签与依赖），用于撤销时恢复
Json task_snapshot_json(Db& db, long long id) {
    Json j = Json::object();
    auto row = db.query_one("SELECT * FROM tasks WHERE id=?", {std::to_string(id)});
    if (!row) return j;
    j["id"] = id;
    j["title"] = row->get("title");
    j["notes"] = row->get("notes");
    j["priority"] = row->get_int("priority");
    j["start_date"] = row->get("start_date");
    j["due_date"] = row->get("due_date");
    j["remind_time"] = row->get("remind_time");
    j["has_reminder"] = row->get_int("has_reminder");
    j["lunar_remind"] = row->get_int("lunar_remind");
    j["lunar_date"] = row->get("lunar_date");
    j["project_id"] = row->get_int("project_id");
    j["parent_id"] = row->get_int("parent_id");
    j["sort_order"] = row->get_int("sort_order");
    j["status"] = row->get("status");
    j["completed_at"] = row->get("completed_at");
    j["pomodoros"] = row->get_int("pomodoros");
    j["deleted_at"] = row->get("deleted_at");
    j["mood"] = row->get("mood");
    j["est_minutes"] = row->get_int("est_minutes");
    j["gave_up_at"] = row->get("gave_up_at");
    j["repeat_rule"] = row->get("repeat_rule");
    j["created_at"] = row->get("created_at");
    j["updated_at"] = row->get("updated_at");
    Json tags = Json::array();
    for (auto& t : db.query(
             "SELECT t.name FROM tags t JOIN task_tags tt ON t.id=tt.tag_id "
             "WHERE tt.task_id=? ORDER BY t.name", {std::to_string(id)}))
        tags.push_back(t.get("name"));
    j["tags"] = tags;
    Json deps = Json::array();
    for (auto& d : db.query(
             "SELECT depends_on FROM task_dependencies WHERE task_id=?",
             {std::to_string(id)}))
        deps.push_back(d.get_int("depends_on"));
    j["depends_on"] = deps;
    return j;
}

// 记录一条可撤销操作（快照在执行操作【前】采集）
void record_undo(Db& db, const std::string& action, long long task_id,
                 const Json& before, long long after_id, const std::string& label) {
    Json payload = Json::object();
    payload["before"] = before;
    db.exec("INSERT INTO undo_log(action,task_id,after_id,label,payload) VALUES(" +
            qstr(action) + "," + std::to_string(task_id) + "," +
            std::to_string(after_id) + "," + qstr(label) + "," +
            qstr(payload.dump()) + ")");
    // 环形上限：仅保留最近 200 条
    db.exec("DELETE FROM undo_log WHERE id <= (SELECT MAX(id) FROM undo_log) - 200");
}

// 用快照恢复任务（行不存在时按原 id 重建，用于撤销 purge）
static void restore_task_from_snapshot(Db& db, const Json& b) {
    long long id = b["id"].as_int_or(0);
    if (id <= 0) return;
    auto s = [&](const char* k) { return b[k].as_string_or(""); };
    auto n = [&](const char* k) { return std::to_string(b[k].as_int_or(0)); };
    auto val = [&](const std::string& v) { return v.empty() ? "NULL" : qstr(v); };
    auto nullable_id = [&](const char* k) {
        long long v = b[k].as_int_or(0);
        return v ? std::to_string(v) : std::string("NULL");
    };
    bool exists = db.query_one("SELECT 1 FROM tasks WHERE id=?",
                               {std::to_string(id)}).has_value();
    std::string fields =
        "title=" + qstr(s("title")) + ", notes=" + qstr(s("notes")) +
        ", priority=" + n("priority") +
        ", start_date=" + val(s("start_date")) + ", due_date=" + val(s("due_date")) +
        ", remind_time=" + val(s("remind_time")) + ", has_reminder=" + n("has_reminder") +
        ", lunar_remind=" + n("lunar_remind") + ", lunar_date=" + val(s("lunar_date")) +
        ", project_id=" + nullable_id("project_id") +
        ", sort_order=" + n("sort_order") + ", status=" + qstr(s("status")) +
        ", completed_at=" + val(s("completed_at")) + ", pomodoros=" + n("pomodoros") +
        ", deleted_at=" + val(s("deleted_at")) + ", mood=" + qstr(s("mood")) +
        ", est_minutes=" + n("est_minutes") + ", gave_up_at=" + val(s("gave_up_at")) +
        ", repeat_rule=" + qstr(s("repeat_rule"));
    if (exists) {
        db.exec("UPDATE tasks SET " + fields +
                ", parent_id=" + nullable_id("parent_id") +
                " WHERE id=" + std::to_string(id));
    } else {
        // 撤销彻底删除：按原 id 重建（先不带 parent，稍后二次修复避免外键顺序问题）
        db.exec("INSERT INTO tasks(id," + std::string(
                    "title,notes,priority,start_date,due_date,remind_time,has_reminder,"
                    "lunar_remind,lunar_date,project_id,parent_id,sort_order,status,"
                    "completed_at,pomodoros,deleted_at,mood,est_minutes,gave_up_at,"
                    "repeat_rule,created_at,updated_at) ") +
                "VALUES(" + std::to_string(id) + "," + qstr(s("title")) + "," +
                qstr(s("notes")) + "," + n("priority") + "," + val(s("start_date")) + "," +
                val(s("due_date")) + "," + val(s("remind_time")) + "," + n("has_reminder") +
                "," + n("lunar_remind") + "," + val(s("lunar_date")) + "," +
                nullable_id("project_id") + ",NULL," + n("sort_order") + "," +
                qstr(s("status")) + "," + val(s("completed_at")) + "," + n("pomodoros") +
                "," + val(s("deleted_at")) + "," + qstr(s("mood")) + "," + n("est_minutes") +
                "," + val(s("gave_up_at")) + "," + qstr(s("repeat_rule")) + "," +
                qstr(s("created_at")) + "," + qstr(s("updated_at")) + ")");
        long long par = b["parent_id"].as_int_or(0);
        if (par && db.query_one("SELECT 1 FROM tasks WHERE id=?",
                                {std::to_string(par)}).has_value())
            db.exec("UPDATE tasks SET parent_id=" + std::to_string(par) +
                    " WHERE id=" + std::to_string(id));
    }
    // 标签恢复
    db.exec("DELETE FROM task_tags WHERE task_id=" + std::to_string(id));
    if (b["tags"].is_array()) {
        for (auto& t : b["tags"]) {
            std::string name = t.as_string_or("");
            if (name.empty()) continue;
            auto row = db.query_one("SELECT id FROM tags WHERE name=?", {name});
            long long tid;
            if (row) tid = row->get_int("id");
            else {
                db.exec("INSERT INTO tags(name) VALUES(" + qstr(name) + ")");
                tid = db.last_insert_rowid();
            }
            db.exec("INSERT OR IGNORE INTO task_tags(task_id,tag_id) VALUES(" +
                    std::to_string(id) + "," + std::to_string(tid) + ")");
        }
    }
    // 依赖恢复
    db.exec("DELETE FROM task_dependencies WHERE task_id=" + std::to_string(id));
    if (b["depends_on"].is_array()) {
        for (auto& d : b["depends_on"]) {
            long long dep = d.as_int_or(0);
            if (dep <= 0 || dep == id) continue;
            if (!db.query_one("SELECT 1 FROM tasks WHERE id=?",
                              {std::to_string(dep)}).has_value())
                continue;
            db.exec("INSERT OR IGNORE INTO task_dependencies(task_id,depends_on) VALUES(" +
                    std::to_string(id) + "," + std::to_string(dep) + ")");
        }
    }
}

// 全量快照（多端同步 / 完整备份用）
Json build_full_snapshot(Db& db) {
    Json j = Json::object();
    j["app"] = "cpp-todo";
    j["snapshotVersion"] = static_cast<long long>(1);
    j["exportedAt"] = now_local();
    Json tasks = Json::array();
    for (auto& r : db.query("SELECT id FROM tasks ORDER BY id"))
        tasks.push_back(task_snapshot_json(db, r.get_int("id")));
    j["tasks"] = tasks;
    Json projects = Json::array();
    for (auto& p : db.query("SELECT * FROM projects ORDER BY id")) {
        Json pj = Json::object();
        pj["id"] = p.get_int("id");
        pj["name"] = p.get("name");
        pj["parentId"] = p.get_int("parent_id");
        pj["color"] = p.get("color", "#4A90D9");
        pj["isFolder"] = p.get_int("is_folder") != 0;
        pj["sortOrder"] = p.get_int("sort_order");
        projects.push_back(pj);
    }
    j["projects"] = projects;
    Json tags = Json::array();
    for (auto& t : db.query("SELECT * FROM tags ORDER BY id")) {
        Json tj = Json::object();
        tj["name"] = t.get("name");
        tj["color"] = t.get("color", "#8E8E93");
        tags.push_back(tj);
    }
    j["tags"] = tags;
    Json templates = Json::array();
    for (auto& t : db.query("SELECT * FROM templates ORDER BY id")) {
        Json tj = Json::object();
        tj["name"] = t.get("name");
        tj["body"] = t.get("body");
        templates.push_back(tj);
    }
    j["templates"] = templates;
    Json filters = Json::array();
    for (auto& f : db.query("SELECT * FROM saved_filters ORDER BY id")) {
        Json fj = Json::object();
        fj["name"] = f.get("name");
        fj["spec"] = f.get("spec");
        filters.push_back(fj);
    }
    j["filters"] = filters;
    Json hols = Json::array();
    for (auto& h : db.query("SELECT date FROM holidays ORDER BY date"))
        hols.push_back(h.get("date"));
    j["holidays"] = hols;
    return j;
}

// ==================== 自然语言快速录入解析 ====================
// 支持：今天/明天/后天/大后天/N天后/周X/下周X/星期X/礼拜X/M月D日/M月D号/MM-DD/YYYY-MM-DD
//       上午|下午|晚上|中午|早上H点 / H:MM / H点M分（可与日期组合，如「明天下午3点」）
//       #标签  !高|!中|!低|!2|!1|!0|!high  /项目名
struct QuickParse {
    std::string title;
    std::vector<std::string> tags;
    std::string projectName;
    int priority = -1;        // -1 = 未指定
    std::string dueDate;      // YYYY-MM-DD
    std::string remindTime;   // HH:MM
};

static bool has_prefix(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
static bool has_suffix(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
static std::string replace_all_str(std::string s, const std::string& a, const std::string& b) {
    size_t pos = 0;
    while ((pos = s.find(a, pos)) != std::string::npos) { s.replace(pos, a.size(), b); pos += b.size(); }
    return s;
}

// 公历天数（Howard Hinnant 算法，1970-01-01 = 0）
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static std::string iso_from_days(long z) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04ld-%02ld-%02ld", y, m, d);
    return buf;
}
static std::string fmt_iso(int y, int m, int d) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

// 中文数字（0-99）→ int；支持阿拉伯与「一~十/两」
static int cn_digit(const std::string& s) {
    const char* cn[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};
    for (int i = 0; i < 9; ++i) if (s == cn[i]) return i + 1;
    return 0;
}
static int parse_cn_int(const std::string& s) {
    if (s.empty()) return 0;
    bool digits = !s.empty();
    for (char c : s) if (c < '0' || c > '9') { digits = false; break; }
    if (digits) return std::atoi(s.c_str());
    if (s == "两") return 2;
    if (s == "十") return 10;
    size_t pos = s.find("十");
    if (pos != std::string::npos) {
        std::string a = s.substr(0, pos);
        std::string b = s.substr(pos + 3);
        int hi = a.empty() ? 1 : cn_digit(a);
        int lo = b.empty() ? 0 : cn_digit(b);
        if (hi > 0) return hi * 10 + lo;
    }
    return cn_digit(s);
}

// 星期几子串 → 1-7（周一=1，周日=7）；consumed = 消耗字节数
static int parse_weekday_str(const std::string& s, int& consumed) {
    consumed = 0;
    if (s.empty()) return 0;
    if (s[0] >= '1' && s[0] <= '7') { consumed = 1; return s[0] - '0'; }
    const char* names[] = {"一", "二", "三", "四", "五", "六", "日", "天"};
    for (int i = 0; i < 8; ++i) {
        std::string nm = names[i];
        if (s.compare(0, nm.size(), nm) == 0) {
            consumed = static_cast<int>(nm.size());
            return i == 7 ? 7 : i + 1;
        }
    }
    return 0;
}

// 本周（或下周）周 wd（1=周一..7=周日）的日期；本周已过去则顺延到下周
static std::string weekday_date(const std::string& today, int wd, bool nextWeek) {
    int y = 0, m = 0, d = 0;
    std::sscanf(today.c_str(), "%d-%d-%d", &y, &m, &d);
    long todayDays = days_from_civil(y, m, d);
    int dow = static_cast<int>((todayDays + 3) % 7) + 1;   // 周一=1（1970-01-01 为周四）
    long target = todayDays - (dow - 1) + (wd - 1) + (nextWeek ? 7 : 0);
    if (!nextWeek && target < todayDays) target += 7;
    return iso_from_days(target);
}

// 尝试解析时间 → HH:MM；支持 9:30 / 9点30 / 9点 / 上午9点 / 下午3点 / 晚上8点半 / 14:05
static bool try_time(const std::string& in, std::string& out) {
    std::string s = in;
    int add12 = 0;
    if (has_prefix(s, "下午") || has_prefix(s, "晚上") || has_prefix(s, "傍晚") ||
        has_prefix(s, "夜里")) { add12 = 12; s = s.substr(6); }
    else if (has_prefix(s, "中午")) { add12 = 12; s = s.substr(6); }
    else if (has_prefix(s, "上午") || has_prefix(s, "早上") || has_prefix(s, "早晨") ||
             has_prefix(s, "清晨") || has_prefix(s, "凌晨")) { s = s.substr(6); }
    s = replace_all_str(s, "：", ":");
    // 「X点半」→ 30 分
    size_t ban = s.find("半");
    if (ban != std::string::npos) {
        s = s.substr(0, ban) + "30";
        int h = -1;
        if (std::sscanf(s.c_str(), "%d点", &h) == 1) {
            if (h >= 0 && h <= 23) {
                if (add12 && h < 12) h += 12;
                char buf[8];
                std::snprintf(buf, sizeof buf, "%02d:30", h);
                out = buf;
                return true;
            }
        }
        return false;
    }
    int h = -1, m = 0;
    if (s.find(':') != std::string::npos) {
        if (std::sscanf(s.c_str(), "%d:%d", &h, &m) != 2) return false;
    } else if (s.find("点") != std::string::npos || s.find("时") != std::string::npos) {
        if (std::sscanf(s.c_str(), "%d点%d", &h, &m) != 2 &&
            std::sscanf(s.c_str(), "%d时%d", &h, &m) != 2) {
            m = 0;
            if (std::sscanf(s.c_str(), "%d点", &h) != 1 &&
                std::sscanf(s.c_str(), "%d时", &h) != 1) return false;
        }
    } else {
        return false;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    if (add12 && h < 12) h += 12;
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", h, m);
    out = buf;
    return true;
}

// 尝试从 token 头部解析日期；成功则返回日期并把 tok 缩减为剩余部分
static std::string try_date(std::string& tok, const std::string& today) {
    int ty = 0, tm_ = 0, td_ = 0;
    std::sscanf(today.c_str(), "%d-%d-%d", &ty, &tm_, &td_);
    std::string date;
    size_t used = 0;

    // N天后
    if (tok.size() > 6 && has_suffix(tok, "天后")) {
        int n = parse_cn_int(tok.substr(0, tok.size() - 6));
        if (n > 0 && n <= 3650) {
            date = lunar::add_days_iso(today, n);
            tok.clear();
            return date;
        }
    }
    if (has_prefix(tok, "大后天")) { date = lunar::add_days_iso(today, 3); used = 9; }
    else if (has_prefix(tok, "后天")) { date = lunar::add_days_iso(today, 2); used = 6; }
    else if (has_prefix(tok, "明天")) { date = lunar::add_days_iso(today, 1); used = 6; }
    else if (has_prefix(tok, "今天")) { date = today; used = 6; }
    else if (has_prefix(tok, "下周") || has_prefix(tok, "本周") || has_prefix(tok, "这周")) {
        int cu = 0;
        int wd = parse_weekday_str(tok.substr(6), cu);
        if (wd) { date = weekday_date(today, wd, has_prefix(tok, "下周")); used = 6 + cu; }
    } else if (has_prefix(tok, "周") || has_prefix(tok, "星期") || has_prefix(tok, "礼拜")) {
        size_t off = (has_prefix(tok, "星期") || has_prefix(tok, "礼拜")) ? 6 : 3;
        int cu = 0;
        int wd = parse_weekday_str(tok.substr(off), cu);
        if (wd) { date = weekday_date(today, wd, false); used = off + cu; }
    } else {
        // M月D日 / M月D号（可带后续时间，如「8月26日上午10点」）
        int m2 = 0, d2 = 0, n = 0;
        if (std::sscanf(tok.c_str(), "%d月%d日%n", &m2, &d2, &n) == 2 && n > 0) used = static_cast<size_t>(n);
        else if (std::sscanf(tok.c_str(), "%d月%d号%n", &m2, &d2, &n) == 2 && n > 0) used = static_cast<size_t>(n);
        if (used) {
            if (m2 < 1 || m2 > 12 || d2 < 1 || d2 > 31) {
                used = 0;
            } else {
                // 早于今天 180 天以上 → 视为明年
                if (days_from_civil(ty, m2, d2) < days_from_civil(ty, tm_, td_) - 180)
                    date = fmt_iso(ty + 1, m2, d2);
                else
                    date = fmt_iso(ty, m2, d2);
            }
        }
        if (!used) {
            // 整个 token 恰为 YYYY-MM-DD 或 MM-DD
            int y2 = 0;
            if (std::sscanf(tok.c_str(), "%d-%d-%d", &y2, &m2, &d2) == 3 &&
                y2 >= 1900 && y2 <= 2099 && m2 >= 1 && m2 <= 12 && d2 >= 1 && d2 <= 31) {
                date = fmt_iso(y2, m2, d2);
                used = tok.size();
            } else if (std::sscanf(tok.c_str(), "%d-%d", &m2, &d2) == 2 &&
                       m2 >= 1 && m2 <= 12 && d2 >= 1 && d2 <= 31 &&
                       tok.find_first_not_of("0123456789-") == std::string::npos) {
                if (days_from_civil(ty, m2, d2) < days_from_civil(ty, tm_, td_) - 180)
                    date = fmt_iso(ty + 1, m2, d2);
                else
                    date = fmt_iso(ty, m2, d2);
                used = tok.size();
            }
        }
    }
    if (used) tok = tok.substr(std::min(used, tok.size()));
    return date;
}

static QuickParse quick_parse(const std::string& text) {
    QuickParse r;
    std::string today = lunar::today_iso();
    std::istringstream ss(text);
    std::string tok;
    std::vector<std::string> titleParts;
    while (ss >> tok) {
        if (tok.size() > 1 && tok[0] == '#') { r.tags.push_back(tok.substr(1)); continue; }
        if (tok.size() > 1 && tok[0] == '!') {
            std::string v = tok.substr(1);
            if (v == "高" || v == "high" || v == "2" || v == "!!") r.priority = 2;
            else if (v == "低" || v == "low" || v == "0") r.priority = 0;
            else r.priority = 1;
            continue;
        }
        if (tok.size() > 1 && tok[0] == '/') { r.projectName = tok.substr(1); continue; }
        // 组合：日期前缀（可带时间后缀）
        {
            std::string rest = tok;
            std::string date = try_date(rest, today);
            if (!date.empty()) {
                r.dueDate = date;
                std::string tm;
                if (!rest.empty()) {
                    if (try_time(rest, tm)) r.remindTime = tm;
                    else titleParts.push_back(rest);
                }
                continue;
            }
        }
        // 纯时间
        {
            std::string tm;
            if (try_time(tok, tm)) { r.remindTime = tm; continue; }
        }
        titleParts.push_back(tok);
    }
    for (size_t i = 0; i < titleParts.size(); ++i) {
        if (i) r.title += " ";
        r.title += titleParts[i];
    }
    return r;
}

} // namespace

Api::Api(Db& db, const std::string& static_root) : db_(db), static_root_(static_root) {}

void Api::register_routes(HttpServer& srv) {
    // 静态资源由 HttpServer 兜底
    srv.on("GET", "/api/meta", [this](const HttpRequest& r) { return handle_meta(r); });
    srv.on("GET", "/api/tasks", [this](const HttpRequest& r) { return handle_tasks(r); });
    srv.on("POST", "/api/tasks", [this](const HttpRequest& r) { return handle_tasks(r); });
    // 精确路由必须先于 /api/tasks/* 通配注册（匹配按注册顺序）
    srv.on("POST", "/api/tasks/batch", [this](const HttpRequest& r) { return handle_batch(r); });
    srv.on("POST", "/api/tasks/reorder", [this](const HttpRequest& r) { return handle_reorder(r); });
    srv.on("GET", "/api/tasks/*", [this](const HttpRequest& r) { return handle_task_detail(r, 0); });
    srv.on("PUT", "/api/tasks/*", [this](const HttpRequest& r) { return handle_task_update(r, 0); });
    srv.on("DELETE", "/api/tasks/*", [this](const HttpRequest& r) { return handle_task_delete(r, 0); });
    srv.on("POST", "/api/tasks/*", [this](const HttpRequest& r) { return handle_task_detail(r, 0); });
    srv.on("GET", "/api/tree", [this](const HttpRequest& r) { return handle_tree(r); });
    srv.on("GET", "/api/today", [this](const HttpRequest& r) { return handle_today(r); });
    srv.on("GET", "/api/calendar", [this](const HttpRequest& r) { return handle_calendar(r); });
    srv.on("GET", "/api/kanban", [this](const HttpRequest& r) { return handle_kanban(r); });
    srv.on("GET", "/api/projects", [this](const HttpRequest& r) { return handle_projects(r); });
    srv.on("POST", "/api/projects", [this](const HttpRequest& r) { return handle_projects(r); });
    srv.on("PUT", "/api/projects/*", [this](const HttpRequest& r) { return handle_projects(r); });
    srv.on("DELETE", "/api/projects/*", [this](const HttpRequest& r) { return handle_projects(r); });
    srv.on("GET", "/api/tags", [this](const HttpRequest& r) { return handle_tags(r); });
    srv.on("POST", "/api/tags", [this](const HttpRequest& r) { return handle_tags(r); });
    srv.on("DELETE", "/api/tags/*", [this](const HttpRequest& r) { return handle_tags(r); });
    srv.on("GET", "/api/filters", [this](const HttpRequest& r) { return handle_filters(r); });
    srv.on("POST", "/api/filters", [this](const HttpRequest& r) { return handle_filters(r); });
    srv.on("PUT", "/api/filters/*", [this](const HttpRequest& r) { return handle_filters(r); });
    srv.on("DELETE", "/api/filters/*", [this](const HttpRequest& r) { return handle_filters(r); });
    srv.on("POST", "/api/import", [this](const HttpRequest& r) { return handle_import(r); });
    srv.on("GET", "/api/holidays", [this](const HttpRequest& r) { return handle_holidays(r); });
    srv.on("POST", "/api/holidays", [this](const HttpRequest& r) { return handle_holidays(r); });
    srv.on("DELETE", "/api/holidays/*", [this](const HttpRequest& r) { return handle_holidays(r); });
    srv.on("GET", "/api/search", [this](const HttpRequest& r) { return handle_search(r); });
    srv.on("GET", "/api/storage", [this](const HttpRequest& r) { return handle_storage(r); });
    srv.on("GET", "/api/storage/volumes", [this](const HttpRequest& r) { return handle_storage_volumes(r); });
    srv.on("POST", "/api/storage/move", [this](const HttpRequest& r) { return handle_storage_move(r); });
    // ---- 新增能力路由 ----
    srv.on("GET", "/api/trash", [this](const HttpRequest& r) { return handle_trash(r); });
    srv.on("DELETE", "/api/trash", [this](const HttpRequest& r) { return handle_trash(r); });
    srv.on("GET", "/api/stats", [this](const HttpRequest& r) { return handle_stats(r); });
    srv.on("GET", "/api/export", [this](const HttpRequest& r) { return handle_export(r); });
    srv.on("GET", "/api/backups", [this](const HttpRequest& r) { return handle_backups(r); });
    srv.on("POST", "/api/backups", [this](const HttpRequest& r) { return handle_backups(r); });
    srv.on("POST", "/api/holidays/auto", [this](const HttpRequest& r) { return handle_holidays_auto(r); });
    srv.on("GET", "/api/digest", [this](const HttpRequest& r) { return handle_digest(r); });
    // ---- 第二批功能 ----
    srv.on("POST", "/api/quick-add", [this](const HttpRequest& r) { return handle_quick_add(r); });
    srv.on("GET", "/api/templates", [this](const HttpRequest& r) { return handle_templates(r); });
    srv.on("POST", "/api/templates", [this](const HttpRequest& r) { return handle_templates(r); });
    srv.on("DELETE", "/api/templates/*", [this](const HttpRequest& r) { return handle_template_detail(r); });
    srv.on("POST", "/api/templates/*", [this](const HttpRequest& r) { return handle_template_detail(r); });
    srv.on("GET", "/api/heatmap", [this](const HttpRequest& r) { return handle_heatmap(r); });
    // ---- 批次 B/C 功能 ----
    srv.on("POST", "/api/undo", [this](const HttpRequest& r) { return handle_undo(r); });
    srv.on("GET", "/api/undo", [this](const HttpRequest& r) { return handle_undo(r); });
    srv.on("POST", "/api/repeat-preview", [this](const HttpRequest& r) { return handle_repeat_preview(r); });
    srv.on("GET", "/api/day", [this](const HttpRequest& r) { return handle_day(r); });
    srv.on("POST", "/api/sync", [this](const HttpRequest& r) { return handle_sync(r); });
    // ---- 心理健康批次 ----
    srv.on("GET", "/api/journal", [this](const HttpRequest& r) { return handle_journal(r); });
    // ---- WebDAV 同步 ----
    srv.on("GET", "/api/webdav-config", [this](const HttpRequest& r) { return handle_webdav_config(r); });
    srv.on("PUT", "/api/webdav-config", [this](const HttpRequest& r) { return handle_webdav_config(r); });
    srv.on("POST", "/api/webdav-sync", [this](const HttpRequest& r) { return handle_webdav_sync(r); });
}

// 从 /api/tasks/123 这类路径中取 id（由 handler 传 0 占位后解析）
static long long path_id(const HttpRequest& req, size_t skip = 2) {
    // /api/tasks/123/complete → 取第 skip 段
    std::vector<std::string> segs;
    std::string cur;
    for (char c : req.path) {
        if (c == '/') { if (!cur.empty()) { segs.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) segs.push_back(cur);
    if (segs.size() > skip) {
        try { return std::stoll(segs[skip]); } catch (...) { return 0; }
    }
    return 0;
}

// 取 /api/tasks/{id}/xxx 的 action
static std::string path_action(const HttpRequest& req) {
    std::vector<std::string> segs;
    std::string cur;
    for (char c : req.path) {
        if (c == '/') { if (!cur.empty()) { segs.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) segs.push_back(cur);
    return segs.size() > 3 ? segs[3] : "";
}

HttpResponse Api::handle_meta(const HttpRequest&) {
    Json j = Json::object();
    j["app"] = "cpp-todo";
    j["version"] = "1.0.0";
    j["today"] = lunar::today_iso();
    int y = 0, m = 0, d = 0;
    std::sscanf(lunar::today_iso().c_str(), "%d-%d-%d", &y, &m, &d);
    auto lt = lunar::solar_to_lunar(y, m, d);
    j["lunarToday"] = lt.chinese;
    return HttpResponse::json(200, j.dump());
}

// ---- 存储位置管理 ----

HttpResponse Api::handle_storage(const HttpRequest&) {
    std::lock_guard<std::recursive_mutex> lk(Db::mutex());
    Json j = Json::object();
    j["dbPath"] = db_.path();
    j["sizeBytes"] = storage::db_file_size(db_.path());
    j["configPath"] = storage::config_path();
    std::string conf = storage::load_default_db();
    j["configuredDefault"] = conf.empty() ? "" : conf;
    j["usingConfig"] = !conf.empty() && conf == db_.path();
    return HttpResponse::json(200, j.dump());
}

HttpResponse Api::handle_storage_volumes(const HttpRequest&) {
    Json arr = Json::array();
    for (auto& v : storage::list_volumes()) {
        Json jv = Json::object();
        jv["name"] = v.name;
        jv["path"] = v.path;
        jv["totalBytes"] = v.totalBytes;
        jv["freeBytes"] = v.freeBytes;
        jv["writable"] = v.writable;
        jv["removable"] = v.removable;
        arr.push_back(jv);
    }
    Json j = Json::object();
    j["volumes"] = arr;
    return HttpResponse::json(200, j.dump());
}

HttpResponse Api::handle_storage_move(const HttpRequest& req) {
    Json body;
    try { body = Json::parse(req.body); }
    catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
    std::string target = body.get_str("path");
    if (target.empty())
        return HttpResponse::json(400, error_json("缺少目标路径（path）").dump());
    bool overwrite = body.get_bool("overwrite");

    std::string new_path, err;
    if (!storage::migrate_db(db_, target, new_path, err, overwrite)) {
        int code = err.find("需确认覆盖") != std::string::npos ? 409 : 400;
        return HttpResponse::json(code, error_json(err).dump());
    }
    Json j = Json::object();
    j["ok"] = true;
    j["newDbPath"] = new_path;
    j["sizeBytes"] = storage::db_file_size(new_path);
    j["note"] = "迁移完成，原数据库文件保留为备份；已写入默认路径配置";
    return HttpResponse::json(200, j.dump());
}

HttpResponse Api::handle_tasks(const HttpRequest& req) {
    if (req.method == "POST") {
        Json body;
        try { body = Json::parse(req.body); }
        catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
        bool ok = false;
        std::string err;
        Json j = apply_task_body(db_, body, 0, ok, err);
        if (!ok) return HttpResponse::json(400, error_json(err).dump());
        Json resp = Json::object();
        resp["ok"] = true;
        resp["task"] = j;
        return HttpResponse::json(201, resp.dump());
    }

    // GET 列表 + 筛选（默认排除回收站；status=trash 查看回收站）
    std::string sql;
    std::vector<std::string> params;

    std::string status = req.q("status", "all");
    if (status == "trash")
        sql = "SELECT * FROM tasks WHERE deleted_at IS NOT NULL";
    else
        sql = "SELECT * FROM tasks WHERE deleted_at IS NULL";
    if (status != "all" && !status.empty() && status != "trash")
        sql += " AND status=" + qstr(status);

    std::string project = req.q("project");
    if (!project.empty() && project != "0")
        sql += " AND project_id=" + project;

    std::string tag = req.q("tag");
    if (!tag.empty())
        sql += " AND EXISTS(SELECT 1 FROM task_tags tt JOIN tags t ON t.id=tt.tag_id "
               "WHERE tt.task_id=tasks.id AND t.name=" + qstr(tag) + ")";

    std::string prio = req.q("priority");
    if (!prio.empty())
        sql += " AND priority>=" + prio;

    // 情绪标签筛选（hard=费力 annoying=烦躁 easy=轻松 excited=期待）
    std::string mood = req.q("mood");
    if (!mood.empty())
        sql += " AND mood=" + qstr(mood);

    std::string q = req.q("q");
    if (!q.empty())
        sql += " AND (title LIKE " + qstr("%" + q + "%") + " OR notes LIKE " +
               qstr("%" + q + "%") + ")";

    std::string due_before = req.q("due_before");
    if (!due_before.empty()) sql += " AND due_date<=" + qstr(due_before);
    std::string due_after = req.q("due_after");
    if (!due_after.empty()) sql += " AND due_date>=" + qstr(due_after);

    std::string due_within = req.q("due_within");
    if (!due_within.empty()) {
        int n = std::max(0, std::atoi(due_within.c_str()));
        std::string to = lunar::add_days_iso(lunar::today_iso(), n);
        sql += " AND due_date IS NOT NULL AND due_date>='" + lunar::today_iso() +
               "' AND due_date<=" + qstr(to);
    }

    std::string parent = req.q("parent");
    if (parent == "0")
        sql += " AND parent_id IS NULL";
    else if (!parent.empty())
        sql += " AND parent_id=" + parent;

    sql += " ORDER BY CASE status WHEN 'done' THEN 1 ELSE 0 END, priority DESC, "
           "COALESCE(due_date,'9999-12-31'), sort_order, id";

    Json arr = Json::array();
    for (auto& r : db_.query(sql, params)) {
        Json j = task_basic_json(db_, r);
        if (req.q("include_children") == "1")
            j["children"] = task_children_json(db_, r.get_int("id"));
        arr.push_back(j);
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["count"] = static_cast<long long>(arr.size());
    resp["tasks"] = arr;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_task_detail(const HttpRequest& req, long long) {
    long long id = path_id(req, 2);
    std::string action = path_action(req);
    if (action == "complete")
        return handle_task_complete(req, id);
    if (action == "reopen")
        return handle_task_reopen(req, id);
    if (action == "restore")
        return handle_task_restore(req, id);
    if (action == "pomodoro")
        return handle_pomodoro(req, id);
    if (action == "deps")
        return handle_deps(req, id);
    if (action == "giveup")
        return handle_task_giveup(req, id);
    if (id <= 0) return HttpResponse::json(404, error_json("任务不存在").dump());
    if (req.method == "POST" && action.empty()) {
        // 以 POST 更新（兼容表单场景）→ 走 PUT 逻辑
        return handle_task_update(req, id);
    }
    Json j = task_full_json(db_, id);
    if (j.is_null()) return HttpResponse::json(404, error_json("任务不存在").dump());
    return HttpResponse::json(200, j.dump());
}

HttpResponse Api::handle_task_update(const HttpRequest& req, long long id) {
    id = path_id(req, 2);
    if (id <= 0) return HttpResponse::json(404, error_json("任务不存在").dump());
    Json body;
    try { body = Json::parse(req.body); }
    catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
    if (db_.query_one("SELECT 1 FROM tasks WHERE id=?", {std::to_string(id)}).has_value() == false)
        return HttpResponse::json(404, error_json("任务不存在").dump());
    bool ok = false;
    std::string err;
    Json j = apply_task_body(db_, body, id, ok, err);
    if (!ok) return HttpResponse::json(400, error_json(err).dump());
    Json resp = Json::object();
    resp["ok"] = true;
    resp["task"] = j;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_task_delete(const HttpRequest& req, long long) {
    long long id = path_id(req, 2);
    if (id <= 0) return HttpResponse::json(404, error_json("任务不存在").dump());
    auto row = db_.query_one("SELECT title FROM tasks WHERE id=?", {std::to_string(id)});
    if (!row) return HttpResponse::json(404, error_json("任务不存在").dump());
    Json before = task_snapshot_json(db_, id);
    if (req.q("purge") == "1") {
        db_.exec("DELETE FROM tasks WHERE id=" + std::to_string(id));
        record_undo(db_, "purge", id, before, 0, row->get("title"));
    } else {
        // 软删除 → 回收站（30 天后启动时自动清理）
        db_.exec("UPDATE tasks SET deleted_at=" + qstr(now_local()) +
                 ", updated_at=datetime('now','localtime') WHERE id=" + std::to_string(id));
        record_undo(db_, "delete", id, before, 0, row->get("title"));
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["purged"] = req.q("purge") == "1";
    return HttpResponse::json(200, resp.dump());
}

// 从回收站恢复
HttpResponse Api::handle_task_restore(const HttpRequest& req, long long) {
    long long id = path_id(req, 2);
    if (id <= 0) return HttpResponse::json(404, error_json("任务不存在").dump());
    auto row = db_.query_one("SELECT title FROM tasks WHERE id=?", {std::to_string(id)});
    if (!row) return HttpResponse::json(404, error_json("任务不存在").dump());
    Json before = task_snapshot_json(db_, id);
    db_.exec("UPDATE tasks SET deleted_at=NULL, updated_at=datetime('now','localtime') "
             "WHERE id=" + std::to_string(id));
    record_undo(db_, "restore", id, before, 0, row->get("title"));
    Json resp = Json::object();
    resp["ok"] = true;
    resp["task"] = task_full_json(db_, id);
    return HttpResponse::json(200, resp.dump());
}

// 回收站列表 / 清空
HttpResponse Api::handle_trash(const HttpRequest& req) {
    if (req.method == "DELETE") {
        db_.exec("DELETE FROM tasks WHERE deleted_at IS NOT NULL");
        Json resp = Json::object();
        resp["ok"] = true;
        return HttpResponse::json(200, resp.dump());
    }
    Json arr = Json::array();
    for (auto& r : db_.query(
             "SELECT * FROM tasks WHERE deleted_at IS NOT NULL ORDER BY deleted_at DESC")) {
        Json j = task_basic_json(db_, r);
        arr.push_back(j);
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["count"] = static_cast<long long>(arr.size());
    resp["tasks"] = arr;
    return HttpResponse::json(200, resp.dump());
}

// 批量操作：{action, ids, projectId?, tag?, status?}
HttpResponse Api::handle_batch(const HttpRequest& req) {
    Json body;
    try { body = Json::parse(req.body); }
    catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
    std::string action = body.get_str("action");
    Json ids = body["ids"];
    if (action.empty() || !ids.is_array() || ids.empty())
        return HttpResponse::json(400, error_json("需要 action 与非空 ids 数组").dump());

    std::string in_list;
    for (auto& v : ids) in_list += std::to_string(v.as_int()) + ",";
    if (!in_list.empty()) in_list.pop_back();
    std::string where = "id IN (" + in_list + ")";

    int affected = 0;
    if (action == "complete") {
        db_.exec("UPDATE tasks SET status='done', completed_at=" + qstr(now_local()) +
                 ", updated_at=datetime('now','localtime') WHERE " + where +
                 " AND deleted_at IS NULL");
    } else if (action == "reopen") {
        db_.exec("UPDATE tasks SET status='todo', completed_at=NULL, "
                 "updated_at=datetime('now','localtime') WHERE " + where +
                 " AND deleted_at IS NULL");
    } else if (action == "delete") {
        db_.exec("UPDATE tasks SET deleted_at=" + qstr(now_local()) +
                 " WHERE " + where + " AND deleted_at IS NULL");
    } else if (action == "purge") {
        db_.exec("DELETE FROM tasks WHERE " + where);
    } else if (action == "restore") {
        db_.exec("UPDATE tasks SET deleted_at=NULL WHERE " + where);
    } else if (action == "move") {
        long long pid = body["projectId"].as_int_or(0);
        db_.exec("UPDATE tasks SET project_id=" + (pid ? std::to_string(pid) : "NULL") +
                 ", updated_at=datetime('now','localtime') WHERE " + where +
                 " AND deleted_at IS NULL");
    } else if (action == "status") {
        std::string st = body.get_str("status");
        if (st != "todo" && st != "doing" && st != "done" && st != "archived")
            return HttpResponse::json(400, error_json("无效状态").dump());
        db_.exec("UPDATE tasks SET status=" + qstr(st) +
                 ", updated_at=datetime('now','localtime') WHERE " + where +
                 " AND deleted_at IS NULL");
    } else if (action == "tag") {
        std::string name = body.get_str("tag");
        if (name.empty())
            return HttpResponse::json(400, error_json("缺少 tag").dump());
        auto row = db_.query_one("SELECT id FROM tags WHERE name=?", {name});
        long long tid;
        if (row) tid = row->get_int("id");
        else {
            db_.exec("INSERT INTO tags(name) VALUES(" + qstr(name) + ")");
            tid = db_.last_insert_rowid();
        }
        bool remove = body.get_bool("remove");
        for (auto& v : ids) {
            long long id = v.as_int();
            if (remove)
                db_.exec("DELETE FROM task_tags WHERE task_id=" + std::to_string(id) +
                         " AND tag_id=" + std::to_string(tid));
            else
                db_.exec("INSERT OR IGNORE INTO task_tags(task_id,tag_id) VALUES(" +
                         std::to_string(id) + "," + std::to_string(tid) + ")");
        }
    } else {
        return HttpResponse::json(400, error_json("未知操作: " + action).dump());
    }
    affected = db_.changes();
    Json resp = Json::object();
    resp["ok"] = true;
    resp["affected"] = affected;
    return HttpResponse::json(200, resp.dump());
}

// 手动排序：{ids: [按新顺序]}
HttpResponse Api::handle_reorder(const HttpRequest& req) {
    Json body;
    try { body = Json::parse(req.body); }
    catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
    Json ids = body["ids"];
    if (!ids.is_array() || ids.empty())
        return HttpResponse::json(400, error_json("缺少 ids 数组").dump());
    int i = 0;
    for (auto& v : ids) {
        db_.exec("UPDATE tasks SET sort_order=" + std::to_string(i) +
                 " WHERE id=" + std::to_string(v.as_int()));
        ++i;
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["count"] = i;
    return HttpResponse::json(200, resp.dump());
}

// 番茄钟：完成一个番茄，计数 +1
HttpResponse Api::handle_pomodoro(const HttpRequest& req, long long) {
    long long id = path_id(req, 2);
    if (id <= 0) return HttpResponse::json(404, error_json("任务不存在").dump());
    if (db_.query_one("SELECT 1 FROM tasks WHERE id=?", {std::to_string(id)}).has_value() == false)
        return HttpResponse::json(404, error_json("任务不存在").dump());
    db_.exec("UPDATE tasks SET pomodoros=pomodoros+1 WHERE id=" + std::to_string(id));
    auto r = db_.query_one("SELECT pomodoros FROM tasks WHERE id=?", {std::to_string(id)});
    Json resp = Json::object();
    resp["ok"] = true;
    resp["pomodoros"] = r ? r->get_int("pomodoros") : 0;
    return HttpResponse::json(200, resp.dump());
}

// 统计仪表盘数据
HttpResponse Api::handle_stats(const HttpRequest& req) {
    std::string today = lunar::today_iso();
    Json resp = Json::object();
    resp["ok"] = true;
    resp["today"] = today;

    auto count = [&](const std::string& cond) -> long long {
        auto r = db_.query_one("SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND " + cond);
        return r ? r->get_int("c") : 0;
    };
    Json totals = Json::object();
    totals["todo"] = count("status='todo'");
    totals["doing"] = count("status='doing'");
    totals["done"] = count("status='done'");
    totals["archived"] = count("status='archived'");
    totals["overdue"] = count("status!='done' AND due_date IS NOT NULL AND due_date<'" + today + "'");
    totals["dueToday"] = count("status!='done' AND due_date='" + today + "'");
    long long all = totals["todo"].as_int() + totals["doing"].as_int() +
                    totals["done"].as_int() + totals["archived"].as_int();
    totals["all"] = all;
    resp["totals"] = totals;

    // 优先级分布（未完成）
    Json prio = Json::array();
    for (auto& r : db_.query(
             "SELECT priority, COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND status!='done' "
             "GROUP BY priority ORDER BY priority DESC")) {
        Json p = Json::object();
        p["priority"] = r.get_int("priority");
        p["count"] = r.get_int("c");
        prio.push_back(p);
    }
    resp["priorityDist"] = prio;

    // 最近 14 天完成趋势
    Json trend = Json::array();
    for (int i = 13; i >= 0; --i) {
        std::string d = lunar::add_days_iso(today, -i);
        auto r = db_.query_one(
            "SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND status='done' "
            "AND completed_at LIKE '" + d + "%'");
        Json t = Json::object();
        t["date"] = d;
        t["count"] = r ? r->get_int("c") : 0;
        trend.push_back(t);
    }
    resp["trend"] = trend;

    // 连续打卡天数（连续每天至少完成 1 个）
    int streak = 0;
    for (int i = 0; i < 3650; ++i) {
        std::string d = lunar::add_days_iso(today, -i);
        auto r = db_.query_one(
            "SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND status='done' "
            "AND completed_at LIKE '" + d + "%'");
        long long c = r ? r->get_int("c") : 0;
        if (c > 0) ++streak;
        else if (i > 0) break;   // 今天没完成不打断连续记录
    }
    resp["streak"] = streak;

    // 项目进度
    Json projects = Json::array();
    for (auto& p : db_.query(
             "SELECT p.id,p.name,p.color,"
             "SUM(CASE WHEN t.status!='done' THEN 1 ELSE 0 END) open,"
             "SUM(CASE WHEN t.status='done' THEN 1 ELSE 0 END) done "
             "FROM projects p LEFT JOIN tasks t ON t.project_id=p.id AND t.deleted_at IS NULL "
             "GROUP BY p.id ORDER BY open DESC")) {
        Json pj = Json::object();
        pj["id"] = p.get_int("id");
        pj["name"] = p.get("name");
        pj["color"] = p.get("color", "#4A90D9");
        pj["open"] = p.get_int("open");
        pj["done"] = p.get_int("done");
        projects.push_back(pj);
    }
    resp["projects"] = projects;

    // 番茄钟总计
    auto pomo = db_.query_one(
        "SELECT COALESCE(SUM(pomodoros),0) s FROM tasks WHERE deleted_at IS NULL");
    resp["pomodoros"] = pomo ? pomo->get_int("s") : 0;
    return HttpResponse::json(200, resp.dump());
}

// 导出下载：?format=todotxt|json|csv|backup（backup = 多端同步全量快照）
HttpResponse Api::handle_export(const HttpRequest& req) {
    std::string format = req.q("format", "todotxt");
    if (format == "backup") {
        Json j = build_full_snapshot(db_);
        HttpResponse r;
        r.status = 200;
        r.content_type = "application/json; charset=utf-8";
        r.body = j.dump();
        r.headers["Content-Disposition"] =
            "attachment; filename=\"todo-backup-" + today_str_for_file() + ".json\"";
        return r;
    }
    if (format != "todotxt" && format != "json" && format != "csv")
        return HttpResponse::json(400, error_json("format 应为 todotxt|json|csv|backup").dump());
    std::string data = exporter::export_tasks(db_, format);
    HttpResponse r;
    r.status = 200;
    if (format == "json") r.content_type = "application/json; charset=utf-8";
    else if (format == "csv") r.content_type = "text/csv; charset=utf-8";
    else r.content_type = "text/plain; charset=utf-8";
    r.body = std::move(data);
    r.headers["Content-Disposition"] =
        "attachment; filename=\"todo-export-" + today_str_for_file() + "." + format + "\"";
    return r;
}

// 备份：GET 列表 / POST 立即备份
HttpResponse Api::handle_backups(const HttpRequest& req) {
    if (req.method == "POST") {
        std::string err = backup::backup_now(db_);
        if (!err.empty()) return HttpResponse::json(500, error_json(err).dump());
        Json resp = Json::object();
        resp["ok"] = true;
        resp["backupsDir"] = "（数据库所在目录）/backups";
        return HttpResponse::json(200, resp.dump());
    }
    Json arr = Json::array();
    for (auto& b : backup::list_backups(db_)) {
        Json j = Json::object();
        j["name"] = b.name;
        j["path"] = b.path;
        j["sizeBytes"] = b.sizeBytes;
        arr.push_back(j);
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["count"] = static_cast<long long>(arr.size());
    resp["backups"] = arr;
    return HttpResponse::json(200, resp.dump());
}

// 生成某年法定节假日（含农历/节气推导）；?year= 缺省今年
HttpResponse Api::handle_holidays_auto(const HttpRequest& req) {
    int year = std::atoi(req.q("year", "0").c_str());
    if (year == 0) {
        int y = 0, m = 0, d = 0;
        std::sscanf(lunar::today_iso().c_str(), "%d-%d-%d", &y, &m, &d);
        year = y;
    }
    if (year < 1901 || year > 2099)
        return HttpResponse::json(400, error_json("年份应在 1901-2099").dump());

    int added = 0;
    Json days = Json::array();
    // 遍历全年，statutory_holiday 命中即入库
    for (int m = 1; m <= 12; ++m) {
        int dim = 31;
        if (m == 4 || m == 6 || m == 9 || m == 11) dim = 30;
        if (m == 2) dim = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29 : 28;
        for (int d = 1; d <= dim; ++d) {
            std::string name = lunar::statutory_holiday(year, m, d);
            if (name.empty()) continue;
            char iso[16];
            std::snprintf(iso, sizeof iso, "%04d-%02d-%02d", year, m, d);
            db_.exec("INSERT OR IGNORE INTO holidays(date) VALUES(" + qstr(iso) + ")");
            ++added;
            Json j = Json::object();
            j["date"] = iso;
            j["name"] = name;
            days.push_back(j);
        }
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["year"] = year;
    resp["added"] = added;
    resp["holidays"] = days;
    return HttpResponse::json(200, resp.dump());
}

// 每日摘要
HttpResponse Api::handle_digest(const HttpRequest& req) {
    Json resp = Json::object();
    resp["ok"] = true;
    resp["digest"] = exporter::daily_digest(db_);
    return HttpResponse::json(200, resp.dump());
}

// 自然语言快速录入；?preview=1 仅解析不落库
HttpResponse Api::handle_quick_add(const HttpRequest& req) {
    Json body;
    try { body = Json::parse(req.body); }
    catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
    std::string text = body["text"].as_string_or("");
    if (text.empty()) return HttpResponse::json(400, error_json("text 不能为空").dump());

    QuickParse qp = quick_parse(text);
    if (qp.title.empty()) return HttpResponse::json(400, error_json("未能解析出任务标题").dump());

    // 项目解析：按名称匹配已有项目（找不到则忽略）
    long long pid = 0;
    bool projMatched = false;
    if (!qp.projectName.empty()) {
        if (auto p = db_.query_one("SELECT id FROM projects WHERE name=?", {qp.projectName})) {
            pid = p->get_int("id");
            projMatched = true;
        }
    }

    Json parsed = Json::object();
    parsed["title"] = qp.title;
    parsed["priority"] = qp.priority < 0 ? 1 : qp.priority;
    parsed["dueDate"] = qp.dueDate;
    parsed["remindTime"] = qp.remindTime;
    parsed["tags"] = Json::array();
    for (auto& t : qp.tags) parsed["tags"].push_back(Json(t));
    parsed["project"] = qp.projectName;
    parsed["projectMatched"] = projMatched;

    if (req.q("preview") == "1") {
        Json resp = Json::object();
        resp["ok"] = true;
        resp["parsed"] = parsed;
        return HttpResponse::json(200, resp.dump());
    }

    Json tb = Json::object();
    tb["title"] = qp.title;
    if (qp.priority >= 0) tb["priority"] = static_cast<long long>(qp.priority);
    if (!qp.dueDate.empty()) tb["dueDate"] = qp.dueDate;
    if (!qp.remindTime.empty()) {
        tb["remindTime"] = qp.remindTime;
        tb["hasReminder"] = true;
    }
    if (pid) tb["projectId"] = pid;
    if (!qp.tags.empty()) {
        Json tags = Json::array();
        for (auto& t : qp.tags) tags.push_back(Json(t));
        tb["tags"] = tags;
    }
    bool ok = false;
    std::string err;
    Json task = apply_task_body(db_, tb, 0, ok, err);
    if (!ok) return HttpResponse::json(400, error_json(err).dump());
    Json resp = Json::object();
    resp["ok"] = true;
    resp["task"] = task;
    resp["parsed"] = parsed;
    return HttpResponse::json(201, resp.dump());
}

// 模板：GET 列表 / POST 保存
HttpResponse Api::handle_templates(const HttpRequest& req) {
    if (req.method == "POST") {
        Json body;
        try { body = Json::parse(req.body); }
        catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
        Json tb = body["body"];
        if (!tb.is_object() || tb["title"].as_string_or("").empty())
            return HttpResponse::json(400, error_json("body.title 不能为空").dump());
        std::string name = body["name"].as_string_or("");
        if (name.empty()) name = tb["title"].as_string_or("");
        db_.exec("INSERT INTO templates(name,body) VALUES(" + qstr(name) + "," + qstr(tb.dump()) + ")");
        Json resp = Json::object();
        resp["ok"] = true;
        resp["id"] = db_.last_insert_rowid();
        resp["name"] = name;
        return HttpResponse::json(201, resp.dump());
    }
    Json arr = Json::array();
    for (auto& r : db_.query("SELECT * FROM templates ORDER BY id DESC")) {
        Json j = Json::object();
        j["id"] = r.get_int("id");
        j["name"] = r.get("name");
        try { j["body"] = Json::parse(r.get("body")); }
        catch (...) { j["body"] = Json::object(); }
        j["createdAt"] = r.get("created_at");
        arr.push_back(j);
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["count"] = static_cast<long long>(arr.size());
    resp["templates"] = arr;
    return HttpResponse::json(200, resp.dump());
}

// 模板详情：DELETE 删除 / POST /api/templates/{id}/apply 实例化为任务
HttpResponse Api::handle_template_detail(const HttpRequest& req) {
    long long id = path_id(req, 2);
    if (id <= 0) return HttpResponse::json(404, error_json("模板不存在").dump());
    auto row = db_.query_one("SELECT * FROM templates WHERE id=" + std::to_string(id));
    if (!row) return HttpResponse::json(404, error_json("模板不存在").dump());
    if (req.method == "DELETE") {
        db_.exec("DELETE FROM templates WHERE id=" + std::to_string(id));
        Json resp = Json::object();
        resp["ok"] = true;
        return HttpResponse::json(200, resp.dump());
    }
    // POST → 实例化
    Json body;
    try { body = Json::parse(row->get("body")); }
    catch (...) { return HttpResponse::json(500, error_json("模板数据损坏").dump()); }
    if (!body.is_object()) return HttpResponse::json(500, error_json("模板数据损坏").dump());
    // 相对天数 → 实际日期（每次使用模板时按当天换算）
    std::string today = lunar::today_iso();
    if (body["dueOffsetDays"].is_number())
        body["dueDate"] = lunar::add_days_iso(today, static_cast<int>(body["dueOffsetDays"].as_int()));
    if (body["startOffsetDays"].is_number())
        body["startDate"] = lunar::add_days_iso(today, static_cast<int>(body["startOffsetDays"].as_int()));
    bool ok = false;
    std::string err;
    Json task = apply_task_body(db_, body, 0, ok, err);
    if (!ok) return HttpResponse::json(400, error_json(err).dump());
    Json resp = Json::object();
    resp["ok"] = true;
    resp["task"] = task;
    return HttpResponse::json(201, resp.dump());
}

// 年度完成热力图：?year= 缺省今年
HttpResponse Api::handle_heatmap(const HttpRequest& req) {
    std::string today = lunar::today_iso();
    int year = std::atoi(req.q("year", today.substr(0, 4)).c_str());
    if (year < 1900 || year > 2100)
        return HttpResponse::json(400, error_json("年份应在 1900-2100").dump());
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04d", year);
    std::string from = std::string(buf) + "-01-01";
    std::snprintf(buf, sizeof buf, "%04d", year + 1);
    std::string to = std::string(buf) + "-01-01";
    Json days = Json::array();
    long long maxc = 0, total = 0;
    for (auto& r : db_.query(
             "SELECT substr(completed_at,1,10) d, COUNT(*) c FROM tasks "
             "WHERE deleted_at IS NULL AND status='done' AND completed_at IS NOT NULL "
             "AND completed_at>=" + qstr(from) + " AND completed_at<" + qstr(to) + " GROUP BY d")) {
        Json d = Json::object();
        d["date"] = r.get("d");
        d["count"] = r.get_int("c");
        if (r.get_int("c") > maxc) maxc = r.get_int("c");
        total += r.get_int("c");
        days.push_back(d);
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["year"] = static_cast<long long>(year);
    resp["days"] = days;
    resp["max"] = maxc;
    resp["total"] = total;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_task_complete(const HttpRequest& req, long long id) {
    auto row = db_.query_one("SELECT * FROM tasks WHERE id=? AND deleted_at IS NULL", {std::to_string(id)});
    if (!row) return HttpResponse::json(404, error_json("任务不存在").dump());
    // 情绪日记：完成时可附带 {feeling, note}（body 可为空）
    std::string feeling, note;
    if (!req.body.empty()) {
        try {
            Json b = Json::parse(req.body);
            feeling = b["feeling"].as_string_or("");
            note = b["note"].as_string_or("");
        } catch (...) { /* body 不是 JSON 时按无日记处理 */ }
    }
    Json undo_before = task_snapshot_json(db_, id);
    db_.exec("UPDATE tasks SET status='done', completed_at=" + qstr(now_local()) +
             " WHERE id=" + std::to_string(id));
    // 写入成就感日志（标题做快照）
    if (!feeling.empty() || !note.empty()) {
        db_.exec("INSERT INTO completion_log(task_id,title,feeling,note,completed_at) VALUES(" +
                 std::to_string(id) + "," + qstr(row->get("title")) + "," + qstr(feeling) +
                 "," + qstr(note) + "," + qstr(now_local()) + ")");
    }
    Json created = Json::object();
    long long next_id = 0;
    // 重复任务：生成下一次实例
    std::string rr = row->get("repeat_rule");
    if (!rr.empty()) {
        try {
            RepeatRule rule = RepeatRule::from_json(Json::parse(rr));
            if (rule.enabled()) {
                HolidayChecker hc(db_);
                std::string base = row->get("due_date");
                std::string next = recurrence::next_after(rule, base, hc);
                if (!next.empty() &&
                    (rule.end_date.empty() || next <= rule.end_date)) {
                    Json body = Json::object();
                    body["title"] = row->get("title");
                    body["notes"] = row->get("notes");
                    body["priority"] = static_cast<long long>(row->get_int("priority"));
                    body["startDate"] = "";
                    body["dueDate"] = next;
                    body["remindTime"] = row->get("remind_time");
                    body["hasReminder"] = row->get_int("has_reminder") != 0;
                    body["lunarRemind"] = row->get_int("lunar_remind") != 0;
                    body["lunarDate"] = row->get("lunar_date");
                    long long pid = row->get_int("project_id");
                    body["projectId"] = pid;
                    long long par = row->get_int("parent_id");
                    if (par) {
                        auto pr = db_.query_one("SELECT status FROM tasks WHERE id=?",
                                                {std::to_string(par)});
                        if (pr && pr->get("status") == "done") par = 0;
                    }
                    body["parentId"] = par;
                    body["status"] = "todo";
                    body["repeatRule"] = Json::parse(rr);
                    body["tags"] = Json::array();
                    for (auto& t : db_.query(
                             "SELECT t.name FROM tags t JOIN task_tags tt ON t.id=tt.tag_id "
                             "WHERE tt.task_id=?",
                             {std::to_string(id)}))
                        body["tags"].push_back(t.get("name"));
                    bool ok = false;
                    std::string err;
                    Json nj = apply_task_body(db_, body, 0, ok, err);
                    if (ok) {
                        created = nj;
                        next_id = nj["id"].as_int_or(0);
                    }
                }
            }
        } catch (...) {}
    }
    // 撤销埋点：完成 → 撤销即恢复完成前状态（下一实例由其自身的 create 记录单独撤销）
    record_undo(db_, "complete", id, undo_before, next_id, row->get("title"));
    Json resp = Json::object();
    resp["ok"] = true;
    resp["nextInstance"] = created;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_task_reopen(const HttpRequest& req, long long id) {
    auto row = db_.query_one("SELECT title FROM tasks WHERE id=?", {std::to_string(id)});
    if (!row) return HttpResponse::json(404, error_json("任务不存在").dump());
    Json before = task_snapshot_json(db_, id);
    db_.exec("UPDATE tasks SET status='todo', completed_at=NULL WHERE id=" +
             std::to_string(id));
    record_undo(db_, "reopen", id, before, 0, row->get("title"));
    Json resp = Json::object();
    resp["ok"] = true;
    return HttpResponse::json(200, resp.dump());
}

// ---- 愧疚阻断：正式放弃任务 ----
// 多次未完成的任务不再弹窗轰炸，而是允许用户主动「放下」：
// 状态改为 archived 并记录 gave_up_at，正式归档，从活跃清单中消失。
HttpResponse Api::handle_task_giveup(const HttpRequest& req, long long id) {
    auto row = db_.query_one("SELECT * FROM tasks WHERE id=? AND deleted_at IS NULL",
                             {std::to_string(id)});
    if (!row) return HttpResponse::json(404, error_json("任务不存在").dump());
    Json before = task_snapshot_json(db_, id);
    db_.exec("UPDATE tasks SET status='archived', gave_up_at=" + qstr(now_local()) +
             ", updated_at=datetime('now','localtime') WHERE id=" + std::to_string(id));
    record_undo(db_, "giveup", id, before, 0, row->get("title"));
    Json resp = Json::object();
    resp["ok"] = true;
    resp["message"] = "已正式放下「" + row->get("title") + "」。放下不是失败，"
                      "是把精力留给更重要的事。";
    return HttpResponse::json(200, resp.dump());
}

// ---- 情绪日记：成就感日志 + 已放下的事 ----
HttpResponse Api::handle_journal(const HttpRequest&) {
    Json entries = Json::array();
    for (auto& r : db_.query(
             "SELECT * FROM completion_log ORDER BY completed_at DESC, id DESC LIMIT 500")) {
        Json j = Json::object();
        j["id"] = r.get_int("id");
        j["taskId"] = r.get_int("task_id");
        j["title"] = r.get("title");
        j["feeling"] = r.get("feeling");
        j["note"] = r.get("note");
        j["completedAt"] = r.get("completed_at");
        entries.push_back(j);
    }
    Json given_up = Json::array();
    for (auto& r : db_.query(
             "SELECT id,title,mood,est_minutes,gave_up_at,completed_at FROM tasks "
             "WHERE gave_up_at IS NOT NULL ORDER BY gave_up_at DESC")) {
        Json j = Json::object();
        j["id"] = r.get_int("id");
        j["title"] = r.get("title");
        j["mood"] = r.get("mood");
        j["estMinutes"] = r.get_int("est_minutes");
        j["gaveUpAt"] = r.get("gave_up_at");
        given_up.push_back(j);
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["entries"] = entries;
    resp["givenUp"] = given_up;
    resp["streakTip"] = "每一条记录都是你认真生活过的证据。";
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_deps(const HttpRequest& req, long long id) {
    // 支持 POST /api/tasks/{id}/deps  {"dependsOn": 123}
    // 和 DELETE /api/tasks/{id}/deps/{dep}
    if (req.method == "DELETE") {
        long long dep = path_id(req, 4);
        db_.exec("DELETE FROM task_dependencies WHERE task_id=" + std::to_string(id) +
                 " AND depends_on=" + std::to_string(dep));
        Json resp = Json::object();
        resp["ok"] = true;
        return HttpResponse::json(200, resp.dump());
    }
    Json body;
    try { body = Json::parse(req.body); }
    catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
    long long dep = body["dependsOn"].as_int_or(body["depends_on"].as_int_or(0));
    if (dep <= 0 || dep == id)
        return HttpResponse::json(400, error_json("无效的依赖任务").dump());
    if (creates_cycle(db_, id, dep))
        return HttpResponse::json(409, error_json("依赖关系将形成循环，已拒绝").dump());
    db_.exec("INSERT OR IGNORE INTO task_dependencies(task_id, depends_on) VALUES(" +
             std::to_string(id) + "," + std::to_string(dep) + ")");
    Json resp = Json::object();
    resp["ok"] = true;
    resp["task"] = task_full_json(db_, id);
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_tree(const HttpRequest& req) {
    std::string status = req.q("status", "all");
    std::string sql = "SELECT * FROM tasks WHERE deleted_at IS NULL";
    if (status != "all") sql += " AND status=" + qstr(status);
    sql += " ORDER BY priority DESC, COALESCE(due_date,'9999-12-31'), sort_order, id";
    auto rows = db_.query(sql);

    std::map<long long, Json> by_id;
    std::vector<long long> roots;
    for (auto& r : rows) {
        long long id = r.get_int("id");
        by_id[id] = task_basic_json(db_, r);
        by_id[id]["children"] = Json::array();
        long long par = r.get_int("parent_id");
        if (par && by_id.count(par)) {} // 稍后挂载
    }
    for (auto& r : rows) {
        long long id = r.get_int("id");
        long long par = r.get_int("parent_id");
        if (par && by_id.count(par)) {
            by_id[par]["children"].push_back(by_id[id]);
        } else {
            roots.push_back(id);
        }
    }
    Json arr = Json::array();
    for (long long id : roots) arr.push_back(by_id[id]);
    Json resp = Json::object();
    resp["ok"] = true;
    resp["tree"] = arr;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_today(const HttpRequest& req) {
    std::string today = lunar::today_iso();
    Json resp = Json::object();
    resp["ok"] = true;
    resp["today"] = today;

    auto query_day = [&](const std::string& cond) {
        Json arr = Json::array();
        std::string sql = "SELECT * FROM tasks WHERE deleted_at IS NULL AND status!='done' AND " + cond +
                          " ORDER BY priority DESC, COALESCE(due_date,'9999-12-31'), sort_order, id";
        for (auto& r : db_.query(sql)) arr.push_back(task_basic_json(db_, r));
        return arr;
    };

    resp["overdue"] = query_day("due_date IS NOT NULL AND due_date<'" + today + "'");
    resp["dueToday"] = query_day("due_date='" + today + "'");
    resp["startToday"] = query_day("start_date='" + today + "'");
    resp["noDate"] = query_day("due_date IS NULL AND start_date IS NULL");
    // 今日已完成
    Json done = Json::array();
    for (auto& r : db_.query(
             "SELECT * FROM tasks WHERE deleted_at IS NULL AND status='done' AND completed_at LIKE '" + today + "%' "
             "ORDER BY completed_at DESC")) {
        done.push_back(task_basic_json(db_, r));
    }
    resp["doneToday"] = done;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_calendar(const HttpRequest& req) {
    // 支持 start/end 任意区间（周视图）；缺省按 year/month 整月
    std::string range_first = req.q("start");
    std::string range_last = req.q("end");
    bool has_range = !range_first.empty() && !range_last.empty() &&
                     range_first <= range_last;

    int year = std::atoi(req.q("year", "0").c_str());
    int month = std::atoi(req.q("month", "0").c_str());
    if (year == 0 || month < 1 || month > 12) {
        int y = 0, m = 0, d = 0;
        std::sscanf(lunar::today_iso().c_str(), "%d-%d-%d", &y, &m, &d);
        year = y; month = m;
    }
    char first[16];
    std::snprintf(first, sizeof first, "%04d-%02d-01", year, month);
    std::string next_first = (month == 12)
        ? (std::to_string(year + 1) + "-01-01")
        : ([&]() {
              char b[16];
              std::snprintf(b, sizeof b, "%04d-%02d-01", year, month + 1);
              return std::string(b);
          })();
    std::string first_iso, last;
    if (has_range) {
        first_iso = range_first;
        last = range_last;
    } else {
        first_iso = first;
        last = lunar::add_days_iso(next_first, -1);
    }

    HolidayChecker hc(db_);
    std::map<std::string, Json> day_map;
    std::string d = first_iso;
    for (int guard = 0; guard < 62 && d <= last; ++guard, d = lunar::add_days_iso(d, 1)) {
        Json dj = Json::object();
        dj["date"] = d;
        int y2 = 0, m2 = 0, dd2 = 0;
        std::sscanf(d.c_str(), "%d-%d-%d", &y2, &m2, &dd2);
        auto lt = lunar::solar_to_lunar(y2, m2, dd2);
        dj["lunar"] = lt.month == 0 ? "" : (lunar::month_name(lt.month, lt.isLeap) +
                                            lunar::day_name(lt.day));
        dj["isHoliday"] = hc(d);
        dj["term"] = lunar::solar_term(y2, m2, dd2);
        dj["holidayName"] = lunar::statutory_holiday(y2, m2, dd2);
        dj["tasks"] = Json::array();
        day_map[d] = dj;
    }

    // 当天到期/开始的任务
    for (auto& r : db_.query(
             "SELECT * FROM tasks WHERE deleted_at IS NULL AND status!='done' AND "
             "(due_date BETWEEN '" + first_iso + "' AND '" + last + "' OR "
             " start_date BETWEEN '" + first_iso + "' AND '" + last + "')")) {
        std::string due = r.get("due_date");
        if (day_map.count(due)) day_map[due]["tasks"].push_back(
            [&]() {
                Json e = Json::object();
                e["task"] = task_basic_json(db_, r);
                e["kind"] = "due";
                return e;
            }());
        std::string start = r.get("start_date");
        if (day_map.count(start) && start != due) day_map[start]["tasks"].push_back(
            [&]() {
                Json e = Json::object();
                e["task"] = task_basic_json(db_, r);
                e["kind"] = "start";
                return e;
            }());
    }

    // 重复任务实例
    for (auto& r : db_.query("SELECT * FROM tasks WHERE deleted_at IS NULL AND status!='done' AND repeat_rule!=''")) {
        RepeatRule rule = RepeatRule::from_json_str(r.get("repeat_rule"));
        if (!rule.enabled()) continue;
        auto occ = recurrence::occurrences_in_range(rule, first_iso, last, hc);
        for (auto& o : occ) {
            if (day_map.count(o)) day_map[o]["tasks"].push_back(
                [&]() {
                    Json e = Json::object();
                    Json tj = task_basic_json(db_, r);
                    tj["recurringInstance"] = true;
                    e["task"] = tj;
                    e["kind"] = "due";
                    return e;
                }());
        }
    }

    // 农历提醒任务：把农历日期映射到本月
    for (auto& r : db_.query(
             "SELECT * FROM tasks WHERE deleted_at IS NULL AND status!='done' AND lunar_remind=1 AND lunar_date!=''")) {
        std::string ld = r.get("lunar_date");
        int lm = 0, lday = 0;
        if (std::sscanf(ld.c_str(), "%d-%d", &lm, &lday) != 2) continue;
        // 尝试本年与本年后一年
        for (int try_y = year - 1; try_y <= year + 1; ++try_y) {
            SolarDate s = lunar::lunar_to_solar(try_y, lm, lday, false);
            if (s.year != 0 && s.iso >= first_iso && s.iso <= last && day_map.count(s.iso)) {
                day_map[s.iso]["tasks"].push_back(
                    [&]() {
                        Json e = Json::object();
                        Json tj = task_basic_json(db_, r);
                        tj["lunarInstance"] = true;
                        e["task"] = tj;
                        e["kind"] = "lunar";
                        return e;
                    }());
                break;
            }
        }
    }

    Json days = Json::array();
    d = first_iso;
    for (int guard = 0; guard < 62 && d <= last; ++guard, d = lunar::add_days_iso(d, 1))
        days.push_back(day_map[d]);

    Json resp = Json::object();
    resp["ok"] = true;
    resp["year"] = year;
    resp["month"] = month;
    resp["days"] = days;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_kanban(const HttpRequest& req) {
    std::string sql = "SELECT * FROM tasks WHERE deleted_at IS NULL AND status IN ('todo','doing','done')";
    std::string tag = req.q("tag");
    if (!tag.empty())
        sql += " AND EXISTS(SELECT 1 FROM task_tags tt JOIN tags t ON t.id=tt.tag_id "
               "WHERE tt.task_id=tasks.id AND t.name=" + qstr(tag) + ")";
    std::string project = req.q("project");
    if (!project.empty() && project != "0")
        sql += " AND project_id=" + project;
    sql += " ORDER BY priority DESC, COALESCE(due_date,'9999-12-31'), sort_order, id";
    Json cols = Json::object();
    cols["todo"] = Json::array();
    cols["doing"] = Json::array();
    cols["done"] = Json::array();
    for (auto& r : db_.query(sql)) {
        std::string st = r.get("status");
        if (st != "todo" && st != "doing" && st != "done") continue;
        Json j = task_basic_json(db_, r);
        j["children"] = task_children_json(db_, r.get_int("id"));
        cols[st].push_back(j);
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["columns"] = cols;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_projects(const HttpRequest& req) {
    if (req.method == "POST") {
        Json body;
        try { body = Json::parse(req.body); }
        catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
        std::string name = body["name"].as_string_or("");
        if (name.empty()) return HttpResponse::json(400, error_json("项目名不能为空").dump());
        long long parent = body["parentId"].as_int_or(0);
        db_.exec("INSERT INTO projects(name,parent_id,color) VALUES(" + qstr(name) + "," +
                 (parent ? std::to_string(parent) : "NULL") + "," +
                 qstr(body["color"].as_string_or("#4A90D9")) + ")");
        Json resp = Json::object();
        resp["ok"] = true;
        resp["id"] = static_cast<long long>(db_.last_insert_rowid());
        return HttpResponse::json(201, resp.dump());
    }
    if (req.method == "PUT" || req.method == "DELETE") {
        long long id = path_id(req, 2);
        if (id <= 0) return HttpResponse::json(404, error_json("项目不存在").dump());
        if (req.method == "DELETE") {
            db_.exec("DELETE FROM projects WHERE id=" + std::to_string(id));
            Json resp = Json::object();
            resp["ok"] = true;
            return HttpResponse::json(200, resp.dump());
        }
        Json body;
        try { body = Json::parse(req.body); }
        catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
        if (body.has("name"))
            db_.exec("UPDATE projects SET name=" + qstr(body["name"].as_string_or("")) +
                     " WHERE id=" + std::to_string(id));
        if (body.has("color"))
            db_.exec("UPDATE projects SET color=" + qstr(body["color"].as_string_or("#4A90D9")) +
                     " WHERE id=" + std::to_string(id));
        if (body.has("parentId")) {
            long long p = body["parentId"].as_int_or(0);
            db_.exec("UPDATE projects SET parent_id=" +
                     (p ? std::to_string(p) : "NULL") + " WHERE id=" + std::to_string(id));
        }
        Json resp = Json::object();
        resp["ok"] = true;
        return HttpResponse::json(200, resp.dump());
    }
    // GET：项目树 + 计数
    Json arr = Json::array();
    for (auto& p : db_.query("SELECT * FROM projects ORDER BY sort_order, name")) {
        Json pj = Json::object();
        pj["id"] = p.get_int("id");
        pj["name"] = p.get("name");
        pj["color"] = p.get("color", "#4A90D9");
        pj["parentId"] = p.get_int("parent_id");
        pj["isFolder"] = p.get_int("is_folder") != 0;
        auto cnt = db_.query_one(
            "SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND project_id=? AND status!='done'",
            {std::to_string(p.get_int("id"))});
        pj["taskCount"] = cnt ? cnt->get_int("c") : 0;
        arr.push_back(pj);
    }
    // 未分类
    auto unc = db_.query_one("SELECT COUNT(*) c FROM tasks WHERE deleted_at IS NULL AND project_id IS NULL AND status!='done'");
    Json inbox = Json::object();
    inbox["id"] = 0;
    inbox["name"] = "未分类";
    inbox["color"] = "#9AA0A6";
    inbox["parentId"] = 0;
    inbox["isFolder"] = false;
    inbox["taskCount"] = unc ? unc->get_int("c") : 0;
    Json resp = Json::object();
    resp["ok"] = true;
    resp["projects"] = arr;
    resp["inbox"] = inbox;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_tags(const HttpRequest& req) {
    if (req.method == "POST") {
        Json body;
        try { body = Json::parse(req.body); }
        catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
        std::string name = body["name"].as_string_or("");
        if (name.empty()) return HttpResponse::json(400, error_json("标签名不能为空").dump());
        auto row = db_.query_one("SELECT id FROM tags WHERE name=?", {name});
        long long id;
        if (row) id = row->get_int("id");
        else {
            db_.exec("INSERT INTO tags(name) VALUES(" + qstr(name) + ")");
            id = db_.last_insert_rowid();
        }
        Json resp = Json::object();
        resp["ok"] = true;
        resp["id"] = id;
        return HttpResponse::json(201, resp.dump());
    }
    if (req.method == "DELETE") {
        long long id = path_id(req, 2);
        db_.exec("DELETE FROM tags WHERE id=" + std::to_string(id));
        Json resp = Json::object();
        resp["ok"] = true;
        return HttpResponse::json(200, resp.dump());
    }
    Json arr = Json::array();
    for (auto& t : db_.query(
             "SELECT t.id,t.name,t.color,COUNT(tt.task_id) c FROM tags t "
             "LEFT JOIN task_tags tt ON tt.tag_id=t.id "
             "LEFT JOIN tasks ts ON ts.id=tt.task_id AND ts.status!='done' AND ts.deleted_at IS NULL "
             "GROUP BY t.id ORDER BY c DESC, t.name")) {
        Json tj = Json::object();
        tj["id"] = t.get_int("id");
        tj["name"] = t.get("name");
        tj["color"] = t.get("color", "#8E8E93");
        tj["count"] = t.get_int("c");
        arr.push_back(tj);
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["tags"] = arr;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_filters(const HttpRequest& req) {
    if (req.method == "POST") {
        Json body;
        try { body = Json::parse(req.body); }
        catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
        std::string name = body["name"].as_string_or("");
        if (name.empty()) return HttpResponse::json(400, error_json("筛选名不能为空").dump());
        std::string spec = body["spec"].dump();
        db_.exec("INSERT INTO saved_filters(name,spec) VALUES(" + qstr(name) + "," +
                 qstr(spec) + ")");
        Json resp = Json::object();
        resp["ok"] = true;
        resp["id"] = static_cast<long long>(db_.last_insert_rowid());
        return HttpResponse::json(201, resp.dump());
    }
    if (req.method == "PUT") {
        long long id = path_id(req, 2);
        Json body;
        try { body = Json::parse(req.body); }
        catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
        if (body.has("name"))
            db_.exec("UPDATE saved_filters SET name=" + qstr(body["name"].as_string_or("")) +
                     " WHERE id=" + std::to_string(id));
        if (body.has("spec"))
            db_.exec("UPDATE saved_filters SET spec=" + qstr(body["spec"].dump()) +
                     " WHERE id=" + std::to_string(id));
        Json resp = Json::object();
        resp["ok"] = true;
        return HttpResponse::json(200, resp.dump());
    }
    if (req.method == "DELETE") {
        long long id = path_id(req, 2);
        db_.exec("DELETE FROM saved_filters WHERE id=" + std::to_string(id));
        Json resp = Json::object();
        resp["ok"] = true;
        return HttpResponse::json(200, resp.dump());
    }
    Json arr = Json::array();
    for (auto& f : db_.query("SELECT * FROM saved_filters ORDER BY sort_order, id")) {
        Json fj = Json::object();
        fj["id"] = f.get_int("id");
        fj["name"] = f.get("name");
        try { fj["spec"] = Json::parse(f.get("spec")); }
        catch (...) { fj["spec"] = Json::object(); }
        arr.push_back(fj);
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["filters"] = arr;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_import(const HttpRequest& req) {
    std::string format = req.q("format", "");
    ImportResult res = importer::import_text(db_, format, req.body);
    Json j = Json::object();
    j["ok"] = res.errors.empty();
    j["tasks"] = res.tasks;
    j["projects"] = res.projects;
    j["tags"] = res.tags;
    j["summary"] = res.summary();
    Json errs = Json::array();
    for (auto& e : res.errors) errs.push_back(e);
    j["errors"] = errs;
    return HttpResponse::json(res.errors.empty() ? 200 : 400, j.dump());
}

HttpResponse Api::handle_holidays(const HttpRequest& req) {
    if (req.method == "POST") {
        Json body;
        try { body = Json::parse(req.body); }
        catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
        std::string date = body["date"].as_string_or("");
        int y = 0, m = 0, d = 0;
        if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
            return HttpResponse::json(400, error_json("日期格式应为 YYYY-MM-DD").dump());
        db_.exec("INSERT OR IGNORE INTO holidays(date) VALUES(" + qstr(date) + ")");
        Json resp = Json::object();
        resp["ok"] = true;
        return HttpResponse::json(200, resp.dump());
    }
    if (req.method == "DELETE") {
        // 路径形如 /api/holidays/2026-10-01，直接取末段
        size_t pos = req.path.rfind('/');
        std::string date = pos == std::string::npos ? "" : req.path.substr(pos + 1);
        int y = 0, m = 0, d = 0;
        if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) == 3)
            db_.exec("DELETE FROM holidays WHERE date=" + qstr(date));
        Json resp = Json::object();
        resp["ok"] = true;
        return HttpResponse::json(200, resp.dump());
    }
    Json arr = Json::array();
    for (auto& h : db_.query("SELECT date FROM holidays ORDER BY date"))
        arr.push_back(h.get("date"));
    Json resp = Json::object();
    resp["ok"] = true;
    resp["holidays"] = arr;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_search(const HttpRequest& req) {
    std::string q = req.q("q");
    if (q.empty()) return HttpResponse::json(200, "{\"ok\":true,\"tasks\":[]}");
    std::string sql = "SELECT * FROM tasks WHERE deleted_at IS NULL AND (title LIKE " + qstr("%" + q + "%") +
                      " OR notes LIKE " + qstr("%" + q + "%") + ")" +
                      " ORDER BY priority DESC, COALESCE(due_date,'9999-12-31') LIMIT 50";
    Json arr = Json::array();
    for (auto& r : db_.query(sql)) arr.push_back(task_basic_json(db_, r));
    Json resp = Json::object();
    resp["ok"] = true;
    resp["tasks"] = arr;
    return HttpResponse::json(200, resp.dump());
}

// ==================== 批次 B/C：撤销 / 重复预览 / 日视图 / 同步 ====================

namespace {

const char* undo_action_name(const std::string& action) {
    if (action == "create") return "创建";
    if (action == "update") return "更新";
    if (action == "delete") return "删除";
    if (action == "purge") return "彻底删除";
    if (action == "complete") return "完成";
    if (action == "reopen") return "重新打开";
    if (action == "restore") return "恢复";
    if (action == "giveup") return "放下";
    return "操作";
}

} // namespace

HttpResponse Api::handle_undo(const HttpRequest& req) {
    auto row = db_.query_one("SELECT * FROM undo_log ORDER BY id DESC LIMIT 1");
    if (req.method == "GET") {
        Json resp = Json::object();
        resp["ok"] = true;
        resp["canUndo"] = row.has_value();
        if (row) {
            resp["action"] = row->get("action");
            resp["actionName"] = undo_action_name(row->get("action"));
            resp["label"] = row->get("label");
            resp["taskId"] = row->get_int("task_id");
        }
        return HttpResponse::json(200, resp.dump());
    }
    // POST：执行撤销
    if (!row)
        return HttpResponse::json(404, error_json("没有可撤销的操作").dump());
    std::string action = row->get("action");
    long long task_id = row->get_int("task_id");
    long long after_id = row->get_int("after_id");
    std::string label = row->get("label");
    Json payload;
    try { payload = Json::parse(row->get("payload")); }
    catch (...) { payload = Json::object(); }
    Json before = payload["before"];

    try {
        if (action == "create") {
            // 撤销创建 = 删除刚创建的任务（级联清理标签/依赖）
            if (after_id > 0)
                db_.exec("DELETE FROM tasks WHERE id=" + std::to_string(after_id));
        } else if (action == "complete") {
            // 撤销完成 = 删除自动生成的下一实例（含其 create 撤销记录），再恢复完成前快照
            if (after_id > 0) {
                db_.exec("DELETE FROM tasks WHERE id=" + std::to_string(after_id));
                db_.exec("DELETE FROM undo_log WHERE action='create' AND after_id=" +
                         std::to_string(after_id));
            }
            restore_task_from_snapshot(db_, before);
        } else if (action == "update" || action == "delete" || action == "purge" ||
                   action == "reopen" || action == "restore" || action == "giveup") {
            // 其余操作 = 恢复操作前快照
            restore_task_from_snapshot(db_, before);
        } else {
            return HttpResponse::json(400, error_json("未知的撤销类型: " + action).dump());
        }
        db_.exec("DELETE FROM undo_log WHERE id=" + std::to_string(row->get_int("id")));
    } catch (const std::exception& e) {
        return HttpResponse::json(500, error_json(std::string("撤销失败: ") + e.what()).dump());
    }

    Json resp = Json::object();
    resp["ok"] = true;
    resp["action"] = action;
    resp["taskId"] = action == "create" ? after_id : task_id;
    resp["desc"] = std::string("已撤销") + undo_action_name(action) +
                   (label.empty() ? "" : "：" + label);
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_repeat_preview(const HttpRequest& req) {
    Json body;
    try { body = Json::parse(req.body); }
    catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
    RepeatRule rule = RepeatRule::from_json(body["rule"]);
    if (!rule.enabled())
        return HttpResponse::json(400, error_json("重复规则无效").dump());
    std::string cur = body["from"].as_string_or(lunar::today_iso());
    int count = static_cast<int>(body["count"].as_int_or(5));
    if (count < 1) count = 1;
    if (count > 20) count = 20;
    HolidayChecker hc(db_);
    Json dates = Json::array();
    for (int i = 0; i < count; ++i) {
        std::string next = recurrence::next_after(rule, cur, hc);
        if (next.empty()) break;
        if (!rule.end_date.empty() && next > rule.end_date) break;
        dates.push_back(next);
        cur = next;
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["dates"] = dates;
    resp["rule"] = rule.to_json();
    return HttpResponse::json(200, resp.dump());
}

// 日视图条目精简 JSON（供时间块渲染）
Json day_entry_json(Db& db, const Db::Row& r, bool virtual_instance) {
    Json j = Json::object();
    j["id"] = r.get_int("id");
    j["title"] = r.get("title");
    j["priority"] = r.get_int("priority");
    j["status"] = r.get("status");
    j["remindTime"] = r.get("remind_time");
    j["dueDate"] = r.get("due_date");
    j["lunarRemind"] = r.get_int("lunar_remind") != 0;
    j["lunarDate"] = r.get("lunar_date");
    j["virtual"] = virtual_instance;
    j["repeat"] = !r.get("repeat_rule").empty();
    Json tags = Json::array();
    for (auto& t : db.query(
             "SELECT t.name FROM tags t JOIN task_tags tt ON t.id=tt.tag_id "
             "WHERE tt.task_id=? ORDER BY t.name", {r.get("id")}))
        tags.push_back(t.get("name"));
    j["tags"] = tags;
    long long pid = r.get_int("project_id");
    if (pid) {
        if (auto p = db.query_one("SELECT name,color FROM projects WHERE id=?",
                                  {std::to_string(pid)})) {
            j["projectName"] = p->get("name");
            j["projectColor"] = p->get("color", "#4A90D9");
        }
    }
    return j;
}

HttpResponse Api::handle_day(const HttpRequest& req) {
    std::string date = req.q("date", lunar::today_iso());
    int y = 0, m = 0, d = 0;
    if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || y < 1901 || y > 2099)
        return HttpResponse::json(400, error_json("日期格式应为 YYYY-MM-DD").dump());
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    date = buf;

    Json resp = Json::object();
    resp["ok"] = true;
    resp["date"] = date;
    // 农历与节假日信息
    LunarDate ld = lunar::solar_to_lunar(y, m, d);
    resp["lunarText"] = ld.chinese;
    resp["term"] = lunar::solar_term(y, m, d);
    std::string hol = lunar::statutory_holiday(y, m, d);
    bool isHol = !hol.empty() ||
                 db_.query_one("SELECT 1 FROM holidays WHERE date=?", {date}).has_value();
    if (hol.empty() && isHol) hol = "节假日";
    resp["holidayName"] = hol;
    resp["isHoliday"] = isHol;
    resp["weekday"] = static_cast<long long>(lunar::weekday_of_iso(date));

    Json timed = Json::array();
    Json allday = Json::array();
    Json done = Json::array();
    std::set<long long> added;

    // 当日到期/开始的普通任务
    for (auto& r : db_.query(
             "SELECT * FROM tasks WHERE deleted_at IS NULL AND status!='done' AND "
             "(due_date=? OR (due_date IS NULL AND start_date=?)) ORDER BY remind_time, id",
             {date, date})) {
        bool has_time = !r.get("remind_time").empty();
        (has_time ? timed : allday).push_back(day_entry_json(db_, r, false));
        added.insert(r.get_int("id"));
    }

    // 已完成：当日到期或当日完成
    for (auto& r : db_.query(
             "SELECT * FROM tasks WHERE deleted_at IS NULL AND status='done' AND "
             "(due_date=? OR completed_at LIKE ?) ORDER BY completed_at DESC, id",
             {date, date + "%"})) {
        Json j = day_entry_json(db_, r, false);
        j["completedAt"] = r.get("completed_at");
        done.push_back(j);
        added.insert(r.get_int("id"));
    }

    // 重复任务的虚拟实例（规则命中当日且本体不在当日）
    HolidayChecker hc(db_);
    for (auto& r : db_.query(
             "SELECT * FROM tasks WHERE deleted_at IS NULL AND status!='done' AND "
             "repeat_rule!='' ORDER BY id")) {
        if (added.count(r.get_int("id"))) continue;
        std::string rr = r.get("repeat_rule");
        try {
            RepeatRule rule = RepeatRule::from_json_str(rr);
            if (!rule.enabled()) continue;
            auto occ = recurrence::occurrences_in_range(rule, date, date, hc);
            if (occ.empty() || occ.front() != date) continue;
            if (r.get("due_date") == date) continue;   // 本体已计入
            bool has_time = !r.get("remind_time").empty();
            (has_time ? timed : allday).push_back(day_entry_json(db_, r, true));
            added.insert(r.get_int("id"));
        } catch (...) {}
    }

    // 农历提醒的虚拟实例
    for (auto& r : db_.query(
             "SELECT * FROM tasks WHERE deleted_at IS NULL AND status!='done' AND "
             "lunar_remind=1 AND lunar_date IS NOT NULL AND lunar_date!='' ORDER BY id")) {
        if (added.count(r.get_int("id"))) continue;
        if (r.get("lunar_date") != std::to_string(ld.month) + "-" + std::to_string(ld.day))
            continue;
        bool has_time = !r.get("remind_time").empty();
        Json j = day_entry_json(db_, r, true);
        j["lunarInstance"] = true;
        (has_time ? timed : allday).push_back(j);
        added.insert(r.get_int("id"));
    }

    resp["timed"] = timed;
    resp["allday"] = allday;
    resp["done"] = done;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_sync(const HttpRequest& req) {
    Json snap;
    try { snap = Json::parse(req.body); }
    catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }
    if (!snap.is_object() || !snap["tasks"].is_array() ||
        snap.get_str("app") != "cpp-todo")
        return HttpResponse::json(400, error_json("无效的同步快照（应为 /api/export?format=backup 导出文件）").dump());

    long long newTasks = 0, updatedTasks = 0, newProjects = 0, newTags = 0;
    std::set<long long> touched;   // 被写入的任务（第二趟修复 parent/标签/依赖）
    try {
        db_.begin();

        // 1. 项目：按名称映射，缺失则创建
        std::map<long long, long long> pmap;
        if (snap["projects"].is_array()) {
            for (auto& p : snap["projects"]) {
                std::string name = p["name"].as_string_or("");
                if (name.empty()) continue;
                auto row = db_.query_one("SELECT id FROM projects WHERE name=?", {name});
                long long lid;
                if (row) {
                    lid = row->get_int("id");
                } else {
                    db_.exec("INSERT INTO projects(name,color,is_folder,sort_order) VALUES(" +
                             qstr(name) + "," + qstr(p["color"].as_string_or("#4A90D9")) +
                             "," + (p["isFolder"].as_bool_or(false) ? "1" : "0") + "," +
                             std::to_string(p["sortOrder"].as_int_or(0)) + ")");
                    lid = db_.last_insert_rowid();
                    ++newProjects;
                }
                pmap[p["id"].as_int_or(0)] = lid;
            }
        }

        // 2. 标签：按名称，缺失则创建
        if (snap["tags"].is_array()) {
            for (auto& t : snap["tags"]) {
                std::string name = t["name"].as_string_or("");
                if (name.empty()) continue;
                if (!db_.query_one("SELECT 1 FROM tags WHERE name=?", {name}).has_value()) {
                    db_.exec("INSERT INTO tags(name,color) VALUES(" + qstr(name) + "," +
                             qstr(t["color"].as_string_or("#8E8E93")) + ")");
                    ++newTags;
                }
            }
        }

        // 3. 任务第一趟：按 id 对齐；存在则 updated_at 新者胜，不存在则按原 id 插入
        for (auto& tk : snap["tasks"]) {
            long long rid = tk["id"].as_int_or(0);
            if (rid <= 0) continue;
            auto s = [&](const char* k) { return tk[k].as_string_or(""); };
            auto n = [&](const char* k) { return std::to_string(tk[k].as_int_or(0)); };
            auto val = [&](const std::string& v) { return v.empty() ? "NULL" : qstr(v); };
            long long rpid = tk["project_id"].as_int_or(0);
            long long lpid = pmap.count(rpid) ? pmap[rpid] : 0;
            std::string proj = lpid ? std::to_string(lpid) : std::string("NULL");
            std::string fields =
                "title=" + qstr(s("title")) + ", notes=" + qstr(s("notes")) +
                ", priority=" + n("priority") +
                ", start_date=" + val(s("start_date")) + ", due_date=" + val(s("due_date")) +
                ", remind_time=" + val(s("remind_time")) + ", has_reminder=" + n("has_reminder") +
                ", lunar_remind=" + n("lunar_remind") + ", lunar_date=" + val(s("lunar_date")) +
                ", sort_order=" + n("sort_order") + ", status=" + qstr(s("status")) +
                ", completed_at=" + val(s("completed_at")) + ", pomodoros=" + n("pomodoros") +
                ", deleted_at=" + val(s("deleted_at")) + ", repeat_rule=" + qstr(s("repeat_rule")) +
                ", updated_at=" + qstr(s("updated_at"));
            auto existing = db_.query_one("SELECT updated_at FROM tasks WHERE id=?",
                                          {std::to_string(rid)});
            if (existing) {
                // 远端更新时间更新（或本地为 NULL）才覆盖
                if (s("updated_at") > existing->get("updated_at")) {
                    db_.exec("UPDATE tasks SET " + fields + ", project_id=" + proj +
                             " WHERE id=" + std::to_string(rid));
                    ++updatedTasks;
                    touched.insert(rid);
                }
            } else {
                // 按原 id 重建（parent 先置 NULL，第二趟修复）
                db_.exec("INSERT INTO tasks(id,title,notes,priority,start_date,due_date,"
                         "remind_time,has_reminder,lunar_remind,lunar_date,project_id,"
                         "parent_id,sort_order,status,completed_at,pomodoros,deleted_at,"
                         "repeat_rule,created_at,updated_at) VALUES(" +
                         std::to_string(rid) + "," + qstr(s("title")) + "," +
                         qstr(s("notes")) + "," + n("priority") + "," + val(s("start_date")) +
                         "," + val(s("due_date")) + "," + val(s("remind_time")) + "," +
                         n("has_reminder") + "," + n("lunar_remind") + "," +
                         val(s("lunar_date")) + "," + proj + ",NULL," + n("sort_order") + "," +
                         qstr(s("status")) + "," + val(s("completed_at")) + "," + n("pomodoros") +
                         "," + val(s("deleted_at")) + "," + qstr(s("repeat_rule")) + "," +
                         qstr(s("created_at")) + "," + qstr(s("updated_at")) + ")");
                ++newTasks;
                touched.insert(rid);
            }
        }

        // 4. 任务第二趟：parent_id / 标签 / 依赖（仅对刚写入的任务）
        for (auto& tk : snap["tasks"]) {
            long long rid = tk["id"].as_int_or(0);
            if (rid <= 0 || !touched.count(rid)) continue;
            long long par = tk["parent_id"].as_int_or(0);
            if (par > 0 && db_.query_one("SELECT 1 FROM tasks WHERE id=?",
                                         {std::to_string(par)}).has_value())
                db_.exec("UPDATE tasks SET parent_id=" + std::to_string(par) +
                         " WHERE id=" + std::to_string(rid));
            if (tk["tags"].is_array()) {
                db_.exec("DELETE FROM task_tags WHERE task_id=" + std::to_string(rid));
                for (auto& t : tk["tags"]) {
                    std::string name = t.as_string_or("");
                    if (name.empty()) continue;
                    auto row = db_.query_one("SELECT id FROM tags WHERE name=?", {name});
                    if (!row) continue;
                    db_.exec("INSERT OR IGNORE INTO task_tags(task_id,tag_id) VALUES(" +
                             std::to_string(rid) + "," +
                             std::to_string(row->get_int("id")) + ")");
                }
            }
            if (tk["depends_on"].is_array()) {
                db_.exec("DELETE FROM task_dependencies WHERE task_id=" + std::to_string(rid));
                for (auto& d : tk["depends_on"]) {
                    long long dep = d.as_int_or(0);
                    if (dep <= 0 || dep == rid) continue;
                    if (!db_.query_one("SELECT 1 FROM tasks WHERE id=?",
                                       {std::to_string(dep)}).has_value())
                        continue;
                    db_.exec("INSERT OR IGNORE INTO task_dependencies(task_id,depends_on) VALUES(" +
                             std::to_string(rid) + "," + std::to_string(dep) + ")");
                }
            }
        }

        // 5. 模板 / 筛选 / 节假日：insert-or-ignore
        if (snap["templates"].is_array()) {
            for (auto& t : snap["templates"]) {
                std::string name = t["name"].as_string_or("");
                std::string body = t["body"].as_string_or("");
                if (name.empty() || body.empty()) continue;
                if (!db_.query_one("SELECT 1 FROM templates WHERE name=? AND body=?",
                                   {name, body}).has_value())
                    db_.exec("INSERT INTO templates(name,body) VALUES(" + qstr(name) + "," +
                             qstr(body) + ")");
            }
        }
        if (snap["filters"].is_array()) {
            for (auto& f : snap["filters"]) {
                std::string name = f["name"].as_string_or("");
                std::string spec = f["spec"].as_string_or("");
                if (name.empty() || spec.empty()) continue;
                if (!db_.query_one("SELECT 1 FROM saved_filters WHERE name=? AND spec=?",
                                   {name, spec}).has_value())
                    db_.exec("INSERT INTO saved_filters(name,spec) VALUES(" + qstr(name) + "," +
                             qstr(spec) + ")");
            }
        }
        if (snap["holidays"].is_array()) {
            for (auto& h : snap["holidays"]) {
                std::string date = h.as_string_or("");
                if (date.empty()) continue;
                db_.exec("INSERT OR IGNORE INTO holidays(date) VALUES(" + qstr(date) + ")");
            }
        }

        db_.commit();
    } catch (const std::exception& e) {
        try { db_.rollback(); } catch (...) {}
        return HttpResponse::json(500, error_json(std::string("同步失败: ") + e.what()).dump());
    }

    Json resp = Json::object();
    resp["ok"] = true;
    resp["newTasks"] = newTasks;
    resp["updatedTasks"] = updatedTasks;
    resp["newProjects"] = newProjects;
    resp["newTags"] = newTags;
    resp["summary"] = "新增 " + std::to_string(newTasks) + " 个任务，更新 " +
                      std::to_string(updatedTasks) + " 个任务，新增项目 " +
                      std::to_string(newProjects) + " 个，新增标签 " +
                      std::to_string(newTags) + " 个";
    return HttpResponse::json(200, resp.dump());
}

// ---- WebDAV 配置与同步 ----

HttpResponse Api::handle_webdav_config(const HttpRequest& req) {
    std::lock_guard<std::recursive_mutex> lk(Db::mutex());
    if (req.method == "GET") {
        Json j = Json::object();
        const char* keys[] = {
            "webdav_enabled", "webdav_url", "webdav_username", "webdav_password",
            "webdav_remote_dir", "webdav_conflict_policy", "webdav_propagate_delete",
            "webdav_timeout"
        };
        for (const char* k : keys) {
            auto row = db_.query_one("SELECT value FROM settings WHERE key=?", {k});
            j[k] = row ? row->get("value") : "";
        }
        j["webdav_enabled"] = j["webdav_enabled"].as_string_or("") == "1";
        j["webdav_propagate_delete"] = j["webdav_propagate_delete"].as_string_or("") != "0";
        j["webdav_timeout"] = j["webdav_timeout"].as_int_or(30);
        return HttpResponse::json(200, j.dump());
    }
    // PUT
    Json body;
    try { body = Json::parse(req.body); }
    catch (...) { return HttpResponse::json(400, error_json("JSON 解析失败").dump()); }

    auto set = [&](const char* key, const std::string& val) {
        db_.exec("REPLACE INTO settings(key,value) VALUES(" + qstr(key) + "," + qstr(val) + ")");
    };
    set("webdav_enabled", body.get_bool("enabled") ? "1" : "0");
    set("webdav_url", body.get_str("url"));
    set("webdav_username", body.get_str("username"));
    set("webdav_password", body.get_str("password"));
    set("webdav_remote_dir", body.get_str("remoteDir"));
    set("webdav_conflict_policy", body.get_str("conflictPolicy"));
    set("webdav_propagate_delete", body.get_bool("propagateDelete") ? "1" : "0");
    set("webdav_timeout", std::to_string(body["timeout"].as_int_or(30)));

    Json resp = Json::object();
    resp["ok"] = true;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_webdav_sync(const HttpRequest& req) {
    std::lock_guard<std::recursive_mutex> lk(Db::mutex());
    (void)req; // POST 无需 body

    auto get_setting = [&](const char* key) -> std::string {
        auto row = db_.query_one("SELECT value FROM settings WHERE key=?", {key});
        return row ? row->get("value") : "";
    };
    if (get_setting("webdav_enabled") != "1")
        return HttpResponse::json(400, error_json("WebDAV 同步未启用").dump());
    std::string url = get_setting("webdav_url");
    if (url.empty())
        return HttpResponse::json(400, error_json("未配置 WebDAV URL").dump());

    // WAL checkpoint，保证主库文件完整
    if (!db_.checkpoint())
        return HttpResponse::json(500, error_json("数据库 checkpoint 失败").dump());

    fs::path db_path(db_.path());
    std::string local_dir = db_path.parent_path().string();

    std::string username = get_setting("webdav_username");
    std::string password = get_setting("webdav_password");
    std::string remote_dir = get_setting("webdav_remote_dir");
    std::string conflict_policy = get_setting("webdav_conflict_policy");
    if (conflict_policy.empty()) conflict_policy = "newer";
    std::string propagate_delete = get_setting("webdav_propagate_delete");
    if (propagate_delete.empty()) propagate_delete = "1";
    std::string timeout_str = get_setting("webdav_timeout");
    if (timeout_str.empty()) timeout_str = "30";

    // 查找 davsync.py
    std::vector<std::string> candidates = {
        "/Users/zhangmingfeng/Projects/TODO-list/webdav-sync/davsync.py",
        "./webdav-sync/davsync.py",
        "../webdav-sync/davsync.py",
    };
    if (!static_root_.empty()) {
        fs::path sr(static_root_);
        candidates.push_back((sr.parent_path() / "webdav-sync" / "davsync.py").string());
    }
    std::string script;
    for (auto& c : candidates) {
        if (fs::exists(c)) { script = c; break; }
    }
    if (script.empty())
        return HttpResponse::json(500, error_json("找不到 davsync.py，请确认 webdav-sync/ 目录存在").dump());

    // 生成临时配置
    char tmpfile[256];
    std::snprintf(tmpfile, sizeof tmpfile, "/tmp/davsync-%lld-%d.json",
                  static_cast<long long>(std::time(nullptr)), std::rand());
    {
        Json cfg = Json::object();
        cfg["url"] = url;
        cfg["username"] = username;
        cfg["password"] = password;
        cfg["local_dir"] = local_dir;
        cfg["remote_dir"] = remote_dir;
        cfg["conflict_policy"] = conflict_policy;
        cfg["propagate_delete"] = (propagate_delete == "1");
        cfg["timeout"] = std::atoi(timeout_str.c_str());
        cfg["ignore"] = Json::array();
        cfg["ignore"].push_back("*.db-wal");
        cfg["ignore"].push_back("*.db-shm");
        cfg["ignore"].push_back("webdav.json");
        std::ofstream ofs(tmpfile);
        if (!ofs) {
            return HttpResponse::json(500, error_json("无法创建临时配置文件").dump());
        }
        ofs << cfg.dump_pretty();
    }

    std::string cmd = "python3 " + script + " sync -c " + tmpfile;
    std::array<char, 4096> buf{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        fs::remove(tmpfile);
        return HttpResponse::json(500, error_json("无法启动同步进程").dump());
    }
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        output += buf.data();
    int status = pclose(pipe);
    fs::remove(tmpfile);

    Json resp = Json::object();
    resp["ok"] = (status == 0);
    resp["exitCode"] = status;
    resp["output"] = output;
    return HttpResponse::json(status == 0 ? 200 : 500, resp.dump());
}

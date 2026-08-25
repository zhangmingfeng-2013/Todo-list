// api.cpp — REST API 实现
#include "api.hpp"
#include "json.hpp"
#include "lunar.hpp"
#include "recurrence.hpp"
#include "importer.hpp"
#include "storage.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

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
             "SELECT * FROM tasks WHERE parent_id=? ORDER BY sort_order, id",
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
        if (body.has("repeatRule")) {
            std::string v = body["repeatRule"].dump();
            upd("repeat_rule", v == "{}" ? Json("") : Json(v));
        }
        db.exec("UPDATE tasks SET updated_at=datetime('now','localtime') WHERE id=" +
                std::to_string(id));
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
        std::string repeat_rule;
        if (body["repeatRule"].is_object() && !body["repeatRule"].empty()) {
            repeat_rule = body["repeatRule"].dump();
        }
        std::string sql = "INSERT INTO tasks(title,notes,priority,start_date,due_date,"
                          "remind_time,has_reminder,lunar_remind,lunar_date,project_id,"
                          "parent_id,status,repeat_rule) VALUES(" +
                          qstr(title) + "," + qstr(notes) + "," + std::to_string(prio) + "," +
                          (start.empty() ? "NULL" : qstr(start)) + "," +
                          (due.empty() ? "NULL" : qstr(due)) + "," +
                          (rtime.empty() ? "NULL" : qstr(rtime)) + "," +
                          std::to_string(has_rem) + "," + std::to_string(lunar_rem) + "," +
                          (lunar_date.empty() ? "NULL" : qstr(lunar_date)) + "," +
                          (pid ? std::to_string(pid) : "NULL") + "," +
                          (parent ? std::to_string(parent) : "NULL") + "," +
                          qstr(status) + "," +
                          (repeat_rule.empty() ? "''" : qstr(repeat_rule)) + ")";
        db.exec(sql);
        id = db.last_insert_rowid();
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

} // namespace

Api::Api(Db& db, const std::string& static_root) : db_(db), static_root_(static_root) {}

void Api::register_routes(HttpServer& srv) {
    // 静态资源由 HttpServer 兜底
    srv.on("GET", "/api/meta", [this](const HttpRequest& r) { return handle_meta(r); });
    srv.on("GET", "/api/tasks", [this](const HttpRequest& r) { return handle_tasks(r); });
    srv.on("POST", "/api/tasks", [this](const HttpRequest& r) { return handle_tasks(r); });
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

    // GET 列表 + 筛选
    std::string sql = "SELECT * FROM tasks WHERE 1=1";
    std::vector<std::string> params;

    std::string status = req.q("status", "all");
    if (status != "all" && !status.empty())
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
    if (action == "deps")
        return handle_deps(req, id);
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
    db_.exec("DELETE FROM tasks WHERE id=" + std::to_string(id));
    Json resp = Json::object();
    resp["ok"] = true;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_task_complete(const HttpRequest& req, long long id) {
    auto row = db_.query_one("SELECT * FROM tasks WHERE id=?", {std::to_string(id)});
    if (!row) return HttpResponse::json(404, error_json("任务不存在").dump());
    db_.exec("UPDATE tasks SET status='done', completed_at=" + qstr(now_local()) +
             " WHERE id=" + std::to_string(id));
    Json created = Json::object();
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
                    if (ok) created = nj;
                }
            }
        } catch (...) {}
    }
    Json resp = Json::object();
    resp["ok"] = true;
    resp["nextInstance"] = created;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_task_reopen(const HttpRequest& req, long long id) {
    if (db_.query_one("SELECT 1 FROM tasks WHERE id=?", {std::to_string(id)}).has_value() == false)
        return HttpResponse::json(404, error_json("任务不存在").dump());
    db_.exec("UPDATE tasks SET status='todo', completed_at=NULL WHERE id=" +
             std::to_string(id));
    Json resp = Json::object();
    resp["ok"] = true;
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
    std::string sql = "SELECT * FROM tasks";
    if (status != "all") sql += " WHERE status=" + qstr(status);
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
        std::string sql = "SELECT * FROM tasks WHERE status!='done' AND " + cond +
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
             "SELECT * FROM tasks WHERE status='done' AND completed_at LIKE '" + today + "%' "
             "ORDER BY completed_at DESC")) {
        done.push_back(task_basic_json(db_, r));
    }
    resp["doneToday"] = done;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_calendar(const HttpRequest& req) {
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
    std::string last = lunar::add_days_iso(next_first, -1);

    HolidayChecker hc(db_);
    std::map<std::string, Json> day_map;
    std::string d = first;
    for (int guard = 0; guard < 31 && d <= last; ++guard, d = lunar::add_days_iso(d, 1)) {
        Json dj = Json::object();
        dj["date"] = d;
        int y2 = 0, m2 = 0, dd2 = 0;
        std::sscanf(d.c_str(), "%d-%d-%d", &y2, &m2, &dd2);
        auto lt = lunar::solar_to_lunar(y2, m2, dd2);
        dj["lunar"] = lt.month == 0 ? "" : (lunar::month_name(lt.month, lt.isLeap) +
                                            lunar::day_name(lt.day));
        dj["isHoliday"] = hc(d);
        dj["tasks"] = Json::array();
        day_map[d] = dj;
    }

    // 当天到期/开始的任务
    for (auto& r : db_.query(
             "SELECT * FROM tasks WHERE status!='done' AND "
             "(due_date BETWEEN '" + std::string(first) + "' AND '" + last + "' OR "
             " start_date BETWEEN '" + std::string(first) + "' AND '" + last + "')")) {
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
    for (auto& r : db_.query("SELECT * FROM tasks WHERE status!='done' AND repeat_rule!=''")) {
        RepeatRule rule = RepeatRule::from_json_str(r.get("repeat_rule"));
        if (!rule.enabled()) continue;
        auto occ = recurrence::occurrences_in_range(rule, first, last, hc);
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
             "SELECT * FROM tasks WHERE status!='done' AND lunar_remind=1 AND lunar_date!=''")) {
        std::string ld = r.get("lunar_date");
        int lm = 0, lday = 0;
        if (std::sscanf(ld.c_str(), "%d-%d", &lm, &lday) != 2) continue;
        // 尝试本年与本年后一年
        for (int try_y = year - 1; try_y <= year + 1; ++try_y) {
            SolarDate s = lunar::lunar_to_solar(try_y, lm, lday, false);
            if (s.year != 0 && s.iso >= first && s.iso <= last && day_map.count(s.iso)) {
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
    d = first;
    for (int guard = 0; guard < 31 && d <= last; ++guard, d = lunar::add_days_iso(d, 1))
        days.push_back(day_map[d]);

    Json resp = Json::object();
    resp["ok"] = true;
    resp["year"] = year;
    resp["month"] = month;
    resp["days"] = days;
    return HttpResponse::json(200, resp.dump());
}

HttpResponse Api::handle_kanban(const HttpRequest& req) {
    std::string sql = "SELECT * FROM tasks WHERE status IN ('todo','doing','done')";
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
            "SELECT COUNT(*) c FROM tasks WHERE project_id=? AND status!='done'",
            {std::to_string(p.get_int("id"))});
        pj["taskCount"] = cnt ? cnt->get_int("c") : 0;
        arr.push_back(pj);
    }
    // 未分类
    auto unc = db_.query_one("SELECT COUNT(*) c FROM tasks WHERE project_id IS NULL AND status!='done'");
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
             "LEFT JOIN tasks ts ON ts.id=tt.task_id AND ts.status!='done' "
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
    std::string sql = "SELECT * FROM tasks WHERE title LIKE " + qstr("%" + q + "%") +
                      " OR notes LIKE " + qstr("%" + q + "%") +
                      " ORDER BY priority DESC, COALESCE(due_date,'9999-12-31') LIMIT 50";
    Json arr = Json::array();
    for (auto& r : db_.query(sql)) arr.push_back(task_basic_json(db_, r));
    Json resp = Json::object();
    resp["ok"] = true;
    resp["tasks"] = arr;
    return HttpResponse::json(200, resp.dump());
}

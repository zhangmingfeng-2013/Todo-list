// importer.cpp — 三种格式导入实现
#include "importer.hpp"
#include "recurrence.hpp"
#include "lunar.hpp"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string esc_sql(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '\'') r += "''";
        else r += c;
    }
    return r;
}

// 确保项目存在，返回 id
long long ensure_project(Db& db, const std::string& name, long long parent_id = 0) {
    std::string n = trim(name);
    if (n.empty()) return 0;
    if (auto row = db.query_one("SELECT id FROM projects WHERE name=?", {n})) {
        return row->get_int("id");
    }
    std::string sql = "INSERT INTO projects(name, parent_id, sort_order) VALUES('" + esc_sql(n) +
                      "', " + (parent_id ? std::to_string(parent_id) : "NULL") + ", 0)";
    db.exec(sql);
    return db.last_insert_rowid();
}

long long ensure_tag(Db& db, const std::string& name) {
    std::string n = trim(name);
    if (n.empty()) return 0;
    if (auto row = db.query_one("SELECT id FROM tags WHERE name=?", {n})) {
        return row->get_int("id");
    }
    // 名称可能含引号，转义
    std::string esc = n;
    std::replace(esc.begin(), esc.end(), '\'', '\"');
    db.exec("INSERT INTO tags(name) VALUES('" + esc + "')");
    return db.last_insert_rowid();
}

void link_tag(Db& db, long long task_id, long long tag_id) {
    if (!task_id || !tag_id) return;
    db.exec("INSERT OR IGNORE INTO task_tags(task_id, tag_id) VALUES(" +
            std::to_string(task_id) + "," + std::to_string(tag_id) + ")");
}

long long insert_task(Db& db, const std::string& title, const std::string& notes,
                      int priority, const std::string& start, const std::string& due,
                      const std::string& remind_time, bool has_reminder,
                      const std::string& status, long long project_id,
                      long long parent_id, const std::string& repeat_rule_json,
                      const std::vector<std::string>& tags) {
    std::string sql =
        "INSERT INTO tasks(title, notes, priority, start_date, due_date, remind_time, "
        "has_reminder, status, project_id, parent_id, repeat_rule) VALUES('" +
        esc_sql(title) + "','" + esc_sql(notes) + "'," + std::to_string(priority) + "," +
        (start.empty() ? "NULL" : "'" + start + "'") + "," +
        (due.empty() ? "NULL" : "'" + due + "'") + "," +
        (remind_time.empty() ? "NULL" : "'" + remind_time + "'") + "," +
        std::to_string(has_reminder ? 1 : 0) + ",'" + esc_sql(status) + "'," +
        (project_id ? std::to_string(project_id) : "NULL") + "," +
        (parent_id ? std::to_string(parent_id) : "NULL") + ",'" +
        esc_sql(repeat_rule_json) + "')";
    db.exec(sql);
    long long id = db.last_insert_rowid();
    for (const auto& t : tags) link_tag(db, id, ensure_tag(db, t));
    return id;
}

// 把 "YYYY-MM-DD HH:MM" 或 ISO 时间拆分为日期与时刻
void split_datetime(const std::string& raw, std::string& date, std::string& time) {
    std::string s = trim(raw);
    if (s.empty()) return;
    // 去 T 与 Z
    std::replace(s.begin(), s.end(), 'T', ' ');
    if (!s.empty() && s.back() == 'Z') s.pop_back();
    auto sp = s.find(' ');
    if (sp == std::string::npos) {
        date = s;
    } else {
        date = s.substr(0, sp);
        time = s.substr(sp + 1);
        if (time.size() >= 5) time = time.substr(0, 5);
        else time.clear();
    }
    // 校验日期格式
    int y = 0, m = 0, d = 0;
    if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) date.clear();
    if (date.empty()) time.clear();
}

} // namespace

std::string ImportResult::summary() const {
    std::string s = "导入完成：任务 " + std::to_string(tasks) + " 个，项目 " +
                    std::to_string(projects) + " 个，标签 " + std::to_string(tags) + " 个";
    if (!errors.empty()) {
        s += "；告警 " + std::to_string(errors.size()) + " 条";
    }
    return s;
}

namespace importer {

// ---------------- Todo.txt ----------------
ImportResult import_todotxt(Db& db, const std::string& text) {
    ImportResult res;
    std::istringstream in(text);
    std::string line;
    std::vector<long long> task_ids;
    while (std::getline(in, line)) {
        std::string l = trim(line);
        if (l.empty()) continue;

        bool done = false;
        int priority = 1;
        std::string creation, completion;
        std::string due, start, title;
        std::string project;
        std::vector<std::string> tags;

        size_t p = 0;
        if (l.rfind("x ", 0) == 0) {
            done = true;
            p = 2;
        }
        // 优先级 (A)
        if (p < l.size() && l[p] == '(' && p + 2 < l.size() && l[p + 2] == ')') {
            char pc = l[p + 1];
            if (pc >= 'A' && pc <= 'Z') {
                if (pc == 'A') priority = 2;
                else if (pc == 'B') priority = 1;
                else priority = 0;
                p += 3;
            }
        }
        // 日期 tokens
        auto is_date = [](const std::string& t) {
            int y = 0, m = 0, d = 0;
            return t.size() == 10 && std::sscanf(t.c_str(), "%d-%d-%d", &y, &m, &d) == 3;
        };
        std::string t1, t2;
        {
            size_t sp = l.find(' ', p);
            t1 = sp == std::string::npos ? l.substr(p) : l.substr(p, sp - p);
        }
        std::string rest = l.substr(p);
        std::istringstream tokens(rest);
        std::vector<std::string> words;
        std::string w;
        while (tokens >> w) words.push_back(w);
        size_t wi = 0;
        if (wi < words.size() && is_date(words[wi])) {
            if (done && wi + 1 < words.size() && is_date(words[wi + 1])) {
                completion = words[wi];
                creation = words[wi + 1];
                wi += 2;
            } else {
                creation = words[wi];
                ++wi;
            }
        }
        std::vector<std::string> body;
        for (; wi < words.size(); ++wi) {
            const std::string& w2 = words[wi];
            if (w2.rfind("due:", 0) == 0 && w2.size() > 4) due = w2.substr(4);
            else if (w2.rfind("t:", 0) == 0 && w2.size() > 2) start = w2.substr(2);
            else if (w2.rfind("h:", 0) == 0 && w2.size() > 2) continue;  // 隐藏
            else if (w2.rfind("rec:", 0) == 0 && w2.size() > 4) continue;
            else if (!w2.empty() && w2[0] == '+') {
                std::string pj = trim(w2.substr(1));
                if (pj.empty()) continue;
                if (project.empty()) project = pj;
                else tags.push_back(pj);
            }
            else if (!w2.empty() && (w2[0] == '@' || w2[0] == '#')) {
                std::string tg = trim(w2.substr(1));
                if (!tg.empty()) tags.push_back(tg);
            }
            else body.push_back(w2);
        }
        if (body.empty()) body.push_back("(无标题)");
        title = body[0];
        for (size_t i = 1; i < body.size(); ++i) title += " " + body[i];

        long long pid = ensure_project(db, project);
        std::string status = done ? "done" : "todo";
        std::string notes;
        long long id = insert_task(db, title, notes, priority, start, due, "", false,
                                   status, pid, 0, "", tags);
        if (id) {
            ++res.tasks;
            task_ids.push_back(id);
            if (done) {
                db.exec("UPDATE tasks SET completed_at=datetime('now','localtime') WHERE id=" +
                        std::to_string(id));
            }
        }
    }
    res.projects = static_cast<int>(db.query("SELECT COUNT(*) c FROM projects").at(0).get_int("c"));
    res.tags = static_cast<int>(db.query("SELECT COUNT(*) c FROM tags").at(0).get_int("c"));
    (void)task_ids;
    return res;
}

// ---------------- Todoist JSON ----------------
ImportResult import_todoist_json(Db& db, const std::string& text) {
    ImportResult res;
    Json root;
    try { root = Json::parse(text); }
    catch (const std::exception& e) {
        res.errors.push_back(std::string("JSON 解析失败: ") + e.what());
        return res;
    }

    // 项目（含嵌套）
    std::map<long long, long long> proj_db;
    if (root["projects"].is_array()) {
        for (auto& p : root["projects"]) {
            long long src_id = p["id"].as_int_or(0);
            long long parent_src = p["parent_id"].as_int_or(0);
            long long parent_db = parent_src ? proj_db[parent_src] : 0;
            long long id = ensure_project(db, p["name"].as_string_or("未命名"), parent_db);
            if (src_id) proj_db[src_id] = id;
            if (id) ++res.projects;
        }
    }

    // 标签
    std::map<long long, std::string> labels;
    if (root["labels"].is_array()) {
        for (auto& lb : root["labels"]) {
            long long id = lb["id"].as_int_or(0);
            labels[id] = lb["name"].as_string_or("");
        }
    }

    // 任务
    std::map<long long, long long> task_db;
    if (root["items"].is_array()) {
        for (auto& it : root["items"]) {
            long long src_id = it["id"].as_int_or(0);
            std::string title = it["content"].as_string_or("");
            std::string notes = it["description"].as_string_or("");
            int prio = 1;
            long long tp = it["priority"].as_int_or(1);
            if (tp == 4) prio = 2; else if (tp == 2) prio = 0;
            std::string due, start, time;
            if (it["due"].is_object()) {
                split_datetime(it["due"]["date"].as_string_or(""), due, time);
            }
            long long proj = it["project_id"].as_int_or(0);
            long long parent = it["parent_id"].as_int_or(0);
            bool checked = it["checked"].as_int_or(0) != 0;
            // 循环规则
            RepeatRule rr;
            if (it["due"].is_object() && it["due"]["is_recurring"].as_bool_or(false)) {
                std::string ds = lower(it["due"]["string"].as_string_or(""));
                if (ds.find("every day") != std::string::npos) rr.freq = "daily";
                else if (ds.find("every week") != std::string::npos) rr.freq = "weekly";
                else if (ds.find("every month") != std::string::npos) rr.freq = "monthly";
                else if (ds.find("every year") != std::string::npos) rr.freq = "yearly";
                else if (ds.find("every monday") != std::string::npos) { rr.freq = "weekly"; rr.weekdays = {1}; }
                else if (ds.find("every tuesday") != std::string::npos) { rr.freq = "weekly"; rr.weekdays = {2}; }
                else if (ds.find("every wednesday") != std::string::npos) { rr.freq = "weekly"; rr.weekdays = {3}; }
                else if (ds.find("every thursday") != std::string::npos) { rr.freq = "weekly"; rr.weekdays = {4}; }
                else if (ds.find("every friday") != std::string::npos) { rr.freq = "weekly"; rr.weekdays = {5}; }
                else if (ds.find("every saturday") != std::string::npos) { rr.freq = "weekly"; rr.weekdays = {6}; }
                else if (ds.find("every sunday") != std::string::npos) { rr.freq = "weekly"; rr.weekdays = {7}; }
            }
            std::vector<std::string> tags;
            if (it["labels"].is_array()) {
                for (auto& lid : it["labels"]) {
                    long long lidv = lid.as_int_or(0);
                    if (labels.count(lidv)) tags.push_back(labels[lidv]);
                }
            }
            long long pid = proj ? proj_db[proj] : 0;
            long long parent_db = parent ? task_db[parent] : 0;
            long long id = insert_task(db, title, notes, prio, start, due,
                                       time, !time.empty(), checked ? "done" : "todo",
                                       pid, parent_db, rr.to_json().dump(), tags);
            if (src_id) task_db[src_id] = id;
            if (id) ++res.tasks;
        }
    }

    // notes 合并进任务备注
    if (root["notes"].is_array()) {
        std::map<long long, std::vector<std::string>> notes_map;
        for (auto& n : root["notes"]) {
            long long item = n["item_id"].as_int_or(0);
            if (item && task_db.count(item)) {
                notes_map[item].push_back(n["content"].as_string_or(""));
            }
        }
        for (auto& kv : notes_map) {
            std::string all;
            for (size_t i = 0; i < kv.second.size(); ++i) {
                if (i) all += "\n\n";
                all += kv.second[i];
            }
            if (!all.empty()) {
                std::string sql = "UPDATE tasks SET notes = CASE WHEN notes='' THEN '" +
                                  esc_sql(all) + "' ELSE notes || char(10)||char(10)||'" +
                                  esc_sql(all) + "' END WHERE id=" + std::to_string(kv.first);
                db.exec(sql);
            }
        }
    }
    return res;
}

// ---------------- TickTick JSON ----------------
ImportResult import_ticktick_json(Db& db, const std::string& text) {
    ImportResult res;
    Json root;
    try { root = Json::parse(text); }
    catch (const std::exception& e) {
        res.errors.push_back(std::string("JSON 解析失败: ") + e.what());
        return res;
    }

    // 列表/文件夹（TickTick 的 lists 视为项目）
    std::map<std::string, long long> list_db;  // list id -> db id
    if (root["lists"].is_array()) {
        for (auto& lst : root["lists"]) {
            std::string id = lst["id"].as_string_or("");
            long long dbid = ensure_project(db, lst["name"].as_string_or("未命名"));
            if (!id.empty()) list_db[id] = dbid;
            if (dbid) ++res.projects;
        }
    }

    // 标签
    std::vector<std::string> tag_names;
    if (root["tags"].is_array()) {
        for (auto& t : root["tags"]) {
            std::string n = t["name"].as_string_or("");
            if (!n.empty()) tag_names.push_back(n);
        }
    }

    auto tick_prio = [](long long p) -> int {
        if (p == 5) return 2;
        if (p == 3) return 1;
        if (p == 1) return 0;
        return 1;
    };
    auto tick_status = [](long long s) -> std::string {
        if (s == 1) return "done";
        return "todo";
    };

    if (root["tasks"].is_array()) {
        for (auto& t : root["tasks"]) {
            std::string title = t["title"].as_string_or("");
            std::string notes = t["content"].as_string_or("");
            std::string start, due, time, start_time;
            split_datetime(t["startDate"].as_string_or(""), start, start_time);
            split_datetime(t["dueDate"].as_string_or(""), due, time);
            bool has_reminder = false;
            if (t["reminders"].is_array() && t["reminders"].size() > 0) has_reminder = true;
            std::string repeat_json;
            std::string rf = t["repeatFlag"].as_string_or("");
            if (!rf.empty()) {
                RepeatRule rr = parse_rrule(rf);
                if (!rr.freq.empty()) repeat_json = rr.to_json().dump();
            }
            std::vector<std::string> tags;
            if (t["tags"].is_array()) {
                for (auto& tg : t["tags"]) tags.push_back(tg.as_string_or(""));
            }
            std::string list_id = t["listId"].as_string_or("");
            long long pid = list_id.empty() ? 0 : list_db[list_id];
            std::string status = tick_status(t["status"].as_int_or(0));

            // 子任务 items
            std::vector<long long> sub_ids;
            if (t["items"].is_array() && t["items"].size() > 0) {
                for (auto& item : t["items"]) {
                    long long sid = insert_task(db, item["title"].as_string_or(""),
                                                "", 1, "", "", "", false,
                                                tick_status(item["status"].as_int_or(0)),
                                                0, 0, "", {});
                    if (sid) sub_ids.push_back(sid);
                }
            }
            long long id = insert_task(db, title, notes, tick_prio(t["priority"].as_int_or(0)),
                                       start, due, time, has_reminder, status, pid, 0,
                                       repeat_json, tags);
            if (id) {
                ++res.tasks;
                // 子任务挂到父任务
                for (long long sid : sub_ids) {
                    db.exec("UPDATE tasks SET parent_id=" + std::to_string(id) +
                            " WHERE id=" + std::to_string(sid));
                    ++res.tasks;
                }
            }
        }
    }
    return res;
}

// ---------------- TickTick CSV ----------------
// 简单 CSV 解析（支持引号包裹）
static std::vector<std::vector<std::string>> parse_csv(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> cur;
    std::string field;
    bool in_quotes = false;
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { field += '"'; ++i; }
                else in_quotes = false;
            } else field += c;
        } else {
            if (c == '"') in_quotes = true;
            else if (c == ',') { cur.push_back(field); field.clear(); }
            else if (c == '\n' || c == '\r') {
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
                cur.push_back(field); field.clear();
                rows.push_back(cur); cur.clear();
            } else field += c;
        }
        ++i;
    }
    if (!field.empty() || !cur.empty()) { cur.push_back(field); rows.push_back(cur); }
    // 去掉空行
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [](const std::vector<std::string>& r) {
                                  if (r.empty()) return true;
                                  for (auto& f : r) if (!trim(f).empty()) return false;
                                  return true;
                              }),
               rows.end());
    return rows;
}

ImportResult import_ticktick_csv(Db& db, const std::string& text) {
    ImportResult res;
    auto rows = parse_csv(text);
    if (rows.empty()) return res;
    // 列名 -> 索引
    std::map<std::string, int> col;
    for (size_t i = 0; i < rows[0].size(); ++i)
        col[lower(trim(rows[0][i]))] = static_cast<int>(i);
    auto get_col = [&](const std::vector<std::string>& r, const std::string& key) {
        auto it = col.find(key);
        if (it == col.end() || it->second >= static_cast<int>(r.size())) return std::string();
        return trim(r[static_cast<size_t>(it->second)]);
    };

    // 可能出现的列名别名
    auto alias = [&](const std::vector<std::string>& r,
                     std::initializer_list<const char*> keys) -> std::string {
        for (auto k : keys) {
            std::string v = get_col(r, k);
            if (!v.empty()) return v;
        }
        return "";
    };

    std::string repeat;
    for (size_t ri = 1; ri < rows.size(); ++ri) {
        auto& r = rows[ri];
        std::string title = alias(r, {"title", "content", "name", "task"});
        if (title.empty()) continue;
        std::string folder = alias(r, {"folder"});
        std::string list = alias(r, {"list", "project"});
        std::string proj_name = !list.empty() ? list : folder;
        std::string tags_raw = alias(r, {"tags", "labels"});
        std::string start = alias(r, {"start date", "startdate", "start"});
        std::string due = alias(r, {"due date", "duedate", "due"});
        std::string remind = alias(r, {"remind time", "remindtime", "reminder"});
        std::string prio = lower(alias(r, {"priority", "pri"}));
        std::string status = lower(alias(r, {"status"}));
        std::string notes = alias(r, {"content", "description", "note"});

        std::vector<std::string> tags;
        {
            std::stringstream ss(tags_raw);
            std::string item;
            while (std::getline(ss, item, ',')) {
                std::string t = trim(item);
                if (!t.empty()) {
                    if (t.front() == '#') t = t.substr(1);
                    if (!t.empty()) tags.push_back(t);
                }
            }
        }
        int p = 1;
        if (prio.find("high") != std::string::npos || prio == "5" || prio == "3" || prio == "a") p = 2;
        else if (prio.find("low") != std::string::npos || prio == "1" || prio == "c") p = 0;
        std::string st = status.find("complete") != std::string::npos ? "done" : "todo";
        long long pid = ensure_project(db, proj_name);
        long long id = insert_task(db, title, notes, p, start, due, remind,
                                   !remind.empty(), st, pid, 0, "", tags);
        if (id) ++res.tasks;
    }
    return res;
}

ImportResult import_text(Db& db, const std::string& format, const std::string& text) {
    std::string f = lower(format);
    if (f == "todotxt" || f == "todo.txt") return import_todotxt(db, text);
    if (f == "todoist" || f == "todoist_json") return import_todoist_json(db, text);
    if (f == "ticktick" || f == "ticktick_json") return import_ticktick_json(db, text);
    if (f == "ticktick_csv" || f == "csv") return import_ticktick_csv(db, text);
    // 自动嗅探：以 { 开头 → JSON
    std::string t = trim(text);
    if (!t.empty() && t[0] == '{') return import_todoist_json(db, text);
    return import_todotxt(db, text);
}

} // namespace importer

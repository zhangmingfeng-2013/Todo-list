// recurrence.cpp — 重复规则实现
#include "recurrence.hpp"
#include "lunar.hpp"

#include <algorithm>
#include <cstdio>
#include <cctype>

Json RepeatRule::to_json() const {
    Json j = Json::object();
    j["freq"] = freq;
    j["interval"] = interval;
    Json wd = Json::array();
    for (int w : weekdays) wd.push_back(w);
    j["weekdays"] = wd;
    j["skipWeekends"] = skip_weekends;
    j["skipHolidays"] = skip_holidays;
    j["endDate"] = end_date;
    j["maxInstances"] = max_instances;
    return j;
}

RepeatRule RepeatRule::from_json(const Json& j) {
    RepeatRule r;
    r.freq = j["freq"].as_string_or("");
    r.interval = static_cast<int>(j["interval"].as_int_or(1));
    if (r.interval < 1) r.interval = 1;
    if (j["weekdays"].is_array()) {
        for (size_t i = 0; i < j["weekdays"].size(); ++i)
            r.weekdays.push_back(static_cast<int>(j["weekdays"][i].as_int_or(0)));
    }
    r.skip_weekends = j["skipWeekends"].as_bool_or(false);
    r.skip_holidays = j["skipHolidays"].as_bool_or(false);
    r.end_date = j["endDate"].as_string_or("");
    r.max_instances = static_cast<int>(j["maxInstances"].as_int_or(0));
    return r;
}

RepeatRule RepeatRule::from_json_str(const std::string& s) {
    if (s.empty()) return RepeatRule{};
    try { return from_json(Json::parse(s)); } catch (...) { return RepeatRule{}; }
}

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

RepeatRule parse_rrule(const std::string& rrule) {
    RepeatRule r;
    std::string up = rrule;
    for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& part : split_csv(up)) {
        auto eq = part.find('=');
        if (eq == std::string::npos) continue;
        std::string key = part.substr(0, eq);
        std::string val = part.substr(eq + 1);
        if (key == "FREQ") {
            if (val == "DAILY") r.freq = "daily";
            else if (val == "WEEKLY") r.freq = "weekly";
            else if (val == "MONTHLY") r.freq = "monthly";
            else if (val == "YEARLY") r.freq = "yearly";
        } else if (key == "INTERVAL") {
            r.interval = std::max(1, std::atoi(val.c_str()));
        } else if (key == "BYDAY") {
            for (const auto& d : split_csv(val)) {
                if (d.size() >= 2) {
                    std::string w = d.substr(d.size() - 2);
                    if (w == "MO") r.weekdays.push_back(1);
                    else if (w == "TU") r.weekdays.push_back(2);
                    else if (w == "WE") r.weekdays.push_back(3);
                    else if (w == "TH") r.weekdays.push_back(4);
                    else if (w == "FR") r.weekdays.push_back(5);
                    else if (w == "SA") r.weekdays.push_back(6);
                    else if (w == "SU") r.weekdays.push_back(7);
                }
            }
            if (!r.weekdays.empty() && r.freq.empty()) r.freq = "weekly";
        } else if (key == "UNTIL") {
            if (val.size() >= 8) r.end_date = val.substr(0, 4) + "-" + val.substr(4, 2) + "-" + val.substr(6, 2);
        } else if (key == "COUNT") {
            r.max_instances = std::max(0, std::atoi(val.c_str()));
        }
    }
    return r;
}

static bool is_weekend(const std::string& iso) {
    int w = lunar::weekday_of_iso(iso);
    return w == 6 || w == 7;
}

namespace recurrence {

static std::string skip_skips(const RepeatRule& rule, const std::string& date,
                              const std::function<bool(const std::string&)>& is_holiday) {
    std::string d = date;
    int guard = 0;
    while (guard++ < 366) {
        bool skip = false;
        if (rule.skip_weekends && is_weekend(d)) skip = true;
        if (!skip && rule.skip_holidays && is_holiday && is_holiday(d)) skip = true;
        if (!skip) return d;
        d = lunar::add_days_iso(d, 1);
    }
    return d;
}

std::string next_after(const RepeatRule& rule, const std::string& current_iso,
                       const std::function<bool(const std::string&)>& is_holiday) {
    if (rule.freq.empty()) return "";
    std::string base = current_iso.empty() ? lunar::today_iso() : current_iso;

    std::string candidate;
    if (rule.freq == "daily") {
        candidate = lunar::add_days_iso(base, rule.interval);
    } else if (rule.freq == "weekly") {
        if (rule.weekdays.empty()) {
            // 无 BYDAY：按 interval 周推进
            candidate = lunar::add_days_iso(base, 7LL * rule.interval);
        } else {
            // 有 BYDAY：先推进到目标周，再找第一个匹配星期
            std::string d = lunar::add_days_iso(base, 7LL * (rule.interval - 1) + 1);
            for (int guard = 0; guard < 3660; ++guard, d = lunar::add_days_iso(d, 1)) {
                int w = lunar::weekday_of_iso(d);
                if (std::find(rule.weekdays.begin(), rule.weekdays.end(), w) != rule.weekdays.end()) {
                    candidate = d;
                    break;
                }
            }
            if (candidate.empty()) candidate = lunar::add_days_iso(base, 7LL * rule.interval);
        }
    } else if (rule.freq == "monthly") {
        // 按月份推进 interval 个月，保持日；日溢出时回退到当月月末
        int y = 0, m = 0, d = 0;
        std::sscanf(base.c_str(), "%d-%d-%d", &y, &m, &d);
        int total = y * 12 + (m - 1) + rule.interval;
        int ny = total / 12, nm = total % 12 + 1;
        // 计算目标月的实际天数（用"下月1号减1天"）
        char next_first[16];
        if (nm == 12) {
            std::snprintf(next_first, sizeof next_first, "%04d-01-01", ny + 1);
        } else {
            std::snprintf(next_first, sizeof next_first, "%04d-%02d-01", ny, nm + 1);
        }
        std::string prev = lunar::add_days_iso(next_first, -1);
        int maxd = std::atoi(prev.c_str() + 8);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", ny, nm, std::min(d, maxd));
        candidate = buf;
    } else if (rule.freq == "yearly") {
        int y = 0, m = 0, d = 0;
        std::sscanf(base.c_str(), "%d-%d-%d", &y, &m, &d);
        int maxd = (m == 2) ? 28 : (m == 4 || m == 6 || m == 9 || m == 11 ? 30 : 31);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y + rule.interval, m, std::min(d, maxd));
        candidate = buf;
        // 闰年 2-29 处理
        if (m == 2 && d == 29) {
            std::snprintf(buf, sizeof buf, "%04d-03-01", y + rule.interval);
            std::string m2 = lunar::add_days_iso(std::string(buf), -1);
            candidate = m2;
        }
    } else if (rule.freq == "custom") {
        // 自定义：按 interval 天推进
        candidate = lunar::add_days_iso(base, rule.interval);
    } else {
        return "";
    }

    if (!rule.end_date.empty() && candidate > rule.end_date) return "";
    // 跳过周末/节假日（向后顺延）
    return skip_skips(rule, candidate, is_holiday);
}

std::vector<std::string> occurrences_in_range(
    const RepeatRule& rule, const std::string& from_iso, const std::string& to_iso,
    const std::function<bool(const std::string&)>& is_holiday) {
    std::vector<std::string> out;
    if (rule.freq.empty() || to_iso < from_iso) return out;
    std::string cur = "";
    int guard = 0;
    while (guard++ < 20000) {
        std::string next = next_after(rule, cur, is_holiday);
        if (next.empty()) break;
        if (next > to_iso) break;
        if (next >= from_iso) out.push_back(next);
        cur = next;
    }
    return out;
}

std::string advance(const RepeatRule& rule, const std::string& anchor_iso, int n,
                    const std::function<bool(const std::string&)>& is_holiday) {
    std::string cur = anchor_iso;
    for (int i = 0; i < n; ++i) {
        cur = next_after(rule, cur, is_holiday);
        if (cur.empty()) break;
    }
    return cur;
}

} // namespace recurrence

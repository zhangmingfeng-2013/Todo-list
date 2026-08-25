// recurrence.hpp — 重复任务规则解析与实例生成
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <functional>

#include "json.hpp"

struct RepeatRule {
    std::string freq;            // daily | weekly | monthly | yearly | custom
    int interval = 1;            // 每 N 天/周/月
    std::vector<int> weekdays;   // 1=周一..7=周日（weekly/custom 时生效）
    bool skip_weekends = false;
    bool skip_holidays = false;
    std::string end_date;        // 可选，YYYY-MM-DD，含该日
    int max_instances = 0;       // 0 = 无限

    bool enabled() const { return !freq.empty(); }
    Json to_json() const;
    static RepeatRule from_json(const Json& j);
    static RepeatRule from_json_str(const std::string& s);
};

// 解析 Todoist/TickTick 的 RRULE 字符串
RepeatRule parse_rrule(const std::string& rrule);

namespace recurrence {

// 由当前日期生成"下一个"应出现的日期（严格晚于 current_iso；current 可为空串）
// holidays: 判断某日是否为节假日（用于 skip_holidays），可为空回调
std::string next_after(const RepeatRule& rule, const std::string& current_iso,
                       const std::function<bool(const std::string&)>& is_holiday);

// 生成 [from_iso, to_iso] 闭区间内所有实例日期（含规则结束约束）
std::vector<std::string> occurrences_in_range(
    const RepeatRule& rule, const std::string& from_iso, const std::string& to_iso,
    const std::function<bool(const std::string&)>& is_holiday);

// 将 daily/weekly 规则从某锚点(anchor_iso)推进 n 个周期后的日期
std::string advance(const RepeatRule& rule, const std::string& anchor_iso, int n,
                    const std::function<bool(const std::string&)>& is_holiday);

} // namespace recurrence

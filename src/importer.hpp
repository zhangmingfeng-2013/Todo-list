// importer.hpp — 导入滴答/Todoist/Todo.txt 数据
#pragma once
#include <string>
#include <vector>
#include <map>

#include "db.hpp"

struct ImportResult {
    int tasks = 0;        // 导入任务数（含子任务）
    int projects = 0;
    int tags = 0;
    std::vector<std::string> errors;   // 解析告警/错误
    std::string summary() const;
};

namespace importer {

// 根据 format 自动分派: todotxt | todoist | ticktick_json | ticktick_csv
ImportResult import_text(Db& db, const std::string& format, const std::string& text);

ImportResult import_todotxt(Db& db, const std::string& text);
ImportResult import_todoist_json(Db& db, const std::string& text);
ImportResult import_ticktick_json(Db& db, const std::string& text);
ImportResult import_ticktick_csv(Db& db, const std::string& text);

} // namespace importer

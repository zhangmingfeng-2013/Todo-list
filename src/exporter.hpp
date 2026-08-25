// exporter.hpp — 数据导出：Todo.txt / JSON / CSV
#pragma once
#include <string>
#include "db.hpp"

namespace exporter {

// 全量导出（不含回收站任务）。format: todotxt | json | csv
std::string export_tasks(Db& db, const std::string& format);

// 每日摘要文本（CLI digest / serve 启动横幅共用）
std::string daily_digest(Db& db);

} // namespace exporter

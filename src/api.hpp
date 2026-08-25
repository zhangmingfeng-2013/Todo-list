// api.hpp — REST API 路由注册
#pragma once
#include <string>

#include "db.hpp"
#include "http.hpp"

class Api {
public:
    Api(Db& db, const std::string& static_root);
    void register_routes(HttpServer& srv);

private:
    Db& db_;
    std::string static_root_;
    // 内部处理函数
    HttpResponse handle_storage(const HttpRequest& req);          // GET 当前存储信息
    HttpResponse handle_storage_volumes(const HttpRequest& req);  // GET 可用卷列表
    HttpResponse handle_storage_move(const HttpRequest& req);     // POST 迁移数据库
    HttpResponse handle_tasks(const HttpRequest& req);
    HttpResponse handle_task_detail(const HttpRequest& req, long long id);
    HttpResponse handle_task_update(const HttpRequest& req, long long id);
    HttpResponse handle_task_delete(const HttpRequest& req, long long id);
    HttpResponse handle_task_restore(const HttpRequest& req, long long id);   // POST 恢复
    HttpResponse handle_task_complete(const HttpRequest& req, long long id);
    HttpResponse handle_task_reopen(const HttpRequest& req, long long id);
    HttpResponse handle_deps(const HttpRequest& req, long long id);
    HttpResponse handle_trash(const HttpRequest& req);          // GET 回收站 / DELETE 清空
    HttpResponse handle_batch(const HttpRequest& req);          // POST 批量操作
    HttpResponse handle_reorder(const HttpRequest& req);        // POST 手动排序
    HttpResponse handle_pomodoro(const HttpRequest& req, long long id); // POST 番茄钟+1
    HttpResponse handle_stats(const HttpRequest& req);          // GET 统计仪表盘
    HttpResponse handle_export(const HttpRequest& req);         // GET 导出下载
    HttpResponse handle_backups(const HttpRequest& req);        // GET 列表 / POST 立即备份
    HttpResponse handle_holidays_auto(const HttpRequest& req);  // POST 生成某年法定节假日
    HttpResponse handle_digest(const HttpRequest& req);         // GET 每日摘要
    HttpResponse handle_quick_add(const HttpRequest& req);      // POST 自然语言快速录入（?preview=1 仅解析）
    HttpResponse handle_templates(const HttpRequest& req);      // GET 模板列表 / POST 保存模板
    HttpResponse handle_template_detail(const HttpRequest& req);// DELETE 删除模板 / POST 实例化模板
    HttpResponse handle_heatmap(const HttpRequest& req);        // GET 年度完成热力图数据
    HttpResponse handle_tree(const HttpRequest& req);
    HttpResponse handle_today(const HttpRequest& req);
    HttpResponse handle_calendar(const HttpRequest& req);
    HttpResponse handle_kanban(const HttpRequest& req);
    HttpResponse handle_projects(const HttpRequest& req);
    HttpResponse handle_tags(const HttpRequest& req);
    HttpResponse handle_filters(const HttpRequest& req);
    HttpResponse handle_import(const HttpRequest& req);
    HttpResponse handle_holidays(const HttpRequest& req);
    HttpResponse handle_search(const HttpRequest& req);
    HttpResponse handle_meta(const HttpRequest& req);
    // ---- 批次 B/C 新增 ----
    HttpResponse handle_undo(const HttpRequest& req);        // POST 撤销最近一次写操作 / GET 查询可撤销项
    HttpResponse handle_repeat_preview(const HttpRequest& req); // POST 重复规则预览（未来 N 次）
    HttpResponse handle_day(const HttpRequest& req);         // GET 时间块日视图数据
    HttpResponse handle_sync(const HttpRequest& req);        // POST 多端同步（合并导入快照）
};

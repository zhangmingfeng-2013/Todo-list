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
    HttpResponse handle_task_complete(const HttpRequest& req, long long id);
    HttpResponse handle_task_reopen(const HttpRequest& req, long long id);
    HttpResponse handle_deps(const HttpRequest& req, long long id);
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
};

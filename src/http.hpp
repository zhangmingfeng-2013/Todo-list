// http.hpp — 极简本地 HTTP 服务器（线程每连接）
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

struct HttpRequest {
    std::string method;      // GET/POST/PUT/DELETE
    std::string path;        // URL 解码后的路径，不含 query
    std::map<std::string, std::string> query;  // 已解码
    std::map<std::string, std::string> headers;
    std::string body;
    std::string client_ip;

    bool has_query(const std::string& k) const { return query.count(k) > 0; }
    std::string q(const std::string& k, const std::string& def = "") const {
        auto it = query.find(k);
        return it == query.end() ? def : it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;

    static HttpResponse json(int status, const std::string& json_body);
    static HttpResponse text(int status, const std::string& t);
    static HttpResponse html(const std::string& h);
    static HttpResponse error(int status, const std::string& msg);
};

// 路由处理器：返回 body 并自行设置状态；path 为请求路径
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    // root: 静态文件根目录
    explicit HttpServer(std::string static_root);
    ~HttpServer();

    // 注册 API 处理器（path 精确匹配或前缀匹配以 * 结尾）
    void on(const std::string& method, const std::string& path_prefix, HttpHandler handler);

    bool start(int port);
    void stop();
    int port() const { return port_; }

    // 阻塞监听（accept 循环）
    void run_loop();

private:
    std::string static_root_;
    int port_ = 0;
    int listen_fd_ = -1;
    volatile bool running_ = false;
    struct Route { std::string method; std::string prefix; HttpHandler handler; };
    std::vector<Route> routes_;

    void handle_client_with_ip(int fd, const std::string& ip);
    HttpResponse route(const HttpRequest& req);
    HttpResponse serve_static(const std::string& url_path);
};

// URL 工具
namespace url {
std::string decode(const std::string& s);
std::map<std::string, std::string> parse_query(const std::string& qs);
std::string mime_type(const std::string& path);
}

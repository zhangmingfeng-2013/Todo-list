// http.cpp — HTTP 服务器实现
#include "http.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <cstdio>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>

namespace url {

std::string decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out += static_cast<char>((h << 4) | l);
                i += 2;
                continue;
            }
        } else if (s[i] == '+') {
            out += ' ';
            continue;
        }
        out += s[i];
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& qs) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i <= qs.size()) {
        size_t amp = qs.find('&', i);
        std::string kv = qs.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        if (!kv.empty()) {
            size_t eq = kv.find('=');
            if (eq == std::string::npos) m[decode(kv)] = "";
            else m[decode(kv.substr(0, eq))] = decode(kv.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return m;
}

std::string mime_type(const std::string& path) {
    auto ext = path.substr(path.find_last_of('.') == std::string::npos
                               ? path.size() : path.find_last_of('.'));
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".map") return "application/json";
    return "application/octet-stream";
}

} // namespace url

HttpResponse HttpResponse::json(int status, const std::string& body) {
    HttpResponse r;
    r.status = status;
    r.content_type = "application/json; charset=utf-8";
    r.body = body;
    return r;
}
HttpResponse HttpResponse::text(int status, const std::string& t) {
    HttpResponse r;
    r.status = status;
    r.content_type = "text/plain; charset=utf-8";
    r.body = t;
    return r;
}
HttpResponse HttpResponse::html(const std::string& h) {
    HttpResponse r;
    r.status = 200;
    r.content_type = "text/html; charset=utf-8";
    r.body = h;
    return r;
}
HttpResponse HttpResponse::error(int status, const std::string& msg) {
    return json(status, "{\"ok\":false,\"error\":\"" + msg + "\"}");
}

HttpServer::HttpServer(std::string root) : static_root_(std::move(root)) {}
HttpServer::~HttpServer() { stop(); }

void HttpServer::on(const std::string& method, const std::string& prefix, HttpHandler h) {
    routes_.push_back({method, prefix, std::move(h)});
}

bool HttpServer::start(int port) {
    listen_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd_ < 0) return false;
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 仅本机
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0)
        return false;
    if (::listen(listen_fd_, 32) != 0) return false;
    port_ = port;
    running_ = true;
    return true;
}

void HttpServer::stop() {
    running_ = false;
    if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); ::close(listen_fd_); listen_fd_ = -1; }
}

void HttpServer::run_loop() {
    while (running_) {
        struct sockaddr_in caddr{};
        socklen_t len = sizeof caddr;
        int fd = static_cast<int>(::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&caddr), &len));
        if (fd < 0) {
            if (!running_) break;
            continue;
        }
        std::thread([this, fd, caddr]() {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            // 记录客户端 IP
            char ip[INET_ADDRSTRLEN] = "127.0.0.1";
            inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof ip);
            handle_client_with_ip(fd, ip);
            ::close(fd);
        }).detach();
    }
}

// 读取完整请求（有限大小保护）
static bool read_request(int fd, std::string& out) {
    out.clear();
    char buf[16384];
    std::string raw;
    // 先读取头部
    while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 65536) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return false;
        raw.append(buf, static_cast<size_t>(n));
    }
    size_t header_end = raw.find("\r\n\r\n");
    std::string headers = raw.substr(0, header_end);
    out = raw;
    // 解析 Content-Length
    size_t cl = 0;
    std::istringstream hs(headers);
    std::string line;
    while (std::getline(hs, line)) {
        if (line.rfind("Content-Length:", 0) == 0 || line.rfind("content-length:", 0) == 0) {
            cl = static_cast<size_t>(std::atoll(line.substr(line.find(':') + 1).c_str()));
        }
    }
    if (cl > 64 * 1024 * 1024) return false;  // 拒绝超大 body
    size_t have = out.size() - (header_end + 4);
    while (have < cl) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return false;
        out.append(buf, static_cast<size_t>(n));
        have += static_cast<size_t>(n);
    }
    return true;
}

void HttpServer::handle_client_with_ip(int fd, const std::string& ip) {
    std::string raw;
    if (!read_request(fd, raw)) return;
    // 解析请求行
    size_t eol = raw.find("\r\n");
    if (eol == std::string::npos) return;
    std::istringstream line(raw.substr(0, eol));
    std::string method, target, version;
    line >> method >> target >> version;

    HttpRequest req;
    req.method = method;
    req.client_ip = ip;
    // 分离 query
    size_t qpos = target.find('?');
    if (qpos == std::string::npos) {
        req.path = url::decode(target);
    } else {
        req.path = url::decode(target.substr(0, qpos));
        req.query = url::parse_query(target.substr(qpos + 1));
    }
    // 头部
    size_t hstart = eol + 2;
    size_t hend = raw.find("\r\n\r\n", hstart);
    if (hend == std::string::npos) return;
    {
        std::istringstream hs(raw.substr(hstart, hend - hstart));
        std::string hline;
        while (std::getline(hs, hline)) {
            if (hline.empty() || hline.back() == '\r') hline.pop_back();
            auto c = hline.find(':');
            if (c != std::string::npos) {
                std::string k = hline.substr(0, c);
                std::string v = hline.substr(c + 1);
                size_t s = v.find_first_not_of(' ');
                if (s != std::string::npos) v = v.substr(s);
                req.headers[k] = v;
            }
        }
    }
    req.body = raw.substr(hend + 4);

    HttpResponse resp = route(req);

    std::ostringstream out;
    static const char* status_text[] = {"", "", "", "", "", ""};
    (void)status_text;
    const char* st = "OK";
    switch (resp.status) {
        case 200: st = "OK"; break;
        case 201: st = "Created"; break;
        case 204: st = "No Content"; break;
        case 400: st = "Bad Request"; break;
        case 404: st = "Not Found"; break;
        case 405: st = "Method Not Allowed"; break;
        case 409: st = "Conflict"; break;
        case 500: st = "Internal Server Error"; break;
        default: st = "OK"; break;
    }
    out << "HTTP/1.1 " << resp.status << " " << st << "\r\n";
    out << "Content-Type: " << resp.content_type << "\r\n";
    out << "Content-Length: " << resp.body.size() << "\r\n";
    out << "Cache-Control: no-store\r\n";
    out << "Connection: close\r\n";
    for (auto& kv : resp.headers) out << kv.first << ": " << kv.second << "\r\n";
    out << "\r\n";
    out << resp.body;
    std::string full = out.str();
    size_t sent = 0;
    while (sent < full.size()) {
        ssize_t n = ::send(fd, full.data() + sent, full.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

HttpResponse HttpServer::route(const HttpRequest& req) {
    for (auto& rt : routes_) {
        if (rt.method != req.method) continue;
        if (rt.prefix.back() == '*') {
            std::string base = rt.prefix.substr(0, rt.prefix.size() - 1);
            if (req.path.rfind(base, 0) == 0) return rt.handler(req);
        } else if (rt.prefix == req.path) {
            return rt.handler(req);
        }
    }
    if (req.method == "GET") return serve_static(req.path);
    return HttpResponse::error(404, "not found");
}

HttpResponse HttpServer::serve_static(const std::string& url_path) {
    std::string rel = url_path;
    if (rel == "/") rel = "/index.html";
    // 防目录穿越
    if (rel.find("..") != std::string::npos)
        return HttpResponse::error(400, "bad path");
    std::string full = static_root_ + rel;
    struct stat st{};
    if (::stat(full.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        // 尝试 .html 后缀
        std::string alt = full + ".html";
        if (::stat(alt.c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) full = alt;
        else return HttpResponse::error(404, "not found");
    }
    std::ifstream f(full, std::ios::binary);
    if (!f) return HttpResponse::error(404, "not found");
    std::ostringstream ss;
    ss << f.rdbuf();
    HttpResponse r;
    r.status = 200;
    r.content_type = url::mime_type(full);
    r.body = ss.str();
    return r;
}

// ai/cpp/ai_gateway.hpp
// cpp-todo AI 网关（参考实现，P0 尚未接入构建）
//
// 接入步骤：
//   1) CMakeLists.txt 增加 find_package(CURL REQUIRED) 与 target_link_libraries(cpp-todo CURL::libcurl)
//   2) 在 main.cpp 注册 /api/ai/* 路由，将请求体转发到本网关 post()
//   3) 调用示例：AiGateway::instance().post("/api/decompose", R"({"goal":"备考期末"})")
#pragma once
#include <string>

struct AiGateway {
    static AiGateway& instance();
    // 向本地 AI 服务发送 JSON，返回响应体（失败返回空串）
    std::string post(const std::string& path, const std::string& json_body);
    void configure(const std::string& base_url, const std::string& api_key);
private:
    AiGateway() = default;
    std::string base_url_ = "http://127.0.0.1:8777";
    std::string api_key_ = "sk-no-key";
};

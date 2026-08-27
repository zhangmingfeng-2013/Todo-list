// ai/cpp/ai_gateway.cpp  (参考实现，需 libcurl)
#include "ai_gateway.hpp"
#include <curl/curl.h>

static size_t write_cb(void* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

AiGateway& AiGateway::instance() {
    static AiGateway g;
    return g;
}

void AiGateway::configure(const std::string& base_url, const std::string& api_key) {
    base_url_ = base_url;
    api_key_ = api_key;
}

std::string AiGateway::post(const std::string& path, const std::string& json_body) {
    std::string url = base_url_ + path;
    std::string resp;
    CURL* c = curl_easy_init();
    if (!c) return "";
    struct curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + api_key_;
    hdr = curl_slist_append(hdr, auth.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? resp : "";
}

std::string AiGateway::get(const std::string& path) {
    std::string url = base_url_ + path;
    std::string resp;
    CURL* c = curl_easy_init();
    if (!c) return "";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? resp : "";
}

// json.hpp — 自包含最小 JSON 解析/序列化（无第三方依赖）
// 支持 RFC 8259 子集：object / array / string / number / bool / null
#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <stdexcept>
#include <cstdint>
#include <cmath>
#include <cctype>

class Json {
public:
    enum class Type { Null, Bool, Num, Str, Arr, Obj };

    Json() : node_(std::make_shared<Node>()) {}
    Json(std::nullptr_t) : Json() {}
    Json(bool b) : node_(std::make_shared<Node>()) { node_->type = Type::Bool; node_->b = b; }
    Json(int v) : Json(static_cast<long long>(v)) {}
    Json(long v) : Json(static_cast<long long>(v)) {}
    Json(long long v) : node_(std::make_shared<Node>()) {
        node_->type = Type::Num; node_->n = static_cast<double>(v);
    }
    Json(double v) : node_(std::make_shared<Node>()) { node_->type = Type::Num; node_->n = v; }
    Json(const char* s) : Json(std::string(s ? s : "")) {}
    Json(const std::string& s) : node_(std::make_shared<Node>()) {
        node_->type = Type::Str; node_->s = s;
    }
    Json(std::string&& s) : node_(std::make_shared<Node>()) {
        node_->type = Type::Str; node_->s = std::move(s);
    }

    // 深拷贝
    Json(const Json& o) : node_(o.node_) {}
    Json& operator=(const Json& o) { node_ = o.node_; return *this; }
    Json(Json&&) noexcept = default;
    Json& operator=(Json&&) noexcept = default;

    static Json array()  { Json j; j.node_->type = Type::Arr; return j; }
    static Json object() { Json j; j.node_->type = Type::Obj; return j; }

    // ---- 类型判断 ----
    Type type() const { return node_->type; }
    bool is_null() const   { return node_->type == Type::Null; }
    bool is_bool() const   { return node_->type == Type::Bool; }
    bool is_number() const { return node_->type == Type::Num; }
    bool is_string() const { return node_->type == Type::Str; }
    bool is_array() const  { return node_->type == Type::Arr; }
    bool is_object() const { return node_->type == Type::Obj; }
    explicit operator bool() const { return !is_null(); }

    // ---- 取值（不安全的 getter，调用前请先 is_*）----
    bool as_bool() const { return node_->b; }
    double as_number() const { return node_->n; }
    long long as_int() const { return static_cast<long long>(std::llround(node_->n)); }
    const std::string& as_string() const { return node_->s; }
    std::string as_string_or(const std::string& def) const {
        return is_string() ? node_->s : def;
    }
    long long as_int_or(long long def) const {
        return is_number() ? as_int() : def;
    }
    bool as_bool_or(bool def) const {
        return is_bool() ? as_bool() : def;
    }

    // ---- 数组操作 ----
    size_t size() const {
        if (node_->type == Type::Arr) return node_->arr.size();
        if (node_->type == Type::Obj) return node_->obj.size();
        return 0;
    }
    bool empty() const {
        if (node_->type == Type::Obj) return node_->obj.empty();
        if (node_->type == Type::Arr) return node_->arr.empty();
        if (node_->type == Type::Str) return node_->s.empty();
        return false;
    }
    Json& at(size_t i) { return node_->arr.at(i); }
    const Json& at(size_t i) const { return node_->arr.at(i); }
    Json& operator[](size_t i) { return node_->arr[i]; }
    const Json& operator[](size_t i) const { return node_->arr[i]; }
    void push_back(const Json& v) { node_->arr.push_back(v); }
    void push_back(Json&& v) { node_->arr.push_back(std::move(v)); }
    void clear() { node_->arr.clear(); }
    // 数组迭代支持（仅对数组有效）
    Json* begin() { return node_->arr.data(); }
    Json* end() { return node_->arr.data() + node_->arr.size(); }
    const Json* begin() const { return node_->arr.data(); }
    const Json* end() const { return node_->arr.data() + node_->arr.size(); }

    // ---- 对象操作 ----
    bool has(const std::string& k) const { return node_->obj.count(k) > 0; }
    // 不存在的 key 会创建一个 Null 值节点（语义同 nlohmann::json）
    Json& operator[](const std::string& k) { return node_->obj[k]; }
    const Json& operator[](const std::string& k) const {
        static const Json nullJson;
        auto it = node_->obj.find(k);
        return it == node_->obj.end() ? nullJson : it->second;
    }
    // 安全读取：返回 null 节点而非抛异常
    const Json& get(const std::string& k) const { return (*this)[k]; }
    std::string get_str(const std::string& k) const { return (*this)[k].as_string_or(""); }
    long long get_int(const std::string& k) const { return (*this)[k].as_int_or(0); }
    bool get_bool(const std::string& k) const { return (*this)[k].as_bool_or(false); }

    // ---- 序列化 ----
    std::string dump() const {
        std::string out;
        write(out);
        return out;
    }
    std::string dump_pretty() const {
        std::string out;
        write_pretty(out, 0);
        return out;
    }

    // ---- 解析：失败抛 std::runtime_error ----
    static Json parse(const std::string& text) {
        Parser p(text);
        Json v = p.parse_value();
        p.skip_ws();
        if (!p.eof()) throw std::runtime_error("json: trailing characters at offset " +
                                               std::to_string(p.pos()));
        return v;
    }

private:
    struct Node {
        Type type = Type::Null;
        bool b = false;
        double n = 0.0;
        std::string s;
        std::vector<Json> arr;
        std::map<std::string, Json> obj;
    };
    std::shared_ptr<Node> node_;

    void write(std::string& out) const {
        switch (node_->type) {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += node_->b ? "true" : "false"; break;
            case Type::Num: {
                if (node_->n == static_cast<long long>(node_->n) &&
                    std::fabs(node_->n) < 9.007199254740992e15) {
                    out += std::to_string(static_cast<long long>(node_->n));
                } else {
                    out += std::to_string(node_->n);
                }
                break;
            }
            case Type::Str: {
                out += '"';
                for (unsigned char c : node_->s) {
                    switch (c) {
                        case '"':  out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\b': out += "\\b";  break;
                        case '\f': out += "\\f";  break;
                        case '\n': out += "\\n";  break;
                        case '\r': out += "\\r";  break;
                        case '\t': out += "\\t";  break;
                        default:
                            if (c < 0x20) {
                                char buf[8];
                                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                                out += buf;
                            } else {
                                out += static_cast<char>(c);
                            }
                    }
                }
                out += '"';
                break;
            }
            case Type::Arr: {
                out += '[';
                for (size_t i = 0; i < node_->arr.size(); ++i) {
                    if (i) out += ',';
                    node_->arr[i].write(out);
                }
                out += ']';
                break;
            }
            case Type::Obj: {
                out += '{';
                bool first = true;
                for (auto& kv : node_->obj) {
                    if (!first) out += ',';
                    first = false;
                    Json key(kv.first);
                    key.write(out);
                    out += ':';
                    kv.second.write(out);
                }
                out += '}';
                break;
            }
        }
    }

    void write_pretty(std::string& out, int indent) const {
        const std::string pad(static_cast<size_t>(indent) * 2, ' ');
        const std::string pad2(static_cast<size_t>(indent + 1) * 2, ' ');
        switch (node_->type) {
            case Type::Null: case Type::Bool: case Type::Num: case Type::Str:
                write(out);
                break;
            case Type::Arr: {
                if (node_->arr.empty()) { out += "[]"; break; }
                out += "[";
                for (size_t i = 0; i < node_->arr.size(); ++i) {
                    if (i) out += ',';
                    out += '\n'; out += pad2;
                    node_->arr[i].write_pretty(out, indent + 1);
                }
                out += '\n'; out += pad; out += ']';
                break;
            }
            case Type::Obj: {
                if (node_->obj.empty()) { out += "{}"; break; }
                out += "{";
                bool first = true;
                for (auto& kv : node_->obj) {
                    if (!first) out += ',';
                    first = false;
                    out += '\n'; out += pad2;
                    Json key(kv.first);
                    key.write(out);
                    out += ": ";
                    kv.second.write_pretty(out, indent + 1);
                }
                out += '\n'; out += pad; out += '}';
                break;
            }
        }
    }

    struct Parser {
        const std::string& text;
        size_t i = 0;
        explicit Parser(const std::string& t) : text(t) {}
        size_t pos() const { return i; }
        bool eof() const { return i >= text.size(); }

        void skip_ws() {
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        }

        [[noreturn]] void fail(const std::string& msg) const {
            throw std::runtime_error("json: " + msg + " at offset " + std::to_string(i));
        }

        char peek() { skip_ws(); return i < text.size() ? text[i] : '\0'; }

        Json parse_value() {
            skip_ws();
            if (i >= text.size()) fail("unexpected end");
            char c = text[i];
            switch (c) {
                case '{': return parse_object();
                case '[': return parse_array();
                case '"': return Json(parse_string());
                case 't': expect("true"); return Json(true);
                case 'f': expect("false"); return Json(false);
                case 'n': expect("null"); return Json(nullptr);
                default:
                    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
                    fail("unexpected character");
            }
        }

        void expect(const char* word) {
            size_t len = std::strlen(word);
            if (text.compare(i, len, word) != 0) fail("invalid literal");
            i += len;
        }

        std::string parse_string() {
            if (text[i] != '"') fail("expected string");
            ++i;
            std::string out;
            while (true) {
                if (i >= text.size()) fail("unterminated string");
                unsigned char c = text[i];
                if (c == '"') { ++i; return out; }
                if (c == '\\') {
                    ++i;
                    if (i >= text.size()) fail("bad escape");
                    char e = text[i++];
                    switch (e) {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '/': out += '/'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'u': {
                            unsigned cp = parse_hex4();
                            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size() && text[i] == '\\' && text[i+1] == 'u') {
                                i += 2;
                                unsigned lo = parse_hex4();
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                } else {
                                    out += static_cast<char>(0xEF);
                                    out += static_cast<char>(0xBF);
                                    out += static_cast<char>(0xBD);
                                    // 回溯不必要，简化处理
                                }
                            }
                            append_utf8(out, cp);
                            break;
                        }
                        default: fail("bad escape");
                    }
                } else {
                    out += static_cast<char>(c);
                    ++i;
                }
            }
        }

        unsigned parse_hex4() {
            if (i + 4 > text.size()) fail("bad \\u escape");
            unsigned v = 0;
            for (int k = 0; k < 4; ++k) {
                char c = text[i++];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
                else fail("bad hex digit");
            }
            return v;
        }

        static void append_utf8(std::string& out, unsigned cp) {
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        Json parse_number() {
            size_t start = i;
            if (text[i] == '-') ++i;
            while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
            if (i < text.size() && text[i] == '.') {
                ++i;
                while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
            }
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                ++i;
                if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
                while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
            }
            std::string num = text.substr(start, i - start);
            if (num.empty() || num == "-") fail("invalid number");
            return Json(std::strtod(num.c_str(), nullptr));
        }

        Json parse_object() {
            Json obj = Json::object();
            ++i; // '{'
            skip_ws();
            if (i < text.size() && text[i] == '}') { ++i; return obj; }
            while (true) {
                skip_ws();
                if (i >= text.size() || text[i] != '"') fail("expected string key");
                std::string key = parse_string();
                skip_ws();
                if (i >= text.size() || text[i] != ':') fail("expected ':'");
                ++i;
                obj[key] = parse_value();
                skip_ws();
                if (i >= text.size()) fail("unterminated object");
                if (text[i] == ',') { ++i; continue; }
                if (text[i] == '}') { ++i; break; }
                fail("expected ',' or '}'");
            }
            return obj;
        }

        Json parse_array() {
            Json arr = Json::array();
            ++i; // '['
            skip_ws();
            if (i < text.size() && text[i] == ']') { ++i; return arr; }
            while (true) {
                arr.push_back(parse_value());
                skip_ws();
                if (i >= text.size()) fail("unterminated array");
                if (text[i] == ',') { ++i; continue; }
                if (text[i] == ']') { ++i; break; }
                fail("expected ',' or ']'");
            }
            return arr;
        }
    };
};

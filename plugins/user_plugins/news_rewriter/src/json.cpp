#include "json.h"

#include <cstdio>
#include <cmath>
#include <sstream>

namespace news_rewriter {

Json::Json() = default;

Json::Json(std::nullptr_t) {}

Json::Json(bool value) : type_(Type::Bool), bool_(value) {}

Json::Json(int value) : type_(Type::Number), number_(static_cast<double>(value)) {}

Json::Json(int64_t value) : type_(Type::Number), number_(static_cast<double>(value)) {}

Json::Json(double value) : type_(Type::Number), number_(value) {}

Json::Json(const char* value)
    : type_(Type::String), string_(value ? value : "") {}

Json::Json(const std::string& value)
    : type_(Type::String), string_(value) {}

Json Json::null_value() {
    Json j;
    j.type_ = Type::Null;
    return j;
}

Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

bool Json::as_bool(bool fallback) const {
    return type_ == Type::Bool ? bool_ : fallback;
}

int64_t Json::as_int(int64_t fallback) const {
    if (type_ == Type::Number) return static_cast<int64_t>(number_);
    if (type_ == Type::String) {
        try {
            return static_cast<int64_t>(std::stoll(string_));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

double Json::as_double(double fallback) const {
    if (type_ == Type::Number) return number_;
    if (type_ == Type::String) {
        try {
            return std::stod(string_);
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

const std::string& Json::as_string() const {
    static const std::string empty;
    return type_ == Type::String ? string_ : empty;
}

std::string Json::as_string(const std::string& fallback) const {
    return type_ == Type::String ? string_ : fallback;
}

std::size_t Json::size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

bool Json::empty() const {
    if (type_ == Type::Array) return array_.empty();
    if (type_ == Type::Object) return object_.empty();
    return true;
}

bool Json::contains(const std::string& key) const {
    return type_ == Type::Object && object_.find(key) != object_.end();
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        *this = object();
    }
    return object_[key];
}

Json& Json::operator[](std::size_t index) {
    if (type_ != Type::Array || index >= array_.size()) {
        static Json null = null_value();
        return null;
    }
    return array_[index];
}

const Json& Json::operator[](std::size_t index) const {
    static const Json null = null_value();
    if (type_ != Type::Array || index >= array_.size()) return null;
    return array_[index];
}

const Json& Json::get(const std::string& key) const {
    static const Json null = null_value();
    if (type_ != Type::Object) return null;
    auto it = object_.find(key);
    return it == object_.end() ? null : it->second;
}

Json& Json::get(const std::string& key) {
    static Json null = null_value();
    if (type_ != Type::Object) return null;
    auto it = object_.find(key);
    return it == object_.end() ? null : it->second;
}

const std::vector<std::string> Json::keys() const {
    std::vector<std::string> result;
    if (type_ != Type::Object) return result;
    result.reserve(object_.size());
    for (const auto& kv : object_) result.push_back(kv.first);
    return result;
}

void Json::push(const Json& value) {
    if (type_ != Type::Array) {
        *this = array();
    }
    array_.push_back(value);
}

// ============================================================================
// Парсер
// ============================================================================

namespace {

struct Parser {
    const std::string& s;
    std::size_t pos = 0;
    std::string error;

    explicit Parser(const std::string& text) : s(text) {}

    void skip_ws() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' ||
                                  s[pos] == '\n' || s[pos] == '\r')) {
            pos++;
        }
    }

    bool fail(const std::string& msg) {
        if (error.empty()) error = msg + " (pos " + std::to_string(pos) + ")";
        return false;
    }

    bool literal(const char* word) {
        const std::size_t len = std::char_traits<char>::length(word);
        if (s.compare(pos, len, word) != 0) return fail("ожидалось '" + std::string(word) + "'");
        pos += len;
        return true;
    }

    bool parse_string(std::string& out) {
        skip_ws();
        if (pos >= s.size() || s[pos] != '"') return fail("ожидалась строка");
        pos++;
        out.clear();
        while (pos < s.size()) {
            const char c = s[pos];
            if (c == '"') {
                pos++;
                return true;
            }
            if (c == '\\') {
                pos++;
                if (pos >= s.size()) return fail("незавершённый escape");
                const char e = s[pos];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (pos + 4 >= s.size()) return fail("короткий \\u");
                        const std::string hex = s.substr(pos + 1, 4);
                        unsigned int code = 0;
                        for (char h : hex) {
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            else return fail("неверный \\u");
                        }
                        if (code < 0x80) {
                            out += static_cast<char>(code);
                        } else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        pos += 4;
                        break;
                    }
                    default: return fail("неизвестный escape");
                }
                pos++;
            } else {
                out += c;
                pos++;
            }
        }
        return fail("незакрытая строка");
    }

    bool parse_number(Json& out) {
        skip_ws();
        const std::size_t start = pos;
        if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) pos++;
        bool any_digit = false;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') { pos++; any_digit = true; }
        if (pos < s.size() && s[pos] == '.') {
            pos++;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') { pos++; any_digit = true; }
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') { pos++; any_digit = true; }
        }
        if (!any_digit) return fail("неверное число");
        const std::string token = s.substr(start, pos - start);
        out = Json(std::stod(token));
        return true;
    }

    bool parse_value(Json& out) {
        skip_ws();
        if (pos >= s.size()) return fail("неожиданный конец JSON");
        const char c = s[pos];
        if (c == '"') {
            std::string str;
            if (!parse_string(str)) return false;
            out = Json(str);
            return true;
        }
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == 't') { if (!literal("true")) return false; out = Json(true); return true; }
        if (c == 'f') { if (!literal("false")) return false; out = Json(false); return true; }
        if (c == 'n') { if (!literal("null")) return false; out = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
        return fail(std::string("неожиданный символ '") + c + "'");
    }

    bool parse_object(Json& out) {
        skip_ws();
        if (pos >= s.size() || s[pos] != '{') return fail("ожидался '{'");
        pos++;
        out = Json::object();
        skip_ws();
        if (pos < s.size() && s[pos] == '}') { pos++; return true; }
        for (;;) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos >= s.size() || s[pos] != ':') return fail("ожидался ':'");
            pos++;
            Json value;
            if (!parse_value(value)) return false;
            out[key] = value;
            skip_ws();
            if (pos >= s.size()) return fail("незакрытый объект");
            if (s[pos] == ',') { pos++; continue; }
            if (s[pos] == '}') { pos++; return true; }
            return fail("ожидались ',' или '}'");
        }
    }

    bool parse_array(Json& out) {
        skip_ws();
        if (pos >= s.size() || s[pos] != '[') return fail("ожидался '['");
        pos++;
        out = Json::array();
        skip_ws();
        if (pos < s.size() && s[pos] == ']') { pos++; return true; }
        for (;;) {
            Json value;
            if (!parse_value(value)) return false;
            out.push(value);
            skip_ws();
            if (pos >= s.size()) return fail("незакрытый массив");
            if (s[pos] == ',') { pos++; continue; }
            if (s[pos] == ']') { pos++; return true; }
            return fail("ожидались ',' или ']'");
        }
    }
};

} // namespace

Json Json::parse(const std::string& text, bool* ok, std::string* error) {
    Parser p(text);
    Json result;
    if (p.parse_value(result)) {
        p.skip_ws();
        if (p.pos != text.size()) {
            p.fail("лишние символы после значения");
        }
    }
    if (p.error.empty()) {
        if (ok) *ok = true;
        return result;
    }
    if (ok) *ok = false;
    if (error) *error = p.error;
    return null_value();
}

// ============================================================================
// Сериализатор
// ============================================================================

namespace {

std::string escape_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '"';
    for (unsigned char c : s) {
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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

} // namespace

std::string Json::dump() const {
    switch (type_) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return bool_ ? "true" : "false";
        case Type::Number: {
            if (std::isfinite(number_) && number_ == static_cast<int64_t>(number_)) {
                return std::to_string(static_cast<int64_t>(number_));
            }
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", number_);
            return buf;
        }
        case Type::String:
            return escape_string(string_);
        case Type::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < array_.size(); i++) {
                if (i) out += ",";
                out += array_[i].dump();
            }
            out += "]";
            return out;
        }
        case Type::Object: {
            std::string out = "{";
            bool first = true;
            for (const auto& kv : object_) {
                if (!first) out += ",";
                first = false;
                out += escape_string(kv.first);
                out += ":";
                out += kv.second.dump();
            }
            out += "}";
            return out;
        }
    }
    return "null";
}

} // namespace news_rewriter

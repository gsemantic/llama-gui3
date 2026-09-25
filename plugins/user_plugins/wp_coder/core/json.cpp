// json.cpp — реализация JsonValue (И1.1).
//
// Парсер написан с нуля на std::string_view-подобном индексаторе:
// без рекурсивной descent-функции с исключениями и без аллокаций
// на каждый узел. Глубина вложенности ограничена — это защита от
// «краша стека» на злонамеренном или сломанном входе модели.

#include "json.h"
#include "json_utils.h"   // text::sanitize_utf8, text::append_utf8, json::escape

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace coder {
namespace json {

/* Глубина вложенности, дальше — ошибка разбора. Модели иногда
 * «забывают» закрывающую скобку и генерируют тысячи вложенных массивов;
 * рекурсивный разбор такого переполнит стек воркер-треда. */
constexpr int kMaxDepth = 128;

const std::string& null_string() {
    static const std::string empty;
    return empty;
}

const JsonValue& null_value() {
    static const JsonValue v;
    return v;
}

/* ======================================================================
 * Impl
 * ====================================================================== */

struct JsonValue::Impl {
    JsonValue::Type type = JsonValue::Type::Null;
    bool boolean = false;
    long long integer = 0;
    double real = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    /* Порядок вставки: сериализованная схема обязана быть детерминированной. */
    std::vector<std::pair<std::string, JsonValue>> obj;

    Impl() = default;
    explicit Impl(JsonValue::Type t) : type(t) {}

    const JsonValue* find_member(const std::string& key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

/* ======================================================================
 * Конструкторы
 * ====================================================================== */

JsonValue::JsonValue() : impl_(new Impl(Type::Null)) {}
JsonValue::JsonValue(Type t) : impl_(new Impl(t)) {}
JsonValue::JsonValue(std::nullptr_t) : impl_(new Impl(Type::Null)) {}
JsonValue::JsonValue(bool v) : impl_(new Impl(Type::Bool)) { impl_->boolean = v; }
JsonValue::JsonValue(int v) : impl_(new Impl(Type::Int)) { impl_->integer = v; }
JsonValue::JsonValue(long long v) : impl_(new Impl(Type::Int)) { impl_->integer = v; }

JsonValue::JsonValue(double v) : impl_(new Impl(Type::Double)) {
    impl_->real = v;
    /* Целое значение храним как Int: иначе "5" и 5 сериализуются
     * по-разному (5.0 против 5) и валидация схемы сочтёт тип неверным. */
    if (std::isfinite(v) && v == static_cast<double>(static_cast<long long>(v))) {
        impl_->type = Type::Int;
        impl_->integer = static_cast<long long>(v);
    }
}

JsonValue::JsonValue(const char* v) : impl_(new Impl(Type::String)) {
    if (v) impl_->str = v;
}

JsonValue::JsonValue(std::string v) : impl_(new Impl(Type::String)) {
    impl_->str = std::move(v);
}

JsonValue::JsonValue(const JsonValue& other)
    : impl_(new Impl(*other.impl_)) {}

JsonValue::JsonValue(JsonValue&& other) noexcept
    : impl_(std::move(other.impl_)) {
    if (!impl_) impl_.reset(new Impl(Type::Null));
}

JsonValue& JsonValue::operator=(const JsonValue& other) {
    if (this != &other) *impl_ = *other.impl_;
    return *this;
}

JsonValue& JsonValue::operator=(JsonValue&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        if (!impl_) impl_.reset(new Impl(Type::Null));
    }
    return *this;
}

JsonValue::~JsonValue() = default;

JsonValue JsonValue::object() { return JsonValue(Type::Object); }
JsonValue JsonValue::array() { return JsonValue(Type::Array); }

/* ======================================================================
 * Разбор
 * ====================================================================== */

namespace {

class Parser {
public:
    Parser(const std::string& text) : s_(text) {}

    bool parse_value(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return fail("слишком глубокая вложенность");
        skip_ws();
        if (i_ >= s_.size()) return fail("неожиданный конец ввода");
        char c = s_[i_];
        switch (c) {
            case '{': return parse_object(out, depth);
            case '[': return parse_array(out, depth);
            case '"': {
                std::string str;
                if (!parse_string(str)) return false;
                out = JsonValue(std::move(str));
                return true;
            }
            case 't': return parse_literal("true", JsonValue(true), out);
            case 'f': return parse_literal("false", JsonValue(false), out);
            case 'n': return parse_literal("null", JsonValue(), out);
            default:  return parse_number(out);
        }
    }

    void skip_ws() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i_; continue; }
            break;
        }
    }

    size_t pos() const { return i_; }
    const std::string& error() const { return error_; }

private:
    const std::string& s_;
    size_t i_ = 0;
    std::string error_;

    bool fail(const char* what) {
        if (error_.empty()) {
            error_ = std::string(what) + " (позиция " + std::to_string(i_) + ")";
        }
        return false;
    }

    bool parse_literal(const char* lit, const JsonValue& value, JsonValue& out) {
        size_t n = 0;
        while (lit[n]) ++n;
        if (s_.compare(i_, n, lit) != 0) return fail("неожиданный литерал");
        i_ += n;
        out = value;
        return true;
    }

    bool parse_number(JsonValue& out) {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        bool has_digits = false;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; has_digits = true; }
        bool is_real = false;
        if (i_ < s_.size() && s_[i_] == '.') {
            is_real = true;
            ++i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; has_digits = true; }
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            is_real = true;
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        if (!has_digits) {
            i_ = start;
            return fail("ожидалось число");
        }
        std::string num = s_.substr(start, i_ - start);
        if (is_real) {
            out = JsonValue(std::strtod(num.c_str(), nullptr));
        } else {
            errno = 0;
            char* end = nullptr;
            long long v = std::strtoll(num.c_str(), &end, 10);
            /* Переполнение int64 — это уже double, иначе получим мусор. */
            out = (errno == ERANGE) ? JsonValue(std::strtod(num.c_str(), nullptr))
                                    : JsonValue(v);
        }
        return true;
    }

    static int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool parse_hex4(std::uint32_t& cp) {
        if (i_ + 4 > s_.size()) return fail("обрезан \\u-escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            int h = hex_value(s_[i_ + k]);
            if (h < 0) return fail("невалидный \\u-escape");
            cp = (cp << 4) | static_cast<std::uint32_t>(h);
        }
        i_ += 4;
        return true;
    }

    bool parse_string(std::string& dst) {
        if (i_ >= s_.size() || s_[i_] != '"') return fail("ожидалась строка");
        ++i_;
        dst.clear();
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == '"') { ++i_; return true; }
            if (c == '\\') {
                ++i_;
                if (i_ >= s_.size()) return fail("незавершённый escape");
                char e = s_[i_++];
                switch (e) {
                    case '"':  dst += '"';  break;
                    case '\\': dst += '\\'; break;
                    case '/':  dst += '/';  break;
                    case 'b':  dst += '\b'; break;
                    case 'f':  dst += '\f'; break;
                    case 'n':  dst += '\n'; break;
                    case 'r':  dst += '\r'; break;
                    case 't':  dst += '\t'; break;
                    case 'u': {
                        std::uint32_t cp = 0;
                        if (!parse_hex4(cp)) return false;
                        /* Сурогатная пара: \uD83D\uDE00 */
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                            size_t save = i_;
                            i_ += 2;
                            std::uint32_t low = 0;
                            if (parse_hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                i_ = save;
                                error_.clear();
                                cp = 0xFFFD;
                            }
                        }
                        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                        text::append_utf8(cp, dst);
                        break;
                    }
                    default: dst += e; break;
                }
                continue;
            }
            dst += c;
            ++i_;
        }
        return fail("строка не закрыта");
    }

    bool parse_array(JsonValue& out, int depth) {
        ++i_;  // '['
        JsonValue arr = JsonValue::array();
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; out = std::move(arr); return true; }
        for (;;) {
            JsonValue item;
            if (!parse_value(item, depth + 1)) return false;
            arr.push_back(std::move(item));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; break; }
            return fail("ожидалась ',' или ']'");
        }
        out = std::move(arr);
        return true;
    }

    bool parse_object(JsonValue& out, int depth) {
        ++i_;  // '{'
        JsonValue obj = JsonValue::object();
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; out = std::move(obj); return true; }
        for (;;) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("ожидалось ':'");
            ++i_;
            JsonValue val;
            if (!parse_value(val, depth + 1)) return false;
            obj.set(key, std::move(val));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; break; }
            return fail("ожидалась ',' или '}'");
        }
        out = std::move(obj);
        return true;
    }
};

} // anonymous namespace

bool JsonValue::parse(const std::string& text, JsonValue& out, std::string* error) {
    size_t consumed = 0;
    std::string err;
    if (!parse_prefix(text, consumed, out, &err)) {
        if (error) *error = err;
        return false;
    }
    /* Проверяем, что после значения — только пробелы. */
    while (consumed < text.size()) {
        char c = text[consumed];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            if (error) {
                *error = "лишние символы после JSON-значения (позиция "
                       + std::to_string(consumed) + ")";
            }
            return false;
        }
        ++consumed;
    }
    return true;
}

bool JsonValue::parse_prefix(const std::string& text, size_t& consumed,
                             JsonValue& out, std::string* error) {
    Parser p(text);
    p.skip_ws();
    JsonValue value;
    if (!p.parse_value(value, 0)) {
        if (error) *error = p.error();
        return false;
    }
    out = std::move(value);
    /* consumed — конец САМОГО значения: пробелы, съеденные парсером
     * при проверке закрывающей скобки, в него не входят. Иначе
     * блок, вырезанный эвристикой, «съел» бы первый пробел мусора. */
    consumed = p.pos();
    while (consumed > 0) {
        char c = text[consumed - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        --consumed;
    }
    return true;
}

/* ======================================================================
 * Тип и значение
 * ====================================================================== */

JsonValue::Type JsonValue::type() const { return impl_->type; }
bool JsonValue::is_null() const { return impl_->type == Type::Null; }
bool JsonValue::is_bool() const { return impl_->type == Type::Bool; }
bool JsonValue::is_int() const { return impl_->type == Type::Int; }
bool JsonValue::is_double() const { return impl_->type == Type::Double; }
bool JsonValue::is_number() const {
    return impl_->type == Type::Int || impl_->type == Type::Double;
}
bool JsonValue::is_string() const { return impl_->type == Type::String; }
bool JsonValue::is_array() const { return impl_->type == Type::Array; }
bool JsonValue::is_object() const { return impl_->type == Type::Object; }

bool JsonValue::as_bool(bool def) const {
    switch (impl_->type) {
        case Type::Bool: return impl_->boolean;
        /* "true"/"1" от локальных моделей — не повод ронять вызов. */
        case Type::Int:  return impl_->integer != 0;
        case Type::String: {
            std::string v;
            for (char c : impl_->str) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "true" || v == "1" || v == "yes") return true;
            if (v == "false" || v == "0" || v == "no") return false;
            return def;
        }
        default: return def;
    }
}

long long JsonValue::as_int(long long def) const {
    switch (impl_->type) {
        case Type::Int: return impl_->integer;
        case Type::Double: return static_cast<long long>(impl_->real);
        case Type::Bool: return impl_->boolean ? 1 : 0;
        case Type::String: {
            const std::string& s = impl_->str;
            if (s.empty()) return def;
            char* end = nullptr;
            long long v = std::strtoll(s.c_str(), &end, 10);
            if (end == s.c_str()) return def;
            return v;
        }
        default: return def;
    }
}

double JsonValue::as_double(double def) const {
    switch (impl_->type) {
        case Type::Int: return static_cast<double>(impl_->integer);
        case Type::Double: return impl_->real;
        case Type::Bool: return impl_->boolean ? 1.0 : 0.0;
        case Type::String: {
            const std::string& s = impl_->str;
            if (s.empty()) return def;
            char* end = nullptr;
            double v = std::strtod(s.c_str(), &end);
            if (end == s.c_str()) return def;
            return v;
        }
        default: return def;
    }
}

const std::string& JsonValue::as_string() const {
    return impl_->type == Type::String ? impl_->str : null_string();
}

std::string JsonValue::as_string_or(const std::string& def) const {
    return impl_->type == Type::String ? impl_->str : def;
}

const char* JsonValue::type_name() const {
    switch (impl_->type) {
        case Type::Null:   return "null";
        case Type::Bool:   return "boolean";
        case Type::Int:    return "integer";
        case Type::Double: return "number";
        case Type::String: return "string";
        case Type::Array:  return "array";
        case Type::Object: return "object";
    }
    return "unknown";
}

/* ======================================================================
 * Массив
 * ====================================================================== */

size_t JsonValue::size() const {
    if (impl_->type == Type::Array) return impl_->arr.size();
    if (impl_->type == Type::Object) return impl_->obj.size();
    return 0;
}

const JsonValue& JsonValue::at(size_t index) const {
    if (impl_->type != Type::Array || index >= impl_->arr.size()) return null_value();
    return impl_->arr[index];
}

void JsonValue::push_back(JsonValue value) {
    if (impl_->type != Type::Array) *impl_ = Impl(Type::Array);
    impl_->arr.push_back(std::move(value));
}

/* ======================================================================
 * Объект
 * ====================================================================== */

bool JsonValue::has(const std::string& key) const {
    return impl_->type == Type::Object && impl_->find_member(key) != nullptr;
}

const JsonValue& JsonValue::get(const std::string& key) const {
    if (impl_->type != Type::Object) return null_value();
    const JsonValue* v = impl_->find_member(key);
    return v ? *v : null_value();
}

void JsonValue::set(const std::string& key, JsonValue value) {
    if (impl_->type != Type::Object) *impl_ = *new Impl(Type::Object);
    for (auto& kv : impl_->obj) {
        if (kv.first == key) { kv.second = std::move(value); return; }
    }
    impl_->obj.emplace_back(key, std::move(value));
}

bool JsonValue::erase(const std::string& key) {
    if (impl_->type != Type::Object) return false;
    for (auto it = impl_->obj.begin(); it != impl_->obj.end(); ++it) {
        if (it->first == key) { impl_->obj.erase(it); return true; }
    }
    return false;
}

std::vector<std::string> JsonValue::keys() const {
    std::vector<std::string> out;
    if (impl_->type != Type::Object) return out;
    out.reserve(impl_->obj.size());
    for (const auto& kv : impl_->obj) out.push_back(kv.first);
    return out;
}

std::vector<std::pair<std::string, const JsonValue*>> JsonValue::entries() const {
    std::vector<std::pair<std::string, const JsonValue*>> out;
    if (impl_->type != Type::Object) return out;
    out.reserve(impl_->obj.size());
    for (const auto& kv : impl_->obj) out.emplace_back(kv.first, &kv.second);
    return out;
}

/* ======================================================================
 * Доступ по пути
 * ====================================================================== */

const JsonValue* JsonValue::find(const std::string& path) const {
    const JsonValue* cur = this;
    size_t i = 0;
    while (i < path.size() && cur) {
        char c = path[i];
        if (c == '.') { ++i; continue; }
        if (c == '[') {
            size_t close = path.find(']', i);
            if (close == std::string::npos) return nullptr;
            std::string idx = path.substr(i + 1, close - i - 1);
            if (idx.empty()) return nullptr;
            if (cur->type() != Type::Array) return nullptr;
            char* end = nullptr;
            long long n = std::strtoll(idx.c_str(), &end, 10);
            if (end == idx.c_str() || n < 0) return nullptr;
            /* Границу проверяем сами: at() за её пределами отдаёт
             * статический null, и find() вернул бы не nullptr. */
            if (cur->size() <= static_cast<size_t>(n)) return nullptr;
            cur = &cur->at(static_cast<size_t>(n));
            i = close + 1;
            continue;
        }
        size_t end = i;
        while (end < path.size() && path[end] != '.' && path[end] != '[') ++end;
        std::string key = path.substr(i, end - i);
        if (cur->type() != Type::Object) return nullptr;
        if (!cur->has(key)) return nullptr;
        cur = &cur->get(key);
        i = end;
    }
    return cur;
}

bool JsonValue::get_bool(const std::string& path, bool def) const {
    const JsonValue* v = find(path);
    return v ? v->as_bool(def) : def;
}

long long JsonValue::get_int(const std::string& path, long long def) const {
    const JsonValue* v = find(path);
    return v ? v->as_int(def) : def;
}

double JsonValue::get_double(const std::string& path, double def) const {
    const JsonValue* v = find(path);
    return v ? v->as_double(def) : def;
}

std::string JsonValue::get_string(const std::string& path,
                                  const std::string& def) const {
    const JsonValue* v = find(path);
    if (!v || v->type() != Type::String) return def;
    return v->as_string();
}

/* ======================================================================
 * Сериализация
 * ====================================================================== */

void JsonValue::dump_into(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    const std::string nl = pretty ? "\n" : "";
    const std::string pad = pretty ? std::string(static_cast<size_t>(indent * (depth + 1)), ' ') : "";
    const std::string pad_end = pretty ? std::string(static_cast<size_t>(indent * depth), ' ') : "";

    switch (impl_->type) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += impl_->boolean ? "true" : "false"; break;
        case Type::Int:    out += std::to_string(impl_->integer); break;
        case Type::Double: {
            if (!std::isfinite(impl_->real)) { out += "null"; break; }
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", impl_->real);
            out += buf;
            break;
        }
        case Type::String: out += '"'; out += json::escape(impl_->str); out += '"'; break;
        case Type::Array: {
            if (impl_->arr.empty()) { out += "[]"; break; }
            out += '[';
            out += nl;
            for (size_t i = 0; i < impl_->arr.size(); ++i) {
                out += pad;
                impl_->arr[i].dump_into(out, indent, depth + 1);
                if (i + 1 < impl_->arr.size()) out += ',';
                out += nl;
            }
            out += pad_end;
            out += ']';
            break;
        }
        case Type::Object: {
            if (impl_->obj.empty()) { out += "{}"; break; }
            out += '{';
            out += nl;
            for (size_t i = 0; i < impl_->obj.size(); ++i) {
                out += pad;
                out += '"';
                out += json::escape(impl_->obj[i].first);
                out += pretty ? "\": " : "\":";
                impl_->obj[i].second.dump_into(out, indent, depth + 1);
                if (i + 1 < impl_->obj.size()) out += ',';
                out += nl;
            }
            out += pad_end;
            out += '}';
            break;
        }
    }
}

std::string JsonValue::dump(int indent) const {
    std::string out;
    dump_into(out, indent, 0);
    return out;
}

} // namespace json
} // namespace coder

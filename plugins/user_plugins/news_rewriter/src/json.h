#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace news_rewriter {

// Минимальный JSON — значения, парсер и сериализатор без внешних зависимостей.
// Достаточно для конфигурации плагина и хранения статей.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json();
    Json(std::nullptr_t);
    Json(bool value);
    Json(int value);
    Json(int64_t value);
    Json(double value);
    Json(const char* value);
    Json(const std::string& value);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool fallback = false) const;
    int64_t as_int(int64_t fallback = 0) const;
    double as_double(double fallback = 0.0) const;
    const std::string& as_string() const;
    std::string as_string(const std::string& fallback) const;

    std::size_t size() const;
    bool empty() const;
    bool contains(const std::string& key) const;

    Json& operator[](const std::string& key);   // объект: доступ/создание
    Json& operator[](std::size_t index);        // массив: доступ (не растёт)
    const Json& operator[](std::size_t index) const; // массив: доступ или null
    const Json& get(const std::string& key) const; // объект: доступ или null
    Json& get(const std::string& key);             // объект: доступ или null

    const std::vector<std::string> keys() const;

    // Фабрики для построения.
    static Json object();
    static Json array();
    void push(const Json& value);

    // Парсинг/сериализация. parse: true при успехе (error заполняется).
    static Json parse(const std::string& text, bool* ok = nullptr,
                      std::string* error = nullptr);
    std::string dump() const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> array_;
    std::map<std::string, Json> object_;

    static Json null_value();
};

} // namespace news_rewriter

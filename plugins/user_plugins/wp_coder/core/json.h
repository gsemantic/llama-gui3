#pragma once

/*
 * json.h — JsonValue: собственный JSON-DOM плагина (И1.1, решение D-2).
 *
 * Раньше для JSON у плагина был только набор извлекателей
 * (core/json_utils.h: json::str, json::int_), которые умеют ровно одно —
 * вытащить верхнеуровневое строковое/числовое поле. Этого не хватает для
 * И1: JSON-схемы параметров инструмента, произвольных аргументов вызова
 * и объекта tool_output — все они требуют вложенности и массивов.
 *
 * Намеренно НЕ расширение json_utils.h: это полноценный DOM с парсером и
 * сериализатором. json_utils.h остаётся для мест, где нужен дешёвый
 * извлекатель без построения дерева (быстрый разбор ответа модели),
 * и для общих текстовых помощников (utf8/sanitize), которые использует
 * и сам JsonValue.
 *
 * Особенности, важные для агента:
 *   - доступ по пути "a.b[0].c" (find/get_string/get_int) — не нужно
 *     разворачивать дерево руками на каждом уровне;
 *   - parse_prefix() — разбирает ПЕРВОЕ значение во входе и говорит,
 *     сколько символов оно заняло. Эвристики извлечения вызова
 *     инструмента (core/tool_protocol.cpp) находят блок «примерно»,
 *     и полная строка после него может быть мусором;
 *   - ключи объекта хранятся в порядке вставки, поэтому сериализованная
 *     схема детерминирована (удобно для промпта и для тестов).
 */

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coder {
namespace json {

class JsonValue {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    /* --- Создание --- */
    JsonValue();                          // null
    JsonValue(std::nullptr_t);
    JsonValue(bool v);
    JsonValue(int v);
    JsonValue(long long v);
    JsonValue(double v);
    JsonValue(const char* v);
    JsonValue(std::string v);
    JsonValue(const JsonValue& other);
    JsonValue(JsonValue&& other) noexcept;
    JsonValue& operator=(const JsonValue& other);
    JsonValue& operator=(JsonValue&& other) noexcept;
    ~JsonValue();

    static JsonValue object();
    static JsonValue array();

    /* --- Разбор --- */

    /* Строгий разбор: весь вход (минус пробелы по краям) — одно значение.
     * При ошибке out не меняется. */
    static bool parse(const std::string& text, JsonValue& out,
                      std::string* error = nullptr);

    /* Разбор первого значения во входе. consumed — число съеденных
     * символов (пробелы до значения в счёт не входят). Нужен там, где
     * блок найден эвристикой и за ним может идти мусор. */
    static bool parse_prefix(const std::string& text, size_t& consumed,
                             JsonValue& out, std::string* error = nullptr);

    /* --- Тип и значение --- */
    Type type() const;
    bool is_null() const;
    bool is_bool() const;
    bool is_int() const;
    bool is_double() const;
    bool is_number() const;   // int или double
    bool is_string() const;
    bool is_array() const;
    bool is_object() const;

    /* Приведение с запасным значением: тип не тот → def, а не исключение.
     * Именно такой режим нужен аргументам инструментов — модель может
     * прислать "5" вместо 5, и это не повод ронять весь вызов. */
    bool as_bool(bool def = false) const;
    long long as_int(long long def = 0) const;
    double as_double(double def = 0.0) const;
    const std::string& as_string() const;
    std::string as_string_or(const std::string& def) const;

    /* Имя типа — для сообщений об ошибках валидации. */
    const char* type_name() const;

    /* --- Массив --- */
    size_t size() const;
    const JsonValue& at(size_t index) const;
    void push_back(JsonValue value);

    /* --- Объект --- */
    bool has(const std::string& key) const;
    const JsonValue& get(const std::string& key) const;
    void set(const std::string& key, JsonValue value);
    bool erase(const std::string& key);
    std::vector<std::string> keys() const;
    /* Ключ + значение в порядке вставки. */
    std::vector<std::pair<std::string, const JsonValue*>> entries() const;

    /* --- Доступ по пути "a.b[0].c" --- */
    const JsonValue* find(const std::string& path) const;
    bool get_bool(const std::string& path, bool def = false) const;
    long long get_int(const std::string& path, long long def = 0) const;
    double get_double(const std::string& path, double def = 0.0) const;
    std::string get_string(const std::string& path,
                           const std::string& def = "") const;

    /* --- Сериализация --- */
    /* indent < 0 — компактно, иначе — pretty-print с этим отступом. */
    std::string dump(int indent = -1) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit JsonValue(Type t);   // только для object() / array()

    void dump_into(std::string& out, int indent, int depth) const;
};

} // namespace json
} // namespace coder

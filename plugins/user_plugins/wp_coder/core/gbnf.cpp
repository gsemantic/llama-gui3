/*
 * gbnf.cpp — генерация GBNF из JSON-схемы (И12.7).
 * Обоснование — в gbnf.h; здесь только код.
 */

#include "gbnf.h"

namespace coder {
namespace {

/* Правила, не зависящие от схемы. Имена зафиксированы: на них ссылаются
 * сгенерированные правила, и переименование молча сломало бы все
 * грамматики — а заметить это можно только на живой модели. */
const char* const kNumberRule =
    "number  ::= \"-\"? (\"0\" | [1-9] [0-9]*) (\".\" [0-9]+)? "
    "([eE] [-+]? [0-9]+)?\n";
/* Порядок правил — не косметика.
 *
 * GBNF допускает рекурсию, то есть циклы ссылок неизбежны (`value` →
 * `array` → `value`), и значит требования «правило определено раньше
 * первого упоминания» здесь не выводится. Но БАЗОВЫЕ правила циклов не
 * имеют, и их порядок можно сделать таким, что каждое ссылается только
 * назад: `char` до `string`, `ws` в самое начало, `value` после всех
 * примитивов. Дешёвая страховка на случай, если разбор на стороне сервера
 * окажется последовательным. Проверить это без живой модели нельзя, и граница
 * названа в §4 плана (И12.7). */
const char* const kObjectRules =
    "ws      ::= [ \\t\\n]*\n"
    "char    ::= [^\"\\\\\\x7F\\x00-\\x1F] | \"\\\\\" ([\"\\\\bfnrt] | \"u\" "
    "[0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F])\n"
    "string  ::= \"\\\"\" char* \"\\\"\"\n"
    "integer ::= \"-\"? (\"0\" | [1-9] [0-9]*)\n"
    "boolean ::= \"true\" | \"false\"\n"
    "null    ::= \"null\"\n"
    "pair    ::= string ws \":\" ws value\n"
    "array   ::= \"[\" ws (value (ws \",\" ws value)*)? ws \"]\"\n"
    "object  ::= \"{\" ws (pair (ws \",\" ws pair)*)? ws \"}\"\n"
    "value   ::= object | array | string | number | boolean | null\n";

/* Литерал GBNF для готовой строки — с кавычками вокруг.
 *
 * Единая точка экранирования. Соблазн написать ключ руками в каждом из двух
 * мест — это как раз то, что в первой версии и сломалось: значение
 * JSON-ключа в грамматике должно быть строкой В КАВЫЧКАХ, а кавычки внутри
 * GBNF тоже требуют экранирования. Одна функция делает это один раз. */
std::string lit(const std::string& already_quoted) {
    std::string out = "\"";
    for (char c : already_quoted) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

/* JSON-ключ или JSON-строка как токен грамматики. */
std::string lit_json_string(const std::string& raw) {
    return lit("\"" + raw + "\"");
}

/* Сборщик правил. Имена генерируемых правил уникальны по счётчику: путь из
 * ключей мог бы повториться только при повторяющемся ключе в одном объекте,
 * а такого в JSON не бывает — но счётчик дешевле доказательства. */
class Builder {
public:
    /* Имя правила для значения по схеме: либо общее примитивное, либо
     * сгенерированное. */
    std::string value_ref(const json::JsonValue& schema, const std::string& path);

    /* `root` — ПОСЛЕДНИМ и отдельно от сгенерированных правил: он ссылается
     * на них, а правило, определённое позже первого упоминания, в
     * последовательном разборе не найдётся. */
    std::string finish(const std::string& root_ref) const {
        return out_ + kNumberRule + kObjectRules + "root   ::= " + root_ref + "\n";
    }

private:
    void emit(const std::string& name, const std::string& body) {
        out_ += name + " ::= " + body + "\n";
    }
    std::string unique(const std::string& base) {
        return base + "_" + std::to_string(counter_++);
    }

    std::string out_;
    int counter_ = 0;
};

std::string type_of(const json::JsonValue& schema) {
    if (!schema.is_object()) return std::string();
    return schema.get_string("type", "");
}

} // namespace

std::string Builder::value_ref(const json::JsonValue& schema,
                               const std::string& path) {
    /* enum для строк — самый полезный случай в схемах инструментов
     * (`grep_search` с режимом, `list` с типом сортировки). Он же самый
     * дешёвый: выборка из конечного списка запрещает опечатку в ЗНАЧЕНИИ,
     * а не только в синтаксисе. */
    if (schema.is_object() && schema.has("enum")) {
        const json::JsonValue& e = schema.get("enum");
        if (e.is_array()) {
            std::string body;
            for (size_t i = 0; i < e.size(); ++i) {
                const json::JsonValue& v = e.at(i);
                /* Не-строковые значения enum в грамматику не ведём: приведя
                 * их к строке, мы разрешили бы грамматикой то, чего в схеме
                 * нет. */
                if (!v.is_string()) continue;
                if (!body.empty()) body += " | ";
                body += lit_json_string(v.as_string());
            }
            if (!body.empty()) {
                const std::string name = unique("prop" + path);
                emit(name, body);
                return name;
            }
        }
    }

    const std::string type = type_of(schema);
    if (type == "string") return "string";
    if (type == "integer") return "integer";
    if (type == "number") return "number";
    if (type == "boolean") return "boolean";
    if (type == "null") return "null";

    if (type == "array") {
        std::string item = "value";
        if (schema.is_object() && schema.has("items")) {
            item = value_ref(schema.get("items"), path + "_i");
        }
        const std::string name = unique("prop" + path);
        emit(name, "\"[\" ws (" + item + " (ws \",\" ws " + item +
                    ")*)? ws \"]\"");
        return name;
    }

    /* Объект распознаём и по `type`, и по одному наличию `properties`:
     * схема инструмента — это объект со `properties` и часто без `type`. */
    if (type == "object" || (schema.is_object() && schema.has("properties"))) {
        /* Ключи разрешены в ЛЮБОМ порядке и в любом подмножестве. Порядок в
         * JSON не значит ничего, а принуждение к нему ограничивало бы
         * модель ради ничего. `required` в грамматику НЕ переносится:
         * иначе модель, не желающая писать необязательный ключ, была бы
         * вынуждена его писать. */
        /* Перебираются ИМЕНА СВОЙСТВ ОБЪЕКТА-ЭКЗЕМПЛЯРА, а не ключи самой
         * схемы. Это главная ошибка, которая здесь была: схема инструмента
         * — это `{"type":"object","properties":{…}}`, и перебор её собственных
         * ключей дал грамматику для ДОКУМЕНТА схемы (то есть для объекта с
         * ключами «type» и «properties»), а не для того, что модель должна
         * сгенерировать. На схеме без `properties` (свойство-объект,
         * записанное как `{"type":"object"}`) перебирается сама схема. */
        const json::JsonValue* props = nullptr;
        const json::JsonValue& nested = schema.get("properties");
        if (schema.is_object() && nested.is_object()) props = &nested;
        const json::JsonValue& holder = props ? *props : schema;
        std::string alt;
        if (holder.is_object()) {
            for (const auto& entry : holder.entries()) {
                const json::JsonValue* sub = entry.second;
                if (!sub) continue;
                const std::string val = value_ref(*sub, path + "_" + entry.first);
                if (!alt.empty()) alt += " | ";
                alt += lit_json_string(entry.first) + " ws \":\" ws " + val;
            }
        }
        const std::string name = unique("prop" + path);
        if (alt.empty()) {
            /* Пустой объект: `{}` и только. Общий `object` разрешил бы
             * любые ключи, то есть грамматика разрешила бы то, чего нет в
             * схеме. */
            emit(name, "\"{\" ws \"}\"");
        } else {
            emit(name, "\"{\" ws (" + alt + ") (ws \",\" ws (" + alt +
                        "))? ws \"}\"");
        }
        return name;
    }

    /* Схема без типа или с незнакомым: общий `value`. Ограничение
     * ослабляется, но не становится неверным. */
    return "value";
}

std::string gbnf_from_schema(const json::JsonValue& schema) {
    Builder b;
    const std::string ref = b.value_ref(schema, "");
    return b.finish(ref);
}

std::string gbnf_for_tool_call(const std::vector<std::string>& names) {
    /* Пустой список — ПУСТАЯ СТРОКА, а не грамматика с пустой альтернативой.
     *
     * Первая версия подставляла `()` — а это синтаксическая ошибка GBNF, и
     * ошибка молчаливая: сервер принял бы правило и вёл себя непредсказуемо.
     * Пустая строка означает «чем ограничивать нечего», и вызывающий решает
     * сам; «инструментов нет» и так означает, что каталог пуст и модель о
     * том предупреждена. */
    if (names.empty()) return std::string();

    std::string tools;
    for (const std::string& n : names) {
        if (!tools.empty()) tools += " | ";
        tools += lit_json_string(n);
    }
    const std::string root_ref =
        "\"{\" ws " + lit_json_string("tool") + " ws \":\" ws (" + tools +
        ") ws \",\" ws " + lit_json_string("arguments") +
        " ws \":\" ws object ws \"}\"";
    Builder b;
    return b.finish(root_ref);
}

} // namespace coder
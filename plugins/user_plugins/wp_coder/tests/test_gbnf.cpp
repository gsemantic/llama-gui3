/*
 * test_gbnf.cpp — И12.7: генерация GBNF-грамматики вызова инструмента.
 *
 * ЧТО ПРОВЕРЯЕТСЯ И ГДЕ ГРАНИЦА
 * ------------------------------
 * Проверяется ГРАММАТИКА КАК СТРОКА: правила, имена, ссылки, порядок. Не
 * проверяется её ИСПОЛНЕНИЕ ограниченной выборкой — для этого нужна живая
 * llama.cpp, которой под рукой нет. Граница названа явно, потому что
 * «проверка грамматики» без слов о том, что именно проверяется, читается
 * как «грамматика работает», а это было бы неправдой.
 *
 * ПОЧЕМУ ПРОВЕРЯЕТСЯ НАСТОЯЩИЕ СХЕМЫ ИНСТРУМЕНТОВ, А НЕ ВЫДУМАННЫЕ.
 * Синтетическая схема в три строки проверяет генератор на том, чего в
 * плагине нет. Настоящие `ToolDef::parameters` проходят по всем пятидесяти
 * инструментам и показывают то, что ломается на самом деле: ключи без
 * `type`, вложенные объекты, `enum` — и то, что схема инструмента это
 * `{type:"object", properties:{…}}`, а не набор свойств верхнего уровня.
 *
 * ГЛАВНАЯ ОШИБКА, КОТОРУЮ ЭТОТ ФАЙЛ ЛОВИТ (уже была). Первая версия в
 * ветке «объект» перебирала СОБСТВЕННЫЕ ключи схемы, а не `properties`. Для
 * `{type:"object", properties:{path,k}}` это давало грамматику для объекта с
 * ключами «type» и «properties» — то есть для документа схемы, а не для того,
 * что модель должна сгенерировать. Проверка «в грамматике есть ключ path»
 * ловит это сразу; проверка «правила непустые» — никогда.
 */

#include "test_framework.h"
#include "../core/base_tools.h"
#include "../core/gbnf.h"
#include "../core/tools_registry.h"

#include <iostream>
#include <string>
#include <vector>

using namespace coder;

namespace {

void register_all_tools() {
    static bool done = false;
    if (done) return;
    done = true;
    register_base_tools();
}

json::JsonValue str_schema() {
    json::JsonValue o = json::JsonValue::object();
    o.set("type", std::string("string"));
    return o;
}

/* Разбиение на строки — и для перебора правил, и для проверки порядка их
 * объявления. Объявлено ПЕРВЫМ: макрос TEST раскрывается в функцию, и
 * помощник, стоящий ниже первого его использования, компилируется только
 * потому, что сегодня повезло с порядком. */
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); continue; }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/* Имена объявленных правил, БЕЗ ПРОБЕЛОВ.
 *
 * Имя выровнено по столбцу («root   ::=»), и подстрока вида `name + " ::="`
 * его не находит: после имени не один пробел, а три. Первая версия этой
 * проверки искала именно так и объявляла несуществующим правило `value`,
 * которое объявлено и всегда было. Сравнение по выровненному пробелами
 * столбцу — это сравнение по вёрстке, а не по содержанию. */
std::vector<std::string> rule_names(const std::string& gbnf) {
    std::vector<std::string> names;
    for (const std::string& line : split_lines(gbnf)) {
        const size_t sep = line.find("::=");
        if (sep == std::string::npos) continue;
        std::string name = line.substr(0, sep);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

/* Текст ОДНОГО правила целиком.
 *
 * Появился после находки: `)? ws "}"` встречается и в сгенерированном
 * правиле объекта, и в ПОСТОЯННОМ блоке (общее `object`), поэтому поиск по
 * всей грамматике проверял не то правило, которое проверялся. Утверждение
 * «объект разрешает несколько пар» обязано смотреть в правило, отвечающее за
 * пары, а не в любую строку файла. */
std::string rule_text(const std::string& gbnf, const std::string& name) {
    for (const std::string& line : split_lines(gbnf)) {
        const size_t sep = line.find("::=");
        if (sep == std::string::npos) continue;
        std::string n = line.substr(0, sep);
        while (!n.empty() && n.back() == ' ') n.pop_back();
        if (n == name) return line;
    }
    return std::string();
}

bool declares(const std::string& gbnf, const std::string& name) {
    for (const std::string& n : rule_names(gbnf)) {
        if (n == name) return true;
    }
    return false;
}

} // anonymous namespace

TEST(схема_инструмента_даёт_правила_а_не_документ_схемы) {
    /* Ловит первую ошибку генератора: `{type:"object", properties:{…}}`
     * должен давать грамматику для ЭКЗЕМПЛЯРА, а не для самой схемы. */
    json::JsonValue props = json::JsonValue::object();
    props.set("path", str_schema());
    props.set("k", str_schema());
    json::JsonValue schema = json::JsonValue::object();
    schema.set("type", std::string("object"));
    schema.set("properties", props);

    const std::string g = gbnf_from_schema(schema);
    ASSERT_TRUE(g.find("\\\"path\\\"") != std::string::npos);
    ASSERT_TRUE(g.find("\\\"k\\\"") != std::string::npos);
    /* Ключей САМОЙ схемы в грамматике быть не должно: модель не пишет
     * «type» и «properties», она пишет значения аргументов. */
    ASSERT_TRUE(g.find("\\\"properties\\\"") == std::string::npos);
}

TEST(обязательные_и_необязательные_ключи_равноправны_в_грамматике) {
    /* `required` в грамматику НЕ переносится: иначе модель, не желающая
     * писать необязательный ключ, была бы вынуждена его писать, а
     * ограничение работало бы против модели, а не за неё. Ключи разрешены
     * в любом порядке и в любом подмножестве. */
    json::JsonValue props = json::JsonValue::object();
    props.set("path", str_schema());
    props.set("note", str_schema());
    json::JsonValue schema = json::JsonValue::object();
    schema.set("type", std::string("object"));
    schema.set("properties", props);
    json::JsonValue required = json::JsonValue::array();
    required.push_back(json::JsonValue(std::string("path")));
    schema.set("required", required);

    const std::string g = gbnf_from_schema(schema);
    /* Повтор пары ключей — это «один или несколько в любом порядке»;
     * без него грамматика разрешала бы РОВНО ОДИН ключ. */
    const size_t first = g.find("\\\"path\\\"");
    ASSERT_TRUE(first != std::string::npos);
    ASSERT_TRUE(g.find("\\\"path\\\"", first + 1) != std::string::npos);
    /* И после ключей запятая необязательна — иначе объект из одного поля
     * был бы незаписываем.
     *
     * Смотрим ТЕКСТ СГЕНЕРИРОВАННОГО правила, а не ищем по грамматике:
     * та же строка `)? ws "}"` есть в постоянном блоке (общее `object`),
     * и поиск по всему файлу проходил бы при сгенерированном правиле без
     * `?` — то есть проверка подтверждала бы чужое правило. Эта находка
     * пришла мутацией: снятие `?` прошло зелёным. */
    std::string generated;
    for (const std::string& n : rule_names(g)) {
        if (n.rfind("prop", 0) == 0) { generated = rule_text(g, n); break; }
    }
    ASSERT_TRUE(!generated.empty());
    ASSERT_TRUE(generated.find("(ws \",\" ws (") != std::string::npos);
    ASSERT_TRUE(generated.find(")? ws \"}\"") != std::string::npos);
}

TEST(типы_аргументов_попадают_в_правила) {
    json::JsonValue props = json::JsonValue::object();
    props.set("s", str_schema());
    json::JsonValue i = json::JsonValue::object();
    i.set("type", std::string("integer"));
    props.set("i", i);
    json::JsonValue b = json::JsonValue::object();
    b.set("type", std::string("boolean"));
    props.set("b", b);
    json::JsonValue arr = json::JsonValue::object();
    arr.set("type", std::string("array"));
    json::JsonValue items = str_schema();
    arr.set("items", items);
    props.set("a", arr);
    json::JsonValue schema = json::JsonValue::object();
    schema.set("type", std::string("object"));
    schema.set("properties", props);

    const std::string g = gbnf_from_schema(schema);
    ASSERT_TRUE(g.find(" integer") != std::string::npos);
    ASSERT_TRUE(g.find(" boolean") != std::string::npos);
    ASSERT_TRUE(g.find("ws \":\" ws string) (ws \",\" ws (") !=
                std::string::npos ||
                g.find("ws \":\" ws boolean") != std::string::npos);
    /* Массив строк: элемент ограничен строкой, а не любым значением. */
    ASSERT_TRUE(g.find("\"[\" ws (string (ws \",\" ws string)*)? ws \"]\"")
                != std::string::npos);
}

TEST(enum_ограничивает_значение_а_не_произвольную_строку) {
    /* Самая дешёвая и самая полезная часть: конечный список запрещает
     * опечатку в ЗНАЧЕНИИ, а не только в синтаксисе. */
    json::JsonValue en = json::JsonValue::array();
    en.push_back(json::JsonValue(std::string("name")));
    en.push_back(json::JsonValue(std::string("content")));
    json::JsonValue mode = json::JsonValue::object();
    mode.set("type", std::string("string"));
    mode.set("enum", en);

    const std::string g = gbnf_from_schema(mode);
    ASSERT_TRUE(g.find("\\\"name\\\"") != std::string::npos);
    ASSERT_TRUE(g.find("\\\"content\\\"") != std::string::npos);
    /* И никакой свободной строки: если бы правило ссылалось на `string`,
     * перечисление было бы размыто в ничто. */
    bool references_string = false;
    for (const std::string& line : split_lines(g)) {
        if (line.find("prop0 ::=") == std::string::npos) continue;
        references_string = line.find(" string") != std::string::npos;
    }
    ASSERT_FALSE(references_string);
}

TEST(вызов_инструмента_разрешает_только_перечисленные_имена) {
    const std::string g = gbnf_for_tool_call({"read_file", "bash"});
    ASSERT_TRUE(g.find("\\\"tool\\\"") != std::string::npos);
    ASSERT_TRUE(g.find("\\\"arguments\\\"") != std::string::npos);
    ASSERT_TRUE(g.find("\\\"read_file\\\"") != std::string::npos);
    ASSERT_TRUE(g.find("\\\"bash\\\"") != std::string::npos);
    /* Инструмент, которого не перечислили, в грамматике отсутствует. */
    ASSERT_TRUE(g.find("write_file") == std::string::npos);
}

TEST(пустой_список_инструментов_даёт_пустую_строку_а_не_пустую_альтернативу) {
    /* Первая версия подставляла `()` — это синтаксическая ошибка GBNF, и
     * ошибка МОЛЧАЛИВАЯ: сервер принял бы правило и вёл себя
     * непредсказуемо. Пустая строка означает «чем ограничивать нечего». */
    ASSERT_EQ(gbnf_for_tool_call({}), std::string());
}

TEST(правила_объявлены_до_того_как_на_них_ссылаются) {
    /* `root` идёт последним и ссылается на сгенерированные правила: правило,
     * определённое позже первого упоминания, в последовательном разборе не
     * нашлось бы. Рекурсия (value → array → value) неизбежна, но БАЗОВЫЕ
     * правила циклов не имеют, и их порядок проверяем. */
    const std::string g = gbnf_from_schema(
        json::JsonValue::object());   /* пустая схема — базовый набор */
    const std::vector<std::string> defined = rule_names(g);
    ASSERT_TRUE(defined.size() >= 8);
    /* root — последний. */
    ASSERT_EQ(defined.back(), std::string("root"));
    /* char раньше string: иначе `string` ссылался бы на ещё не объявленный
     * `char`. */
    size_t char_at = defined.size();
    size_t string_at = defined.size();
    for (size_t i = 0; i < defined.size(); ++i) {
        if (defined[i] == "char") char_at = i;
        if (defined[i] == "string") string_at = i;
    }
    ASSERT_TRUE(char_at < string_at);
    /* `pair` ссылается на `string`, значит `string` обязан быть объявлен
     * раньше: это единственная проверка «ссылка назад» среди базовых
     * правил, и она единственная, которая не про цикл. */
    size_t pair_at = defined.size();
    for (size_t i = 0; i < defined.size(); ++i) {
        if (defined[i] == "pair") pair_at = i;
    }
    ASSERT_TRUE(pair_at != defined.size());
    ASSERT_TRUE(string_at < pair_at);
}

TEST(все_схемы_инструментов_превращаются_в_грамматику_без_мусора) {
    /* Настоящие инструменты вместо выдуманных: генератор, который работает
     * на синтетике, может сломаться на `enum` из шести значений или на
     * вложенном объекте. Проверяется, а не «длина больше нуля»: грамматика
     * без единого правила прошла бы половину этих проверок. */
    register_all_tools();
    size_t checked = 0;
    for (const ToolDef& def : ToolsRegistry::instance().defs()) {
        if (def.name.empty() || !def.parameters.is_object()) continue;
        ++checked;
        const std::string g = gbnf_from_schema(def.parameters);
        if (!declares(g, "root")) {
            std::cerr << "  " << def.name
                      << ": грамматика без корневого правила" << std::endl;
            ASSERT_TRUE(false);
        }
        if (g.find("()") != std::string::npos) {
            std::cerr << "  " << def.name
                      << ": пустая альтернатива — синтаксическая ошибка"
                      << std::endl;
            ASSERT_TRUE(false);
        }
        /* Строка обязана быть СПОСОБНА повторяться: `char*`, а не `char`.
         * Односимвольная строка — это грамматика, которая запрещает модели
         * написать «src/main.cpp», то есть ограничение, запрещающее
         * полезное. */
        if (rule_text(g, "string").find("char*") == std::string::npos) {
            std::cerr << "  " << def.name
                      << ": строка не может повторяться (char без звёздочки)"
                      << std::endl;
            ASSERT_TRUE(false);
        }
        /* Пустой объект — это ровно `{}`: общее `object` разрешило бы любые
         * ключи, то есть грамматика разрешила бы то, чего в схеме нет. */
        {
            json::JsonValue empty_props = json::JsonValue::object();
            json::JsonValue empty_schema = json::JsonValue::object();
            empty_schema.set("type", std::string("object"));
            empty_schema.set("properties", empty_props);
            const std::string eg = gbnf_from_schema(empty_schema);
            std::string rule;
            for (const std::string& n : rule_names(eg)) {
                if (n.rfind("prop", 0) == 0) { rule = rule_text(eg, n); break; }
            }
            if (rule.empty() || rule.find("\"}\"") == std::string::npos) {
                std::cerr << "  пустой объект схемы разрешает не только {}"
                          << std::endl;
                ASSERT_TRUE(false);
            }
        }
        /* Каждое имя правила, на которое ссылаются, объявлено ли? Список
         * ссылок собирается грубо, но ловит главное: правило, которого
         * нет, а на которое ссылаются, — это ошибка, которая проявилась бы
         * только на живой модели. */
        for (const std::string& name : {"value", "string", "integer",
                                        "boolean", "null", "object",
                                        "array", "pair", "ws", "char",
                                        "number"}) {
            if (!declares(g, name)) {
                std::cerr << "  " << def.name << ": правило " << name
                          << " упомянуто, но не объявлено" << std::endl;
                ASSERT_TRUE(false);
            }
        }
    }
    /* Схемы реально были — иначе проверка была бы зелёной на пустом
     * переборе. Порог низкий сознательно: здесь регистрируются только
     * базовые инструменты (модульные меняют счётчик и тянут за собой
     * ModuleRegistry, см. замечание в test_tool.cpp), и задача проверки —
     * «схемы настоящие», а не «схем все пятьдесят семь». */
    ASSERT_TRUE(checked >= 20);
}
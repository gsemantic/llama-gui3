/*
 * test_json.cpp — И1.1: собственный JsonValue (решение D-2).
 *
 * Проверяем не «счастливый путь», а то, на чём реально ломается агент:
 * мусор после значения, сурогатные пары, битый UTF-8, приведение типов
 * от локальных моделей ("5" вместо 5) и доступ по пути.
 */

#include "test_framework.h"
#include "../core/json.h"
#include "../core/json_utils.h"

using namespace coder;

TEST(json_parses_scalars) {
    json::JsonValue v;
    ASSERT_TRUE(json::JsonValue::parse("42", v));
    ASSERT_TRUE(v.is_int());
    ASSERT_EQ(v.as_int(), 42LL);

    ASSERT_TRUE(json::JsonValue::parse("-7.5", v));
    ASSERT_TRUE(v.is_double());
    ASSERT_TRUE(v.as_double() < -7.4 && v.as_double() > -7.6);

    ASSERT_TRUE(json::JsonValue::parse("true", v));
    ASSERT_TRUE(v.as_bool());
    ASSERT_TRUE(json::JsonValue::parse("null", v));
    ASSERT_TRUE(v.is_null());
    ASSERT_TRUE(json::JsonValue::parse("\"текст\"", v));
    ASSERT_EQ(v.as_string(), std::string("текст"));
}

TEST(json_rejects_garbage_and_trailing_text) {
    json::JsonValue v;
    std::string err;
    ASSERT_FALSE(json::JsonValue::parse("{\"a\":}", v, &err));
    ASSERT_TRUE(!err.empty());
    /* Строгий разбор: хвост после значения — ошибка. */
    ASSERT_FALSE(json::JsonValue::parse("{\"a\":1} мусор", v));
    ASSERT_TRUE(json::JsonValue::parse("  {\"a\":1}  \n", v));
    ASSERT_EQ(v.get_int("a"), 1LL);
    /* Незакрытая скобка. */
    ASSERT_FALSE(json::JsonValue::parse("{\"a\":1", v));
    /* Пустой ввод. */
    ASSERT_FALSE(json::JsonValue::parse("", v));
}

TEST(json_parse_prefix_ignores_tail) {
    /* Так выглядит блок, вырезанный эвристикой extract_action:
     * валидный JSON и мусор после него. */
    std::string block = "{\"tool\":\"read_file\"} осталось мусора";
    json::JsonValue v;
    size_t consumed = 0;
    ASSERT_TRUE(json::JsonValue::parse_prefix(block, consumed, v));
    ASSERT_EQ(consumed, (size_t)20);
    ASSERT_EQ(v.get_string("tool"), std::string("read_file"));
}

TEST(json_nested_object_and_array) {
    json::JsonValue v;
    ASSERT_TRUE(json::JsonValue::parse(
        "{\"a\":{\"b\":[10,20,{\"c\":\"deep\"}]}}", v));
    ASSERT_TRUE(v.is_object());
    ASSERT_EQ(v.get_int("a.b[0]"), 10LL);
    ASSERT_EQ(v.get_int("a.b[1]"), 20LL);
    ASSERT_EQ(v.get_string("a.b[2].c"), std::string("deep"));
    /* Выход за границы и неверный путь — nullptr, а не исключение. */
    ASSERT_TRUE(v.find("a.b[9]") == nullptr);
    ASSERT_TRUE(v.find("a.zzz") == nullptr);
    ASSERT_TRUE(v.find("a.b[2].c.d") == nullptr);
    ASSERT_TRUE(v.find("a.b[-1]") == nullptr);
    /* Ветка не того типа. */
    ASSERT_TRUE(v.find("a.b[0].c") == nullptr);
}

TEST(json_string_escapes_and_surrogates) {
    json::JsonValue v;
    ASSERT_TRUE(json::JsonValue::parse("\"a\\nb\\t\\\"c\\\\d\\/e\"", v));
    ASSERT_EQ(v.as_string(), std::string("a\nb\t\"c\\d/e"));
    /* Сурогатная пара \uD83D\uDE00 = U+1F600 */
    ASSERT_TRUE(json::JsonValue::parse("\"\\uD83D\\uDE00\"", v));
    ASSERT_EQ(v.as_string(), std::string("\xF0\x9F\x98\x80"));
    /* Одиночный суррогат заменяется на U+FFFD, а не ломает UTF-8. */
    ASSERT_TRUE(json::JsonValue::parse("\"\\uD83D\"", v));
    ASSERT_TRUE(text::is_valid_utf8(v.as_string()));
}

TEST(json_set_get_erase_keeps_insertion_order) {
    json::JsonValue obj = json::JsonValue::object();
    obj.set("tool", "read_file");
    obj.set("path", "a.txt");
    obj.set("k", 5);
    /* Повторная установка не добавляет дубль, а заменяет по месту. */
    obj.set("path", "b.txt");
    ASSERT_EQ(obj.dump(), std::string("{\"tool\":\"read_file\",\"path\":\"b.txt\",\"k\":5}"));
    ASSERT_EQ(obj.size(), (size_t)3);
    ASSERT_TRUE(obj.has("path"));
    ASSERT_EQ(obj.get_string("path"), std::string("b.txt"));
    ASSERT_TRUE(obj.erase("k"));
    ASSERT_FALSE(obj.erase("k"));
    ASSERT_FALSE(obj.has("k"));
    /* Отсутствующий ключ — null, а не исключение. */
    ASSERT_TRUE(obj.get("k").is_null());
}

TEST(json_array_build_and_dump) {
    json::JsonValue arr = json::JsonValue::array();
    arr.push_back(1);
    arr.push_back("два");
    arr.push_back(json::JsonValue(true));
    ASSERT_EQ(arr.size(), (size_t)3);
    ASSERT_EQ(arr.at(1).as_string(), std::string("два"));
    ASSERT_TRUE(arr.at(7).is_null());
    ASSERT_EQ(arr.dump(), std::string("[1,\"два\",true]"));
}

TEST(json_type_coercion_for_models) {
    /* Локальные модели регулярно присылают "5" вместо 5 и "true"
     * вместо true. Это не повод отклонять вызов инструмента. */
    json::JsonValue v;
    ASSERT_TRUE(json::JsonValue::parse("{\"k\":\"5\",\"b\":\"true\"}", v));
    ASSERT_EQ(v.get_int("k", 0), 5LL);
    ASSERT_TRUE(v.get_bool("b", false));
    ASSERT_TRUE(json::JsonValue::parse("{\"k\":5}", v));
    ASSERT_EQ(v.get_int("k", 0), 5LL);
    /* Мусор — запасное значение. */
    ASSERT_TRUE(json::JsonValue::parse("{\"k\":\"abc\"}", v));
    ASSERT_EQ(v.get_int("k", -1), -1LL);
    /* Отсутствующее поле — тоже запасное значение. */
    ASSERT_EQ(v.get_int("missing", 7), 7LL);
    ASSERT_EQ(v.get_string("missing", "нет"), std::string("нет"));
}

TEST(json_double_that_is_whole_stays_integer) {
    /* Иначе "5" и 5 сериализуются по-разному и валидация схемы
     * посчитает тип неверным. */
    json::JsonValue v(5.0);
    ASSERT_TRUE(v.is_int());
    ASSERT_EQ(v.dump(), std::string("5"));
    json::JsonValue w(5.5);
    ASSERT_TRUE(w.is_double());
    ASSERT_EQ(w.dump(), std::string("5.5"));
}

TEST(json_dump_escapes_and_repairs_utf8) {
    json::JsonValue v("a\"b\\c\nd\te");
    ASSERT_EQ(v.dump(), std::string("\"a\\\"b\\\\c\\nd\\te\""));
    /* Битый UTF-8 в данных не должен попадать в сериализацию:
     * значение чинится, round-trip остаётся валидным. */
    std::string broken = "ok";
    broken.push_back(static_cast<char>(0xE2));
    json::JsonValue b(broken);
    std::string dumped = b.dump();
    json::JsonValue back;
    ASSERT_TRUE(json::JsonValue::parse(dumped, back));
    ASSERT_TRUE(text::is_valid_utf8(back.as_string()));
}

TEST(json_dump_pretty_and_empty_containers) {
    json::JsonValue obj = json::JsonValue::object();
    obj.set("a", 1);
    obj.set("b", json::JsonValue::array());
    ASSERT_EQ(obj.dump(2), std::string("{\n  \"a\": 1,\n  \"b\": []\n}"));
    json::JsonValue empty = json::JsonValue::object();
    ASSERT_EQ(empty.dump(), std::string("{}"));
    ASSERT_EQ(json::JsonValue::array().dump(), std::string("[]"));
    ASSERT_EQ(json::JsonValue().dump(), std::string("null"));
}

TEST(json_entries_and_keys_in_order) {
    json::JsonValue obj = json::JsonValue::object();
    obj.set("z", 1);
    obj.set("a", 2);
    auto keys = obj.keys();
    ASSERT_EQ(keys.size(), (size_t)2);
    ASSERT_EQ(keys[0], std::string("z"));
    ASSERT_EQ(keys[1], std::string("a"));
    auto entries = obj.entries();
    ASSERT_EQ(entries[1].first, std::string("a"));
    ASSERT_EQ(entries[1].second->as_int(), 2LL);
}

TEST(json_deep_nesting_is_rejected_not_crashed) {
    /* Модель способна сгенерировать «забытую» скобку и тысячи
     * вложенных массивов: рекурсивный разбор такого роняет стек. */
    std::string deep;
    for (int i = 0; i < 500; ++i) deep += "[";
    deep += "1";
    for (int i = 0; i < 500; ++i) deep += "]";
    json::JsonValue v;
    ASSERT_FALSE(json::JsonValue::parse(deep, v));
}

TEST(json_value_copies_are_deep) {
    json::JsonValue original = json::JsonValue::object();
    original.set("a", 1);
    json::JsonValue copy = original;
    copy.set("a", 2);
    copy.set("b", 3);
    ASSERT_EQ(original.get_int("a"), 1LL);
    ASSERT_FALSE(original.has("b"));
    /* Перемещение не оставляет value в неопределённом состоянии. */
    json::JsonValue moved = std::move(copy);
    ASSERT_EQ(moved.get_int("a"), 2LL);
    ASSERT_TRUE(moved.has("b"));
}

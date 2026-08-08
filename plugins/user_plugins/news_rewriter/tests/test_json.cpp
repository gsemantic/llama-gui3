#include "test_framework.h"

#include <string>

#include "json.h"

using namespace news_rewriter;

static void test_parse_scalars() {
    {
        bool ok = false;
        Json j = Json::parse("null", &ok);
        TEST_ASSERT_TRUE(ok);
        TEST_ASSERT_TRUE(j.is_null());
    }
    {
        bool ok = false;
        Json j = Json::parse("true", &ok);
        TEST_ASSERT_TRUE(ok);
        TEST_ASSERT_TRUE(j.is_bool());
        TEST_ASSERT_TRUE(j.as_bool());
    }
    {
        bool ok = false;
        Json j = Json::parse("42", &ok);
        TEST_ASSERT_TRUE(ok);
        TEST_ASSERT_TRUE(j.is_number());
        TEST_ASSERT_EQUAL(j.as_int(), 42);
    }
    {
        bool ok = false;
        Json j = Json::parse("-3.5e2", &ok);
        TEST_ASSERT_TRUE(ok);
        TEST_ASSERT_TRUE(j.is_number());
        TEST_ASSERT_EQUAL(j.as_double(), -350.0);
    }
    {
        bool ok = false;
        Json j = Json::parse("\"hello\\nworld\"", &ok);
        TEST_ASSERT_TRUE(ok);
        TEST_ASSERT_TRUE(j.is_string());
        TEST_ASSERT_EQUAL(j.as_string(), "hello\nworld");
    }
}

static void test_parse_object() {
    const std::string text = "{\"a\": 1, \"b\": {\"c\": \"x\"}, \"arr\": [1, 2, 3]}";
    bool ok = false;
    Json j = Json::parse(text, &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(j.is_object());
    TEST_ASSERT_EQUAL(j.get("a").as_int(), 1);
    TEST_ASSERT_EQUAL(j.get("b").get("c").as_string(), "x");
    const Json& arr = j.get("arr");
    TEST_ASSERT_TRUE(arr.is_array());
    TEST_ASSERT_EQUAL(arr.size(), std::size_t(3));
    TEST_ASSERT_EQUAL(arr[1].as_int(), 2);
}

static void test_parse_unicode_escape() {
    bool ok = false;
    Json j = Json::parse("\"\\u043f\\u0440\\u0438\\u0432\\u0435\\u0442\"", &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(j.as_string(), "привет");
}

static void test_parse_errors() {
    bool ok = true;
    Json::parse("{ invalid", &ok);
    TEST_ASSERT_FALSE(ok);
    ok = true;
    Json::parse("{\"a\": }", &ok);
    TEST_ASSERT_FALSE(ok);
    ok = true;
    Json::parse("[1, 2", &ok);
    TEST_ASSERT_FALSE(ok);
    ok = true;
    Json::parse("tru", &ok);
    TEST_ASSERT_FALSE(ok);
    ok = true;
    Json::parse("", &ok);
    TEST_ASSERT_FALSE(ok);
}

static void test_roundtrip() {
    Json j = Json::object();
    j["name"] = "news_rewriter";
    j["count"] = 42;
    j["ratio"] = 0.5;
    j["active"] = true;
    j["nothing"] = Json();
    Json arr = Json::array();
    arr.push(Json("a"));
    arr.push(Json(1));
    j["list"] = arr;
    Json nested = Json::object();
    nested["x"] = Json("y");
    j["nested"] = nested;

    const std::string dumped = j.dump();

    bool ok = false;
    Json r = Json::parse(dumped, &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(r.get("name").as_string(), "news_rewriter");
    TEST_ASSERT_EQUAL(r.get("count").as_int(), 42);
    TEST_ASSERT_EQUAL(r.get("ratio").as_double(), 0.5);
    TEST_ASSERT_TRUE(r.get("active").as_bool());
    TEST_ASSERT_TRUE(r.get("nothing").is_null());
    const Json& list = r.get("list");
    TEST_ASSERT_TRUE(list.is_array());
    TEST_ASSERT_EQUAL(list.size(), std::size_t(2));
    TEST_ASSERT_EQUAL(list[0].as_string(), "a");
    TEST_ASSERT_EQUAL(list[1].as_int(), 1);
    TEST_ASSERT_EQUAL(r.get("nested").get("x").as_string(), "y");
}

static void test_escape_special_chars() {
    Json j;
    j = Json("a\"b\\c\n");
    const std::string dumped = j.dump();
    bool ok = false;
    Json r = Json::parse(dumped, &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(r.as_string(), "a\"b\\c\n");
}

static void test_missing_key_returns_null() {
    Json j = Json::object();
    j["a"] = Json(1);
    TEST_ASSERT_TRUE(j.get("missing").is_null());
    TEST_ASSERT_FALSE(j.contains("missing"));
    TEST_ASSERT_TRUE(j.contains("a"));
}

REGISTER_TEST(test_parse_scalars);
REGISTER_TEST(test_parse_object);
REGISTER_TEST(test_parse_unicode_escape);
REGISTER_TEST(test_parse_errors);
REGISTER_TEST(test_roundtrip);
REGISTER_TEST(test_escape_special_chars);
REGISTER_TEST(test_missing_key_returns_null);

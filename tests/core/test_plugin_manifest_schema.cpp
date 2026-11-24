// test_plugin_manifest_schema.cpp — схема манифеста плагина (R2.5).
//
// Проверяется не «схема красивая», а то, что сломало бы загрузку молча:
//   - файл схемы — вообще JSON (опечатка в запятой = схема не читается);
//   - в enum прав есть rag_write (без него манифест с этим правом
//     отвергался бы как «неизвестное право»);
//   - блок rag объявляет те же ключи, что читает хост (R3.3);
//   - additionalProperties: false — иначе новое поле проходит мимо схемы;
//   - приложенные манифесты (wp_coder, news_rewriter) соответствуют
//     объявленным в схеме типам и enum'у прав.
//
// Схема и манифесты лежат в дереве исходников, а не в build/: тест иначе
// проверял бы копии, как это уже случалось с версией плагина.

#include "../test_framework.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using json = nlohmann::json;

namespace {

#ifndef PLUGIN_SCHEMA_PATH
#define PLUGIN_SCHEMA_PATH "plugins/plugin.schema.json"
#endif
#ifndef PLUGIN_MANIFEST_DIR
#define PLUGIN_MANIFEST_DIR "plugins"
#endif

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

json load_json(const std::string& path) {
    return json::parse(read_file(path), nullptr, /*allow_exceptions=*/false);
}

bool string_in(const json& array, const std::string& value) {
    if (!array.is_array()) return false;
    for (const auto& v : array) {
        if (v.is_string() && v.get<std::string>() == value) return true;
    }
    return false;
}

} // namespace

void test_schema_is_valid_json() {
    const json schema = load_json(PLUGIN_SCHEMA_PATH);
    TEST_ASSERT(!schema.is_discarded());
    TEST_ASSERT(schema.is_object());

    /* Без этого схема «есть», но не описывает ничего: плагин с новым
     * полем прошёл бы проверку, а хост его не увидел бы. */
    TEST_ASSERT(schema.value("additionalProperties", true) == false);
}

void test_schema_declares_rag_write_permission() {
    const json schema = load_json(PLUGIN_SCHEMA_PATH);
    const json permissions = schema["properties"]["permissions"]["items"];
    TEST_ASSERT(string_in(permissions["enum"], "rag_write"));
    TEST_ASSERT(string_in(permissions["enum"], "rag_access"));
}

void test_schema_declares_rag_block() {
    const json schema = load_json(PLUGIN_SCHEMA_PATH);
    TEST_ASSERT(schema["properties"].contains("rag"));

    const json rag = schema["properties"]["rag"];
    TEST_ASSERT(rag.is_object());
    TEST_ASSERT(rag.value("additionalProperties", true) == false);

    /* Ключи, которые читает хост (R3.3). Если схема объявит лишний —
     * плагин «настроит» то, чего нет; если пропустит нужный — настройка
     * не пройдёт валидацию. */
    for (const char* key : {"enabled", "scope_suffix", "roots", "max_docs",
                            "max_chunk_bytes", "max_index_bytes", "mode"}) {
        TEST_ASSERT(rag["properties"].contains(key));
    }

    /* Суффикс — не путь: двоеточие и слэш запрещены (это разделитель
     * скоупа и обход каталога). */
    TEST_ASSERT(rag["properties"]["scope_suffix"].value("maxLength", 0) == 64);
}

void test_shipped_manifests_match_schema() {
    const json schema = load_json(PLUGIN_SCHEMA_PATH);
    const json permissions_enum = schema["properties"]["permissions"]["items"]["enum"];
    const std::string name_pattern = schema["properties"]["name"]["pattern"];

    const char* manifests[] = {"wp_coder.json", "news_rewriter.json"};
    size_t checked = 0;

    for (const char* file : manifests) {
        const std::string path =
            (std::filesystem::path(PLUGIN_MANIFEST_DIR) / file).string();
        const json m = load_json(path);
        if (m.is_discarded()) {
            /* Файла может не быть в чужой сборке — это не провал схемы. */
            continue;
        }
        ++checked;

        for (const char* field : {"name", "version", "description", "api_version"}) {
            TEST_ASSERT(m.contains(field));
            TEST_ASSERT(m[field].is_string());
            TEST_ASSERT(!m[field].get<std::string>().empty());
        }

        /* Имя — идентификатор: хост строит из него имя скоупа и путь. */
        const std::string name = m["name"].get<std::string>();
        TEST_ASSERT(name.size() >= 2);
        TEST_ASSERT(name[0] >= 'a' && name[0] <= 'z');
        for (char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                            c == '_' || c == '-';
            TEST_ASSERT(ok);
        }
        (void)name_pattern;

        /* Права — только из enum: неизвестное право означало бы, что
         * плагин просит то, чего хост не умеет, и молча не получает. */
        if (m.contains("permissions") && m["permissions"].is_array()) {
            for (const auto& p : m["permissions"]) {
                TEST_ASSERT(p.is_string());
                TEST_ASSERT(string_in(permissions_enum, p.get<std::string>()));
            }
        }
    }

    /* Хотя бы один манифест обязан найтись: иначе тест «прошёл» на пустом
     * каталоге — ровно та ловушка, из-за которой проверки молчат. */
    TEST_ASSERT(checked > 0);
}

int main() {
    REGISTER_TEST(test_schema_is_valid_json);
    REGISTER_TEST(test_schema_declares_rag_write_permission);
    REGISTER_TEST(test_schema_declares_rag_block);
    REGISTER_TEST(test_shipped_manifests_match_schema);

    return test::TestRunner::instance().run();
}
#include "test_framework.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "sink.h"
#include "storage.h"

using namespace news_rewriter;

namespace {

namespace fs = std::filesystem;

// Временный каталог для теста (уникальный, авто-очистка).
class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("news_rewriter_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() { fs::remove_all(path_); }
    std::string path() const { return path_.string(); }

private:
    fs::path path_;
};

static Article make_article(const std::string& url, const std::string& title,
                            const std::string& body) {
    Article a;
    a.url = url;
    a.id = sha256_hex(url);
    a.source = "test.example";
    a.fetched_at = "2026-08-08T12:00:00Z";
    a.title_original = title;
    a.body_original = body;
    a.title_rewritten = "Переписан: " + title;
    a.body_rewritten = "Переписан: " + body;
    a.language = "ru";
    a.content_hash = sha256_hex(title + "\n" + body);
    return a;
}

static void test_storage_init_and_dirs() {
    TempDir tmp;
    Storage s;
    TEST_ASSERT_TRUE(s.init(tmp.path()));
    TEST_ASSERT_TRUE(s.ready());
    TEST_ASSERT_TRUE(fs::exists(fs::path(tmp.path()) / "news_rewriter" / "articles"));
    TEST_ASSERT_TRUE(fs::is_directory(fs::path(tmp.path()) / "news_rewriter" / "articles"));
}

static void test_storage_save_json_and_md() {
    TempDir tmp;
    Storage s;
    TEST_ASSERT_TRUE(s.init(tmp.path()));

    const Article a = make_article("https://test.example/news/1", "Заголовок", "Текст");
    TEST_ASSERT_TRUE(s.save_article_json(a));
    TEST_ASSERT_TRUE(s.save_article_md(a));

    TEST_ASSERT_TRUE(fs::exists(s.article_json_path(a)));
    TEST_ASSERT_TRUE(fs::exists(s.article_md_path(a)));

    // содержимое .md — человекочитаемый рерайт
    std::ifstream in(s.article_md_path(a));
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    TEST_ASSERT(content.find("Переписан") != std::string::npos);
    TEST_ASSERT(content.find("Текст (оригинал)") == std::string::npos);
    TEST_ASSERT(content.find("Переписанный заголовок") == std::string::npos);
    // заголовок файла — переписанный, а не оригинальный
    TEST_ASSERT(content.find("# Переписан: Заголовок") != std::string::npos);
    // порядок: заголовок → текст рерайта → ссылка и дата
    const std::size_t p_body = content.find("Переписан: Текст");
    const std::size_t p_src = content.find("Источник:");
    const std::size_t p_date = content.find("Дата:");
    TEST_ASSERT(p_body != std::string::npos);
    TEST_ASSERT(p_src != std::string::npos);
    TEST_ASSERT(p_date != std::string::npos);
    TEST_ASSERT(p_body < p_src);
    TEST_ASSERT(p_src < p_date);
}

// Относительный URL заглавного изображения резолвится в абсолютный при
// сохранении .md (и JSON), иначе ссылка на фото в локальном файле битая.
static void test_storage_resolves_relative_image_url() {
    TempDir tmp;
    Storage s;
    TEST_ASSERT_TRUE(s.init(tmp.path()));

    Article a = make_article("https://test.example/news/1", "Заголовок", "Текст");
    a.source_image = "/img/hero.jpg";   // относительный путь из источника
    TEST_ASSERT_TRUE(s.save_article_json(a));
    TEST_ASSERT_TRUE(s.save_article_md(a));

    std::ifstream in(s.article_md_path(a));
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // В .md — абсолютный URL, а не сырой относительный.
    TEST_ASSERT(content.find("https://test.example/img/hero.jpg)") != std::string::npos);
    TEST_ASSERT(content.find("](/img/hero.jpg)") == std::string::npos);

    // И в JSON сохранён абсолютный URL.
    std::ifstream jin(s.article_json_path(a));
    std::string jraw((std::istreambuf_iterator<char>(jin)), std::istreambuf_iterator<char>());
    bool ok = false;
    Json j = Json::parse(jraw, &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(std::string(j["source_image"].as_string()),
                      "https://test.example/img/hero.jpg");
}

// Дата публикации оригинала попадает в .md («Дата оригинала:») и в JSON,
// чтобы в выходной статье было видно, когда новость вышла в источнике.
static void test_storage_includes_original_date() {
    TempDir tmp;
    Storage s;
    TEST_ASSERT_TRUE(s.init(tmp.path()));

    Article a = make_article("https://test.example/news/1", "Заголовок", "Текст");
    a.published_at = 1755000000;   // 2025-08-12T12:00:00Z
    TEST_ASSERT_TRUE(s.save_article_json(a));
    TEST_ASSERT_TRUE(s.save_article_md(a));

    std::ifstream in(s.article_md_path(a));
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    TEST_ASSERT(content.find("Дата оригинала: 2025-08-12T12:00:00Z") !=
                std::string::npos);

    std::ifstream jin(s.article_json_path(a));
    std::string jraw((std::istreambuf_iterator<char>(jin)), std::istreambuf_iterator<char>());
    bool ok = false;
    Json j = Json::parse(jraw, &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT(!j["published_at"].is_null());
    TEST_ASSERT_EQUAL(j["published_at"].as_int(), static_cast<std::int64_t>(1755000000));
}

static void test_storage_index_roundtrip() {
    TempDir tmp;
    Storage s;
    TEST_ASSERT_TRUE(s.init(tmp.path()));

    const Article a = make_article("https://test.example/news/2", "Заголовок 2", "Текст 2");
    s.mark_written(a);

    Json index;
    TEST_ASSERT_TRUE(s.load_index(index));
    TEST_ASSERT_TRUE(index.is_object());
    TEST_ASSERT_TRUE(index.contains(a.id));
    const Json& entry = index.get(a.id);
    TEST_ASSERT_TRUE(entry.is_object());
    TEST_ASSERT_EQUAL(entry.get("content_hash").as_string(), a.content_hash);
}

static void test_storage_state_roundtrip() {
    TempDir tmp;
    Storage s;
    TEST_ASSERT_TRUE(s.init(tmp.path()));

    Json state = Json::object();
    state["last_run"] = "2026-08-08T12:00:00Z";
    TEST_ASSERT_TRUE(s.save_state(state));

    Json loaded;
    TEST_ASSERT_TRUE(s.load_state(loaded));
    TEST_ASSERT_EQUAL(loaded.get("last_run").as_string(), "2026-08-08T12:00:00Z");
}

static void test_storage_duplicate_by_id() {
    TempDir tmp;
    Storage s;
    TEST_ASSERT_TRUE(s.init(tmp.path()));

    const Article a = make_article("https://test.example/news/3", "Заголовок 3", "Текст 3");
    TEST_ASSERT_FALSE(s.is_duplicate(a));
    s.mark_written(a);
    TEST_ASSERT_TRUE(s.is_duplicate(a));  // повторный обход того же URL
}

static void test_storage_duplicate_by_content_hash() {
    TempDir tmp;
    Storage s;
    TEST_ASSERT_TRUE(s.init(tmp.path()));

    const Article a = make_article("https://test.example/news/4", "Заголовок 4", "Текст 4");
    s.mark_written(a);

    // другой URL, но тот же текст
    Article b = make_article("https://test.example/other/4", "Заголовок 4", "Текст 4");
    TEST_ASSERT_TRUE(s.is_duplicate(b));
}

static void test_local_file_sink_writes() {
    TempDir tmp;
    Storage s;
    TEST_ASSERT_TRUE(s.init(tmp.path()));

    const Article a = make_article("https://test.example/news/5", "Заголовок 5", "Текст 5");
    std::unique_ptr<Sink> sink = make_local_file_sink(SinkConfig{}, s, LogFn{});
    TEST_ASSERT_TRUE(sink != nullptr);
    TEST_ASSERT_TRUE(sink->write(a));
    TEST_ASSERT_TRUE(fs::exists(s.article_json_path(a)));
    TEST_ASSERT_TRUE(fs::exists(s.article_md_path(a)));
}

static void test_sink_registry() {
    SinkRegistry& reg = SinkRegistry::instance();
    reg.register_factory("local_file", make_local_file_sink);

    TempDir tmp;
    Storage s;
    TEST_ASSERT_TRUE(s.init(tmp.path()));

    SinkConfig cfg;
    cfg.type = "local_file";
    std::unique_ptr<Sink> sink = reg.create(cfg, s, LogFn{});
    TEST_ASSERT_TRUE(sink != nullptr);
    TEST_ASSERT_EQUAL(std::string(sink->name()), "local_file");

    SinkConfig unknown;
    unknown.type = "http";
    TEST_ASSERT_TRUE(reg.create(unknown, s, LogFn{}) == nullptr);
}

REGISTER_TEST(test_storage_init_and_dirs);
REGISTER_TEST(test_storage_save_json_and_md);
REGISTER_TEST(test_storage_resolves_relative_image_url);
REGISTER_TEST(test_storage_includes_original_date);
REGISTER_TEST(test_storage_index_roundtrip);
REGISTER_TEST(test_storage_state_roundtrip);
REGISTER_TEST(test_storage_duplicate_by_id);
REGISTER_TEST(test_storage_duplicate_by_content_hash);
REGISTER_TEST(test_local_file_sink_writes);
REGISTER_TEST(test_sink_registry);

} // namespace

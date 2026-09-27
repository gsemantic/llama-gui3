/*
 * test_glob.cpp — И4.1: поиск файлов по шаблону пути.
 *
 * Проверяется в два слоя, потому что ломается в них разное:
 *   - match_pattern — чистый язык шаблонов (**, *, ?, {a,b}). Здесь важны
 *     границы: «** матчит ноль сегментов» и «{a} — литерал» отличают
 *     рабочий glob от того, который молча не находит ничего;
 *   - find и сам инструмент — обход дерева, фильтры, сортировка и
 *     ЧЕСТНОСТЬ усечения. Молчаливый неполный результат хуже отказа:
 *     модель сделает вывод «файла нет» и пойдёт искать в другом месте.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/glob.h"
#include "../core/json.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/project.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path make_tmp_tree() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_glob_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

void touch(const fs::path& p, const std::string& body = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << body;
}

void init_tools(const fs::path& project) {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = project.string();
        engine_state().allowed_external_paths.clear();
    }
    register_base_tools();
}

/* Дерево фиксированного состава:
 *   index.php
 *   a.php
 *   notes.md
 *   src/app.ts
 *   src/deep/inner.php
 *   src/deep/deeper/innermost.php
 *   .github/workflows/deploy.yml
 *   .env.example
 *   node_modules/pkg/index.php      (пропускается обходом)
 *   .git/config                      (пропускается обходом) */
fs::path build_tree() {
    fs::path t = make_tmp_tree();
    touch(t / "index.php");
    touch(t / "a.php");
    touch(t / "notes.md");
    touch(t / "src" / "app.ts");
    touch(t / "src" / "deep" / "inner.php");
    touch(t / "src" / "deep" / "deeper" / "innermost.php");
    touch(t / ".github" / "workflows" / "deploy.yml");
    touch(t / ".env.example");
    touch(t / "node_modules" / "pkg" / "index.php");
    touch(t / ".git" / "config");
    return t;
}

std::vector<std::string> paths_of(const fileglob::Result& r) {
    std::vector<std::string> v;
    for (const auto& e : r.entries) v.push_back(e.path);
    return v;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // anonymous namespace

/* ======================================================================
 * Язык шаблонов
 * ====================================================================== */

TEST(glob_star_matches_within_one_segment_only) {
    ASSERT_TRUE(fileglob::match_pattern("*.php", "a.php"));
    ASSERT_TRUE(fileglob::match_pattern("*.php", "index.php"));
    /* '*' НЕ ходит через разделитель — иначе «*.php» нашла бы всё подряд. */
    ASSERT_FALSE(fileglob::match_pattern("*.php", "src/a.php"));
    ASSERT_FALSE(fileglob::match_pattern("*.php", "a.js"));
    /* Несколько '*' в шаблоне. */
    ASSERT_TRUE(fileglob::match_pattern("*-*test.js", "wp-unit-test.js"));
    ASSERT_FALSE(fileglob::match_pattern("*-*test.js", "wpunit.js"));
}

TEST(glob_question_mark_matches_exactly_one_char) {
    ASSERT_TRUE(fileglob::match_pattern("a?.php", "ab.php"));
    ASSERT_FALSE(fileglob::match_pattern("a?.php", "a.php"));
    ASSERT_FALSE(fileglob::match_pattern("a?.php", "abc.php"));
    /* '?' тоже не пересекает разделитель. */
    ASSERT_FALSE(fileglob::match_pattern("a?c", "a/c"));
}

TEST(glob_double_star_covers_zero_and_many_segments) {
    /* Ноль сегментов: '**' перед '/.php' обязан найти файл в корне. */
    ASSERT_TRUE(fileglob::match_pattern("**/*.php", "index.php"));
    /* Один и несколько сегментов. */
    ASSERT_TRUE(fileglob::match_pattern("**/*.php", "a.php"));
    ASSERT_TRUE(fileglob::match_pattern("**/*.php", "src/a.php"));
    ASSERT_TRUE(fileglob::match_pattern("**/*.php", "src/deep/deeper/a.php"));
    /* '**' посередине: ноль сегментов и любое их число. */
    ASSERT_TRUE(fileglob::match_pattern("src/**/*.php", "src/a.php"));
    ASSERT_TRUE(fileglob::match_pattern("src/**/*.php", "src/deep/a.php"));
    ASSERT_TRUE(fileglob::match_pattern("src/**/*.php", "src/deep/deeper/a.php"));
    /* Порядок каталогов значим. */
    ASSERT_FALSE(fileglob::match_pattern("src/**/*.php", "tests/src/a.php"));
    ASSERT_FALSE(fileglob::match_pattern("**/*.php", "a.js"));
}

TEST(glob_braces_expand_alternatives) {
    ASSERT_TRUE(fileglob::match_pattern("**/*.{ts,tsx}", "src/app.ts"));
    ASSERT_TRUE(fileglob::match_pattern("**/*.{ts,tsx}", "src/app.tsx"));
    ASSERT_FALSE(fileglob::match_pattern("**/*.{ts,tsx}", "src/app.js"));
    /* Альтернативы с кусками пути — как в bash. */
    ASSERT_TRUE(fileglob::match_pattern("{src,tests}/**/*.php", "src/a.php"));
    ASSERT_TRUE(fileglob::match_pattern("{src,tests}/**/*.php", "tests/deep/a.php"));
    ASSERT_FALSE(fileglob::match_pattern("{src,tests}/**/*.php", "docs/a.php"));
    /* Вложенные скобки: без их поддержки '{a{b,c}}' молча схлопнулся бы
     * в себя и не нашёл бы ничего. */
    ASSERT_TRUE(fileglob::match_pattern("**/*.{js,{ts,tsx}}", "src/app.tsx"));
    /* Скобки без запятой — литерал, как в bash. */
    ASSERT_TRUE(fileglob::match_pattern("weird{a}.txt", "weird{a}.txt"));
    ASSERT_FALSE(fileglob::match_pattern("weird{a}.txt", "weirda.txt"));
    /* Непарная '{' не должна ронять поиск. */
    ASSERT_TRUE(fileglob::match_pattern("a{b.txt", "a{b.txt"));
}

TEST(glob_pattern_is_normalized_before_matching) {
    /* Модели пишут './src//*.php' и Windows-слэши — и то и другое должно
     * значить одно и то же, иначе glob молчал бы впустую. */
    ASSERT_TRUE(fileglob::match_pattern("./src/*.ts", "src/app.ts"));
    ASSERT_TRUE(fileglob::match_pattern("src//*.ts", "src/app.ts"));
    ASSERT_TRUE(fileglob::match_pattern("src\\*.ts", "src/app.ts"));
    ASSERT_TRUE(fileglob::match_pattern("src/", "src"));
}

TEST(glob_matches_hidden_files) {
    /* Осознанное решение (см. glob.h): агент ищет .github и .env.example,
     * и пустой результат на законном вопросе хуже лишнего совпадения. */
    ASSERT_TRUE(fileglob::match_pattern("**/*.yml", ".github/workflows/deploy.yml"));
    ASSERT_TRUE(fileglob::match_pattern(".*", ".env.example"));
    ASSERT_TRUE(fileglob::match_pattern("**/*", ".gitignore"));
}

/* ======================================================================
 * Обход дерева
 * ====================================================================== */

TEST(glob_find_walks_tree_and_returns_relative_paths) {
    fs::path t = build_tree();
    fileglob::Result r = fileglob::find(t.string(), "**/*.php");
    auto v = paths_of(r);
    ASSERT_TRUE(r.error.empty());
    ASSERT_TRUE(contains(v, "index.php"));
    ASSERT_TRUE(contains(v, "src/deep/inner.php"));
    ASSERT_TRUE(contains(v, "src/deep/deeper/innermost.php"));
    /* Каталоги по умолчанию не в ответе: '**' + '/ *' на репозитории иначе
     * наполнил бы его каталогами раньше файлов. */
    for (const auto& e : r.entries) ASSERT_FALSE(e.is_dir);
    /* node_modules и .git не обходятся: тот же список, что у repo_map. */
    ASSERT_FALSE(contains(v, "node_modules/pkg/index.php"));
    ASSERT_FALSE(contains(v, ".git/config"));
    fs::remove_all(t);
}

TEST(glob_find_applies_extension_filter) {
    fs::path t = build_tree();
    fileglob::Options o;
    o.extensions = {"php"};
    fileglob::Result r = fileglob::find(t.string(), "**/*", o);
    auto v = paths_of(r);
    ASSERT_TRUE(contains(v, "index.php"));
    ASSERT_FALSE(contains(v, "notes.md"));
    ASSERT_FALSE(contains(v, "src/app.ts"));
    /* Без точки в аргументе тоже: ".php" и "php" — одно и то же. */
    fileglob::Options o2;
    o2.extensions = {".TS"};
    auto v2 = paths_of(fileglob::find(t.string(), "**/*", o2));
    ASSERT_TRUE(contains(v2, "src/app.ts"));
    ASSERT_FALSE(contains(v2, "index.php"));
    fs::remove_all(t);
}

TEST(glob_find_sorts_newest_first) {
    fs::path t = make_tmp_tree();
    touch(t / "old.php");
    touch(t / "new.php");
    touch(t / "mid.php");
    /* Задаём разные времена явно: полагаться на скорость записи значило
     * бы проверять удачу, а не сортировку. */
    std::error_code ec;
    fs::last_write_time(t / "old.php", fs::file_time_type::clock::now() - std::chrono::hours(3), ec);
    fs::last_write_time(t / "new.php", fs::file_time_type::clock::now(), ec);
    fs::last_write_time(t / "mid.php", fs::file_time_type::clock::now() - std::chrono::hours(1), ec);

    fileglob::Result r = fileglob::find(t.string(), "*.php");
    auto v = paths_of(r);
    ASSERT_EQ(v.size(), (size_t)3);
    ASSERT_EQ(v[0], std::string("new.php"));
    ASSERT_EQ(v[1], std::string("mid.php"));
    ASSERT_EQ(v[2], std::string("old.php"));

    fileglob::Options o;
    o.newest_first = false;
    ASSERT_EQ(paths_of(fileglob::find(t.string(), "*.php", o))[0], std::string("mid.php"));
    fs::remove_all(t);
}

TEST(glob_find_reports_truncation_instead_of_pretending) {
    fs::path t = make_tmp_tree();
    for (int i = 0; i < 7; ++i) touch(t / ("f" + std::to_string(i) + ".php"));
    fileglob::Options o;
    o.limit = 3;
    fileglob::Result r = fileglob::find(t.string(), "*.php", o);
    ASSERT_EQ(r.entries.size(), (size_t)3);
    ASSERT_EQ(r.matched, (size_t)7);
    /* Молчаливый неполный список хуже явного: модель сочла бы, что
     * файлов всего три. */
    ASSERT_TRUE(r.truncated);
    fs::remove_all(t);
}

TEST(glob_find_caps_walk_and_says_so) {
    fs::path t = make_tmp_tree();
    for (int i = 0; i < 20; ++i) touch(t / ("f" + std::to_string(i) + ".txt"));
    fileglob::Options o;
    o.max_visited = 5;
    o.limit = 100;
    fileglob::Result r = fileglob::find(t.string(), "*.nothing", o);
    ASSERT_TRUE(r.walk_capped);
    ASSERT_TRUE(r.visited <= 6);
    ASSERT_TRUE(r.entries.empty());
    ASSERT_TRUE(r.error.empty());
    fs::remove_all(t);
}

TEST(glob_find_reports_errors_instead_of_empty_result) {
    fileglob::Result missing = fileglob::find("/tmp/wp_coder_glob_missing_dir_zzz", "*.php");
    ASSERT_TRUE(!missing.error.empty());
    ASSERT_TRUE(missing.entries.empty());

    fs::path t = build_tree();
    fileglob::Result notdir = fileglob::find((t / "index.php").string(), "*");
    ASSERT_TRUE(notdir.error.find("не каталог") != std::string::npos);

    /* Пустой шаблон — ошибка с подсказкой, а не «ничего не найдено»:
     * иначе модель будет ужесточать шаблон вместо того, чтобы его задать. */
    fileglob::Result empty = fileglob::find(t.string(), "   ");
    ASSERT_TRUE(empty.error.find("pattern") != std::string::npos);
    ASSERT_TRUE(empty.entries.empty());
    fs::remove_all(t);
}

TEST(glob_find_accepts_absolute_pattern_inside_root) {
    fs::path t = build_tree();
    /* Модели присылают путь целиком; отказ был бы неинформативным. */
    std::string pattern = t.lexically_normal().generic_string() + "/src/**/*.php";
    auto v = paths_of(fileglob::find(t.string(), pattern));
    ASSERT_TRUE(contains(v, "src/app.ts") == false);
    ASSERT_TRUE(contains(v, "src/deep/inner.php"));
    ASSERT_TRUE(contains(v, "src/deep/deeper/innermost.php"));
    ASSERT_TRUE(v.size() == 2);
    fs::remove_all(t);
}

TEST(glob_find_does_not_follow_symlinks_out_of_tree) {
    fs::path t = build_tree();
    fs::path outside = make_tmp_tree();
    touch(outside / "secret.php");
    std::error_code ec;
    fs::create_directory_symlink(outside, t / "link", ec);
    if (ec) {  /* симлинки недоступны — тест не применим */
        fs::remove_all(t);
        fs::remove_all(outside);
        return;
    }
    auto v = paths_of(fileglob::find(t.string(), "**/*.php"));
    /* Обход по ссылкам либо зациклился бы, либо ушёл бы из проекта мимо
     * гейта на внешние пути. */
    ASSERT_FALSE(contains(v, "link/secret.php"));
    fs::remove_all(t);
    fs::remove_all(outside);
}

/* ======================================================================
 * Инструмент
 * ====================================================================== */

TEST(glob_tool_is_registered_and_documented) {
    fs::path t = build_tree();
    init_tools(t);
    const ToolDef* d = ToolsRegistry::instance().find("glob");
    ASSERT_TRUE(d != nullptr);
    /* Только чтение: в режиме Research glob обязан быть доступен. */
    ASSERT_TRUE(tf_has(d->flags, TF_READ_ONLY));
    ASSERT_EQ(permission_key_of(*d), std::string("read"));
    /* И1.5: описание генерируется из схемы, и без pattern в каталоге
     * модель не знает, что ей передавать. */
    std::string cat = ToolsRegistry::instance().build_tool_catalogue();
    size_t at = cat.find("\n- glob —");
    ASSERT_TRUE(at != std::string::npos);
    size_t end = cat.find("\n- ", at + 1);
    std::string block = cat.substr(at + 1, (end == std::string::npos
                                           ? cat.size() : end) - at - 1);
    ASSERT_TRUE(block.find("pattern") != std::string::npos);
    ASSERT_TRUE(block.find("обязательно") != std::string::npos);
    fs::remove_all(t);
}

TEST(glob_tool_requires_pattern) {
    fs::path t = build_tree();
    init_tools(t);
    json::JsonValue args = json::JsonValue::object();
    std::string r = ToolsRegistry::instance().run("glob", args);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(r.find("pattern") != std::string::npos);
    fs::remove_all(t);
}

TEST(glob_tool_finds_files_and_reports_count) {
    fs::path t = build_tree();
    init_tools(t);
    json::JsonValue args = json::JsonValue::object();
    args.set("pattern", "**/*.php");
    std::string r = ToolsRegistry::instance().run("glob", args);
    ASSERT_TRUE(r.find("index.php") != std::string::npos);
    ASSERT_TRUE(r.find("src/deep/inner.php") != std::string::npos);
    ASSERT_TRUE(r.find("ничего не найдено") == std::string::npos);
    /* node_modules в ответе быть не должно. */
    ASSERT_TRUE(r.find("node_modules") == std::string::npos);
    fs::remove_all(t);
}

TEST(glob_tool_respects_path_and_include) {
    fs::path t = build_tree();
    init_tools(t);
    json::JsonValue args = json::JsonValue::object();
    args.set("pattern", "**/*");
    args.set("path", "src");
    args.set("include", "php, ts");
    std::string r = ToolsRegistry::instance().run("glob", args);
    ASSERT_TRUE(r.find("deep/inner.php") != std::string::npos);
    ASSERT_TRUE(r.find("app.ts") != std::string::npos);
    /* За пределами src ничего не ищется. */
    ASSERT_TRUE(r.find("index.php") == std::string::npos);
    ASSERT_TRUE(r.find("notes.md") == std::string::npos);
    fs::remove_all(t);
}

TEST(glob_tool_says_when_nothing_matches) {
    fs::path t = build_tree();
    init_tools(t);
    json::JsonValue args = json::JsonValue::object();
    args.set("pattern", "**/*.rs");
    std::string r = ToolsRegistry::instance().run("glob", args);
    ASSERT_TRUE(r.find("ничего не найдено") != std::string::npos);
    fs::remove_all(t);
}

TEST(glob_tool_outside_project_requires_permission) {
    fs::path t = build_tree();
    fs::path outside = make_tmp_tree();
    touch(outside / "secret.php");
    init_tools(t);
    json::JsonValue args = json::JsonValue::object();
    args.set("pattern", "*.php");
    args.set("path", outside.string());
    std::string r = ToolsRegistry::instance().run("glob", args);
    /* Либо отказ с требованием разрешения, либо явный запрет — но НЕ
     * список файлов чужого каталога. */
    ASSERT_TRUE(r.find("secret.php") == std::string::npos);
    ASSERT_TRUE(r.find("Доступ") != std::string::npos ||
                r.find("разрешен") != std::string::npos);
    fs::remove_all(t);
    fs::remove_all(outside);
}

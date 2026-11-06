/*
 * test_ast_symbols.cpp — И13.5: repo_map на AST хоста, откат на скан.
 *
 * Проверки двух слоёв и обзора между ними:
 *   1. мост host_bridge/ast_symbols.cpp — зовёт поле хоста и освобождает
 *      строку; молчащая проверка тут опаснее всего, потому что «мост вернул
 *      пустое» и «мост не звал хоста» снаружи неразличимы;
 *   2. core/ast_symbols.cpp — решение по ТРЁМ состояниям ответа плюс
 *      четвёртое (ast:true с пустым списком), которого в ABI нет и ради
 *      которого задача и написана;
 *   3. repo_map — что доходит до модели: символы, хуки и честная строка о
 *      деградации.
 *
 * Материал для слоя 2 — в памяти (подставленный ответ хоста), потому что
 * настоящего хоста у тестов нет: без подстановки проверка была бы зелёной
 * на любом коде, который просто не зовёт слот. Материал для слоя 3 — живой
 * обзор настоящего дерева, и ответ хоста подставляется тем же слотом.
 */

#include "test_framework.h"
#include "test_support.h"

#include "../core/ast_symbols.h"
#include "../core/base_tools.h"
#include "../core/engine.h"
#include "../core/json.h"
#include "../core/tools_registry.h"
#include "../host_bridge/ast_symbols.h"

#include "plugins/plugin_api.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace coder;
namespace fs = std::filesystem;

/* --- Фикстуры --- */

static fs::path make_tmp_tree(const std::string& tag) {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_ast_" + tag + "_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    register_tmp_tree(tmp.string());
    return tmp;
}

static void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

/* Ответ хоста, собранный из тех же полей, что отдаёт ast_symbols_for_file
 * (проверено на живом материале: {"ok":1,"ast":true,"language":"cpp",
 * "symbols":[{"kind":…,"name":…,"parent":…,"line":…,"end_line":…}]}). */
static std::string host_said(const std::string& kind, const std::string& name) {
    return std::string("{\"ok\":1,\"ast\":true,\"language\":\"cpp\",\"symbols\":[")
         + "{\"kind\":\"" + kind + "\",\"name\":\"" + name +
           "\",\"parent\":\"\",\"line\":3,\"end_line\":9}]}";
}

/* Ответ хоста, который возвращает один и тот же текст на любой файл. */
static HostCallbacks host_answering(const std::string& answer) {
    HostCallbacks cb;
    cb.ast_symbols = [answer](const std::string&, const std::string&) {
        return answer;
    };
    return cb;
}

static std::string joined(const std::vector<ast_symbols::Symbol>& v) {
    std::string s;
    for (const auto& sym : v) {
        if (!s.empty()) s += ", ";
        s += ast_symbols::symbol_text(sym);
    }
    return s;
}

/* --- 1. Мост к полю хоста --- */

/*
 * Мост обязан вернуть строку хоста И освободить её. Проверяется счётчиком
 * на fake_free_string, а не «строка совпала»: совпадение строки осталось бы
 * зелёным и при утечке malloc'нутой памяти хоста — а это утечка на каждый
 * файл обзора проекта.
 */
TEST(the_ast_bridge_returns_what_the_host_said_and_frees_the_string) {
    static std::string answer =
        "{\"ok\":1,\"ast\":true,\"symbols\":[{\"kind\":\"class\",\"name\":\"W\"}]}";
    static int freed = 0;
    static char* freed_ptr = nullptr;

    LlamaHostApi api{};
    api.size = sizeof(api);
    api.llm_ast_symbols = [](LlamaPluginHost*, const char*, const char*) -> char* {
        return strdup(answer.c_str());
    };
    api.free_string = [](LlamaPluginHost*, char* s) {
        ++freed;
        freed_ptr = s;
        free(s);
    };

    const std::string got = host::call_ast_symbols(&api, nullptr, "/tmp/x.cpp", "");
    ASSERT_EQ(got, answer);
    ASSERT_EQ(freed, 1);
    ASSERT_TRUE(freed_ptr != nullptr);
}

/* У старого хоста поля нет. Мост обязан молчать, НЕ читая поле за пределами
 * заполненной части структуры: подставленная функция считает вызовы, и
 * ненулевой счётчик означал бы чтение за границей. */
TEST(the_ast_bridge_asks_nobody_when_the_host_has_no_such_field) {
    static int called = 0;
    LlamaHostApi api{};
    api.size = offsetof(LlamaHostApi, llm_ast_symbols);   /* поле не заполнено */
    api.llm_ast_symbols = [](LlamaPluginHost*, const char*, const char*) -> char* {
        ++called;
        return nullptr;
    };
    ASSERT_EQ(host::call_ast_symbols(&api, nullptr, "/tmp/x.cpp", ""), std::string());
    ASSERT_EQ(called, 0);
}

/* Хост ответил nullptr — это «спросить не вышло», а не «символов нет».
 * Различать обязан вызывающий (см. core/ast_symbols.cpp), и мост обязан
 * просто не выдавать за ответ мусор. */
TEST(the_ast_bridge_survives_a_host_that_answers_nothing) {
    LlamaHostApi api{};
    api.size = sizeof(api);
    api.llm_ast_symbols = [](LlamaPluginHost*, const char*, const char*) -> char* {
        return nullptr;
    };
    api.free_string = [](LlamaPluginHost*, char*) {};
    ASSERT_EQ(host::call_ast_symbols(&api, nullptr, "/tmp/x.cpp", ""), std::string());
}

/* Пустой путь — граница моста, а не хоста: хост на такой отвечает отказом
 * («пустой путь» в его ast_symbols_for_file), но звать его для этого
 * незачем, и вызывающий узнал бы об ошибке только постфактум. Подставленная
 * функция считает вызовы, поэтому проверка ловит именно ЗОВОН, а не текст
 * ответа. */
TEST(the_ast_bridge_does_not_ask_the_host_about_an_empty_path) {
    static int called = 0;
    LlamaHostApi api{};
    api.size = sizeof(api);
    api.llm_ast_symbols = [](LlamaPluginHost*, const char*, const char*) -> char* {
        ++called;
        return strdup("{\"ok\":0}");
    };
    api.free_string = [](LlamaPluginHost*, char* s) { free(s); };
    ASSERT_EQ(host::call_ast_symbols(&api, nullptr, "", ""), std::string());
    ASSERT_EQ(called, 0);
}

/*
 * free_string у хоста обязателен по ABI, но мост зовёт его через проверку —
 * как и llm_blocking.cpp: утечка строки хоста не зависит от того, кто
 * прислал указатель, а падение по нулевому указателю зависит. Здесь хост
 * отдаёт malloc'нутую строку, а free_string у него нет: мост обязан
 * пережить это и не вызвать пустую функцию.
 */
TEST(the_ast_bridge_does_not_call_a_host_that_cannot_free) {
    LlamaHostApi api{};
    api.size = sizeof(api);
    api.llm_ast_symbols = [](LlamaPluginHost*, const char*, const char*) -> char* {
        return strdup("{\"ok\":1,\"ast\":true,\"symbols\":[{\"kind\":\"class\","
                      "\"name\":\"Kept\"}]}");
    };
    api.free_string = nullptr;
    const std::string got = host::call_ast_symbols(&api, nullptr, "/tmp/x.cpp", "");
    ASSERT_TRUE(got.find("Kept") != std::string::npos);
}

/* --- 2. Решение «AST или скан» --- */

/*
 * Положительный контроль на материале в памяти: ответ с РЕАЛЬНЫМИ полями
 * даёт символы, и вид словаря переносится в метку обзора. Если разбор
 * перестанет работать, проверка падает здесь, а не «где-то дальше».
 */
TEST(symbols_of_the_host_become_symbols_of_the_overview) {
    const fs::path t = make_tmp_tree("ok");
    const fs::path f = t / "a.cpp";
    write_file(f, "namespace acme { void f(); }\n");

    const ast_symbols::FileSymbols r =
        ast_symbols::file_symbols(host_answering(host_said("namespace", "acme")), f.string());
    ASSERT_TRUE(r.from_ast);
    ASSERT_EQ(r.degrade, std::string());
    ASSERT_EQ(joined(r.symbols), std::string("n:acme"));
}

/* Словарь видов хоста закрытый (ast_kind_for_type, И6.3): у каждого вида,
 * кроме unknown, обязана быть метка, иначе символ молча исчез бы из обзора.
 * Перечисление написано здесь руками намеренно: новый вид на стороне хоста
 * обязан уронить эту проверку, а не тихо не печататься. */
TEST(every_kind_of_the_host_dictionary_gets_a_tag_in_the_overview) {
    const fs::path t = make_tmp_tree("kinds");
    const fs::path f = t / "a.cpp";
    write_file(f, "x\n");

    struct Case { const char* kind; const char* expect; };
    const Case cases[] = {
        {"function", "f:fn"}, {"method", "f:fn"}, {"block", "f:fn"},
        {"class", "c:fn"},   {"namespace", "n:fn"}, {"enum", "e:fn"},
        {"typedef", "t:fn"}, {"macro", "m:fn"},
    };
    for (const auto& c : cases) {
        const ast_symbols::FileSymbols r =
            ast_symbols::file_symbols(host_answering(host_said(c.kind, "fn")), f.string());
        ASSERT_EQ(joined(r.symbols), std::string(c.expect));
    }
}

/*
 * ГЛАВНОЕ этой задачи. Хост отвечает «ok, ast:true, symbols:[]» — а на
 * живой php-файл отвечает именно так, когда грамматика новее библиотеки
 * tree-sitter: разбор не удался, но parse_source признака неудачи не имеет.
 * Снаружи это неотличимо от файла без определений, и поверить ответу —
 * значит молча выкинуть весь обзор проекта.
 */
TEST(an_empty_symbol_list_from_the_host_is_not_taken_for_a_file_without_definitions) {
    const fs::path t = make_tmp_tree("empty");
    const fs::path f = t / "a.php";
    write_file(f, "<?php\nfunction real_one() { return 1; }\n");

    HostCallbacks cb = host_answering("{\"ok\":1,\"ast\":true,\"symbols\":[]}");
    const ast_symbols::FileSymbols r = ast_symbols::file_symbols(cb, f.string());
    ASSERT_FALSE(r.from_ast);
    ASSERT_EQ(joined(r.symbols), std::string("f:real_one"));
    ASSERT_TRUE(r.degrade.find("ноль символов") != std::string::npos);
}

/* ast:false — грамматики нет (на этой машине так отвечает php). Причина
 * хоста обязана дойти до обзора: по ней модель отличает «собери грамматику»
 * от «доверься скану». */
TEST(a_file_without_a_grammar_names_the_reason_of_the_host) {
    const fs::path t = make_tmp_tree("nogram");
    const fs::path f = t / "a.php";
    write_file(f, "<?php\nfunction real_one() {}\n");

    HostCallbacks cb = host_answering(
        "{\"ok\":1,\"ast\":false,\"language\":\"php\",\"symbols\":[],"
        "\"reason\":\"грамматика tree-sitter для 'php' не собрана\"}");
    const ast_symbols::FileSymbols r = ast_symbols::file_symbols(cb, f.string());
    ASSERT_FALSE(r.from_ast);
    ASSERT_EQ(joined(r.symbols), std::string("f:real_one"));
    ASSERT_TRUE(r.degrade.find("php") != std::string::npos);
}

/* Отказ хоста (ok:0) — то же самое, только с причиной. */
TEST(a_refusal_of_the_host_is_named_and_not_swallowed) {
    const fs::path t = make_tmp_tree("refuse");
    const fs::path f = t / "a.cobol";
    write_file(f, "IDENTIFICATION DIVISION.\n");

    HostCallbacks cb = host_answering(
        "{\"ok\":0,\"ast\":false,\"error\":\"язык не определён по расширению\"}");
    const ast_symbols::FileSymbols r = ast_symbols::file_symbols(cb, f.string());
    ASSERT_FALSE(r.from_ast);
    ASSERT_TRUE(r.degrade.find("язык не определён") != std::string::npos);
}

/*
 * Три состояния ответа проверяются НЕЗАВИСИМО друг от друга. Нынешний хост
 * на ok:0 держит ast:false, поэтому отказ маскирует проверку ok:false, а
 * отказ с ast:false маскирует проверку ok:false наоборот: код, снявший одну
 * из проверок, на честном хосте остаётся зелёным. Здесь отказ приходит с
 * ast:true и непустым списком — то есть хост, который признал отказ и всё
 * же приложил символы. Поверить им нельзя: ok:0 — это «разбора не было».
 */
TEST(a_refusal_is_not_believed_even_when_the_host_sends_symbols_with_it) {
    const fs::path t = make_tmp_tree("refuse2");
    const fs::path f = t / "a.cobol";
    write_file(f, "IDENTIFICATION DIVISION.\n");

    HostCallbacks cb = host_answering(
        "{\"ok\":0,\"ast\":true,\"symbols\":[{\"kind\":\"function\","
        "\"name\":\"from_nowhere\"}],\"error\":\"файл не открыт\"}");
    const ast_symbols::FileSymbols r = ast_symbols::file_symbols(cb, f.string());
    ASSERT_FALSE(r.from_ast);
    ASSERT_EQ(joined(r.symbols), std::string());
    ASSERT_TRUE(r.degrade.find("файл не открыт") != std::string::npos);
}

/* Мусор вместо ответа: и не-JSON, и JSON без symbols. Оба обязаны уйти на
 * скан, а не разобраться в «символы пустые» — иначе битый хост тихо
 * превращает обзор в пустые строки. */
TEST(garbage_from_the_host_is_not_taken_for_symbols) {
    const fs::path t = make_tmp_tree("garbage");
    const fs::path f = t / "a.cpp";
    write_file(f, "class Kept {}\n");

    for (const std::string& answer :
         {std::string("не json вовсе"),
          std::string("{\"ok\":1,\"ast\":true}"),
          std::string("{\"ok\":1,\"ast\":true,\"symbols\":\"нет\"}"),
          std::string("")}) {
        const ast_symbols::FileSymbols r =
            ast_symbols::file_symbols(host_answering(answer), f.string());
        ASSERT_FALSE(r.from_ast);
        ASSERT_EQ(joined(r.symbols), std::string("c:Kept"));
        ASSERT_TRUE(!r.degrade.empty());
    }
}

/* Символы без вида или без имени пропускаются, а если не осталось ничего —
 * файл уходит на скан (пустой список хоста доверия не заслуживает). */
TEST(symbols_the_host_could_not_name_are_dropped_rather_than_invented) {
    const fs::path t = make_tmp_tree("noname");
    const fs::path f = t / "a.cpp";
    write_file(f, "class Kept {}\n");

    HostCallbacks cb = host_answering(
        "{\"ok\":1,\"ast\":true,\"symbols\":[{\"kind\":\"function\",\"name\":\"\"},"
        "{\"kind\":\"unknown\",\"name\":\"qqq\"}]}");
    const ast_symbols::FileSymbols r = ast_symbols::file_symbols(cb, f.string());
    ASSERT_FALSE(r.from_ast);
    ASSERT_EQ(joined(r.symbols), std::string("c:Kept"));
}

/* Старый хост без поля: слот пуст, обзор собирается сканом, и причина
 * названа (молчащий откат выглядел бы как полный обзор). */
TEST(an_old_host_without_the_field_leaves_a_string_scan_and_says_so) {
    const fs::path t = make_tmp_tree("oldhost");
    const fs::path f = t / "a.php";
    write_file(f, "<?php\nfunction real_one() {}\n");

    HostCallbacks cb;                       /* слота ast_symbols нет */
    const ast_symbols::FileSymbols r = ast_symbols::file_symbols(cb, f.string());
    ASSERT_FALSE(r.from_ast);
    ASSERT_EQ(joined(r.symbols), std::string("f:real_one"));
    ASSERT_TRUE(!r.degrade.empty());
}

/* Хуки читаются строкой ВСЕГДА, даже когда определения пришли из AST: хост
 * отдаёт объявления, а add_action — это вызов. Забыть про это — значит
 * выкинуть из обзора проекта WordPress ровно то, зачем его зовут. */
/*
 * ast:false проверяется НЕЗАВИСИМО от списка. Нынешний хост на ast:false
 * присылает symbols:[] (он инициализирует поле пустым массивом), поэтому
 * проверка «пустой список уводит на скан» маскирует ошибку: код, который
 * поле ast вообще не смотрит, на таком хосте выглядит исправным. Ответ с
 * ast:false И непустым списком — это ответ хоста, который признал, что
 * грамматики нет, и всё же приложил символы; верить им нельзя, потому что
 * они пришли не из разбора. Материал подставлен, но подставлен так, что
 * звать его правдоподобно: без него этот код был бы зелёным на любом хосте,
 * который отвечает честно.
 */
TEST(ast_false_is_believed_nowhere_even_when_the_host_sends_symbols_with_it) {
    const fs::path t = make_tmp_tree("astfalse");
    const fs::path f = t / "a.php";
    write_file(f, "<?php\nfunction real_one() {}\n");

    HostCallbacks cb = host_answering(
        "{\"ok\":1,\"ast\":false,\"language\":\"php\",\"symbols\":["
        "{\"kind\":\"function\",\"name\":\"from_nowhere\"}],"
        "\"reason\":\"грамматики нет\"}");
    const ast_symbols::FileSymbols r = ast_symbols::file_symbols(cb, f.string());
    ASSERT_FALSE(r.from_ast);
    ASSERT_TRUE(joined(r.symbols).find("from_nowhere") == std::string::npos);
    ASSERT_EQ(joined(r.symbols), std::string("f:real_one"));
    ASSERT_TRUE(r.degrade.find("грамматики нет") != std::string::npos);
}

TEST(hooks_are_read_by_string_even_when_symbols_came_from_the_host) {
    const fs::path t = make_tmp_tree("hooks");
    const fs::path f = t / "a.php";
    write_file(f, "<?php\nfunction boot() { add_action('init', 'boot'); }\n"
                  "add_filter('the_content', 'boot');\n"
                  "add_shortcode('acme', 'boot');\n");

    const ast_symbols::FileSymbols r =
        ast_symbols::file_symbols(host_answering(host_said("function", "boot")), f.string());
    ASSERT_TRUE(r.from_ast);
    ASSERT_EQ(joined(r.hooks), std::string("h:init, h:the_content, h:acme"));
}

/*
 * Почему скан НЕ дописывается к AST. На живом материале скан берёт имена из
 * комментариев и строк (f:commented_out из строки комментария,
 * c:NotAString из строкового литерала), и дописывание вернуло бы в обзор
 * ровно то, ради чего уходили в AST. Проверка закрепляет отказ от склейки:
 * разбор ответил — в обзоре ровно его символы.
 */
TEST(the_scan_does_not_add_its_own_guesses_to_the_symbols_of_the_host) {
    const fs::path t = make_tmp_tree("nomerge");
    const fs::path f = t / "a.py";
    write_file(f, "def helper():\n    # function commented_out()\n    return 1\n");

    const ast_symbols::FileSymbols r =
        ast_symbols::file_symbols(host_answering(host_said("function", "helper")), f.string());
    ASSERT_EQ(joined(r.symbols), std::string("f:helper"));
    ASSERT_TRUE(joined(r.symbols).find("commented_out") == std::string::npos);
}

/* --- 3. repo_map: что доходит до модели --- */

static void init_engine_with(const HostCallbacks& cb) {
    HostCallbacks c = cb;
    c.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) {
        return false;
    };
    c.llm_complete = [](const std::string&, const std::string&, std::string&) {
        return false;
    };
    c.llm_is_connected = []() { return false; };
    c.chat_event = [](const std::string&) {};
    Engine::instance().init(c);
    test_support::approve_all_permissions();
    register_base_tools();
    /* Состояние движка живёт между проверками (переиспользуемость), а
     * repo_map спрашивает токен отмены на каждом шаге обхода: без
     * СВЕЖЕГО токена обзор мгновенно сочтётся прерванным. */
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().repo_map_cache_root.clear();
        engine_state().repo_map_cache_result.clear();
        engine_state().turn_abort = std::make_shared<AbortToken>();
    }
}

static std::string run_repo_map(const std::string& root) {
    json::JsonValue args = json::JsonValue::object();
    args.set("root", root);
    return ToolsRegistry::instance().run_output("repo_map", args).output;
}

TEST(repo_map_shows_the_symbols_of_the_host_and_says_how_many_were_scanned) {
    const fs::path t = make_tmp_tree("tool");
    write_file(t / "mod.py", "class Widget:\n    pass\n");
    write_file(t / "plugin.php",
               "<?php\nfunction boot() {}\nadd_action('init', 'boot');\n");

    /* Ответ хоста разный на каждый файл: иначе проверка «деградация названа»
     * прошла бы потому, что AST ответил всегда, и второй файл не отличался
     * бы от первого. */
    HostCallbacks cb;
    cb.ast_symbols = [](const std::string& path, const std::string&) {
        const bool py = path.size() > 3 && path.compare(path.size() - 3, 3, ".py") == 0;
        return py ? host_said("class", "Widget")
                  : "{\"ok\":1,\"ast\":false,\"symbols\":[],\"reason\":\"грамматики нет\"}";
    };
    init_engine_with(cb);

    const std::string out = run_repo_map(t.string());
    ASSERT_TRUE(out.find("c:Widget") != std::string::npos);
    ASSERT_TRUE(out.find("h:init") != std::string::npos);
    /* Модель обязана видеть, что одна строка обзора — из разбора, другая —
     * из скана: иначе «нет символов» читается как «нет кода». */
    ASSERT_TRUE(out.find("строковым сканом") != std::string::npos);
    ASSERT_TRUE(out.find("1 файлов") != std::string::npos);
}

/*
 * Предел на файл (kMaxSymFile) не должен молчать. Проверка требует ТОЧНОГО
 * числа: иначе «ещё 20» и «ещё 0» прошли бы одинаково, то есть проверка
 * была бы зелёной на коде, который обрезает список и не говорит об этом.
 * 60 символов при пределе 40 — остаётся 20.
 */
TEST(repo_map_names_the_over_limit_list_of_a_file_instead_of_dropping_it) {
    std::string body;
    for (int i = 0; i < 60; ++i) body += "class C" + std::to_string(i) + " {}\n";
    const fs::path t = make_tmp_tree("cap");
    write_file(t / "many.cpp", body);

    HostCallbacks cb;
    cb.ast_symbols = [](const std::string&, const std::string&) {
        std::string syms;
        for (int i = 0; i < 60; ++i) {
            if (i) syms += ",";
            syms += "{\"kind\":\"class\",\"name\":\"C" + std::to_string(i) + "\"}";
        }
        return "{\"ok\":1,\"ast\":true,\"symbols\":[" + syms + "]}";
    };
    init_engine_with(cb);

    const std::string out = run_repo_map(t.string());
    ASSERT_TRUE(out.find("ещё 20") != std::string::npos);
    /* И список не длиннее предела плюс одна строка «ещё»: иначе «ещё N»
     * печаталось бы поверх уже обрезанного списка и обрезание было бы
     * незаметно. */
    ASSERT_TRUE(out.find("C39") != std::string::npos);
    ASSERT_TRUE(out.find("C40") == std::string::npos);
}

/*
 * ОБРАТНАЯ сторона честности. Строка о скане обязана уходить и когда
 * разбор ответил на КАЖДЫЙ файл: иначе деградация, которой нет, печаталась
 * бы на каждом обзоре и модель читала бы «часть карты ненадёжна» там, где
 * карта собрана разбором. Обратная проверка не «зеркало» прямой: без неё
 * строка могла бы печататься всегда, и всё было бы зелёным.
 */
TEST(repo_map_says_nothing_about_scanning_when_the_host_parsed_every_file) {
    const fs::path t = make_tmp_tree("noast");
    write_file(t / "a.php", "<?php\nfunction boot() { add_action('init', 'boot'); }\n");
    write_file(t / "b.php", "<?php\nfunction ready() {}\n");

    init_engine_with(host_answering(host_said("function", "boot")));
    const std::string out = run_repo_map(t.string());
    ASSERT_TRUE(out.find("c:boot") == std::string::npos);   /* разбор ответил */
    ASSERT_TRUE(out.find("h:init") != std::string::npos);   /* хуки — всегда скан */
    ASSERT_TRUE(out.find("строковым сканом") == std::string::npos);
}

/*
 * Порядок причин в сводке — по числу файлов, а не по алфавиту: обзор
 * показывает три причины из, скажем, пяти, и первая по алфавиту может
 * оказаться не той, которая объясняет почти весь проект.
 *
 * Материал подстроен так, чтобы порядок по алфавиту и по счётчику
 * ПРОТИВОПОЛОЖНЫ: причина на два файла начинается с «z», причина на один —
 * с «a». Проверка, сортирующая по алфавиту, покажет «a» первой и упадёт.
 */
TEST(repo_map_leads_with_the_reason_that_explains_most_of_the_project) {
    const fs::path t = make_tmp_tree("order");
    write_file(t / "one.php", "<?php\nfunction one() {}\n");
    write_file(t / "two.php", "<?php\nfunction two() {}\n");
    write_file(t / "three.php", "<?php\nfunction three() {}\n");

    HostCallbacks cb;
    cb.ast_symbols = [](const std::string& path, const std::string&) {
        const bool many = path.find("one.php") != std::string::npos
                       || path.find("two.php") != std::string::npos;
        return std::string("{\"ok\":1,\"ast\":false,\"symbols\":[],\"reason\":\"")
             + (many ? "zz_many" : "aa_one") + "\"}";
    };
    init_engine_with(cb);

    const std::string out = run_repo_map(t.string());
    const size_t many = out.find("zz_many");
    const size_t one = out.find("aa_one");
    ASSERT_TRUE(many != std::string::npos);
    ASSERT_TRUE(one != std::string::npos);
    ASSERT_TRUE(many < one);
    ASSERT_TRUE(out.find("zz_many (2 файлов)") != std::string::npos);
    ASSERT_TRUE(out.find("aa_one (1 файлов)") != std::string::npos);
}

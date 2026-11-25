/*
 * test_session_archive.cpp — A0.1: archive_write (RAG-архив сессий).
 *
 * План docs/RAG_SESSION_ARCHIVE_PLAN.md, задача A0.1: инструмент пишет
 * полный текст с локаторами в скоуп `plugin:wp_coder:archive` через
 * rag_index_document_ex, и оригинал находится по `call_id`.
 *
 * Почему проверка именно такая (а не «инструмент есть в каталоге»):
 *   - «есть в реестре» проходило бы и на обработчике, который ничего не
 *     пишет, — ровно тот класс «механизм есть ≠ работает», что уже стоил
 *     двух задач (см. test_tool.cpp, rag_инструменты_исполняются…);
 *   - скоуп проверяется по факту вызова хоста: запись не в тот скоуп
 *     (например, в `plugin:wp_coder` вместо `:archive`) сломала бы
 *     изоляцию RAG v2 (критерий 4 плана), и «нашлось бы» — но не там;
 *   - локатор обязан лежать В ТЕКСТЕ документа, а не только в результате
 *     инструмента: архив ищется по содержимому (A0.2), и «найти по
 *     call_id» без этого было бы враньём.
 *
 * Хост RAG подменён in-memory (как в test_tool.cpp, rag_v2…): rag_index_
 * document_ex читает записанный файл и запоминает содержимое, rag_search_ex
 * ищет по нему подстрокой. Это не «имитация обхода» — обхода здесь нет,
 * а сам инструмент исполняется целиком.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"

using namespace coder;

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

/* Минимальный JSON-ответ поиска: один чанк с данным содержимым. Собираем
 * через json::JsonValue, а не склейкой строк, — в тексте есть переводы
 * строк и кавычки, и ручной JSON развалился бы на экранировании. */
std::string search_hit(const std::string& file_path, const std::string& content) {
    json::JsonValue j = json::JsonValue::object();
    j.set("ok", json::JsonValue(true));
    json::JsonValue chunks = json::JsonValue::array();
    json::JsonValue c = json::JsonValue::object();
    c.set("content", json::JsonValue(content));
    c.set("file_path", json::JsonValue(file_path));
    c.set("start_line", json::JsonValue(1));
    chunks.push_back(c);
    j.set("chunks", chunks);
    return j.dump();
}

} // anonymous namespace

TEST(archive_write_пишет_и_находится_по_call_id) {
    register_rag_tools();
    const fs::path data = fs::temp_directory_path() /
                          ("wp_coder_archive_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(data, ec);
    fs::create_directories(data);
    register_tmp_tree(data.string());

    /* In-memory «стор»: путь → содержимое, снятое с rag_index_document_ex. */
    std::vector<std::pair<std::string, std::string>> docs;
    std::string seen_scope;
    HostCallbacks cb = Engine::instance().callbacks();
    cb.path_data_dir = [&data]() -> std::string { return data.string(); };
    cb.rag_index_document_ex =
        [&](const std::string& scope, const std::string& path,
            const std::string&) -> std::string {
        seen_scope = scope;
        docs.emplace_back(path, read_all(path));
        return std::string(
            R"({"ok":true,"chunks":1,"documents":1,"truncated":false,"reason":""})");
    };
    cb.rag_search_ex = [&docs](const std::string&, const std::string& query,
                               int, const std::string&) -> std::string {
        for (const auto& d : docs) {
            if (d.second.find(query) != std::string::npos)
                return search_hit(d.first, d.second);
        }
        return std::string(R"({"ok":true,"chunks":[]})");
    };
    cb.rag_scope_info = [](const std::string&) {
        return std::string(R"({"ok":true,"ready":true})");
    };
    /* Легаси-слоты намеренно null: _ex-путь на них падать не должен. */
    cb.rag_process_document = nullptr;
    cb.rag_build_prompt = nullptr;
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = data.string();
        engine_state().turn_abort = std::make_shared<AbortToken>();
    }

    const std::string marker = "УНИКАЛЬНЫЙ_МАРКЕР_A0_1_вывод_инструмента";
    const std::string full_text =
        std::string("строка один\n") + marker + "\nстрока три\n";

    json::JsonValue args = json::JsonValue::object();
    args.set("session_id", json::JsonValue("ses_test"));
    args.set("call_id", json::JsonValue("call_42"));
    args.set("text", json::JsonValue(full_text));
    args.set("file", json::JsonValue("src/app.php"));
    args.set("line", json::JsonValue(120));

    const ToolOutput w = ToolsRegistry::instance().run_output("archive_write", args);

    /* 1. Скоуп — именно архив, а не проект: иначе изоляция RAG v2 нарушена. */
    ASSERT_EQ(seen_scope, std::string("plugin:wp_coder:archive"));

    /* 2. В документе лежит ПОЛНЫЙ текст (ничего не обрезано) и ЛОКАТОР
     *    session_id:call_id — по нему и будет искать A0.2. */
    ASSERT_EQ(docs.size(), (size_t)1);
    ASSERT_TRUE(docs[0].second.find(full_text) != std::string::npos);
    ASSERT_TRUE(docs[0].second.find("ses_test:call_42") != std::string::npos);

    /* 3. Результат инструмента несёт локатор (а не только «записано»). */
    ASSERT_TRUE(w.output.find("ses_test:call_42") != std::string::npos);
    /* file:line — из аргументов, а не из метаданных архивного чанка. */
    ASSERT_TRUE(w.output.find("src/app.php:120") != std::string::npos);

    /* 4. «Находится по call_id»: поиск по содержимому архива по call_id
     *    возвращает тот самый полный текст. */
    const std::string found = cb.rag_search_ex("plugin:wp_coder:archive",
                                               "call_42", 5, "");
    ASSERT_TRUE(found.find(marker) != std::string::npos);

    /* 5. Без session_id — честный отказ, а не запись «в никуда». */
    json::JsonValue bad = json::JsonValue::object();
    bad.set("call_id", json::JsonValue("call_43"));
    bad.set("text", json::JsonValue("x"));
    const ToolOutput refused =
        ToolsRegistry::instance().run_output("archive_write", bad);
    ASSERT_TRUE(refused.output.find("session_id") != std::string::npos);
    /* Отказ ничего не записал: в сторе по-прежнему один документ. */
    ASSERT_EQ(docs.size(), (size_t)1);

    /* 6. Старый хост без RAG v2 — внятный отказ, а не тихая потеря. */
    HostCallbacks old = Engine::instance().callbacks();
    old.rag_index_document_ex = nullptr;
    Engine::instance().init(old);
    const ToolOutput no_rag =
        ToolsRegistry::instance().run_output("archive_write", args);
    ASSERT_TRUE(no_rag.output.find("RAG v2 недоступен") != std::string::npos);

    fs::remove_all(data, ec);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir.clear();
    }
}
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
#include "../core/agent_components.h"
#include "../core/tools_registry.h"
#include "../core/compaction.h"

using namespace coder;

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <set>
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


/* A0.2: archive_search возвращает ЛОКАТОРЫ + file:line и находится по
 * содержимому и по call_id. Пишем через настоящий archive_write (не
 * подкладываем документ руками): проверяется связка «записал →
 * нашёл», а не два независимых куска. */
TEST(archive_search_находит_по_содержимому_и_по_call_id) {
    register_rag_tools();
    const fs::path data = fs::temp_directory_path() /
                          ("wp_coder_archive2_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(data, ec);
    fs::create_directories(data);
    register_tmp_tree(data.string());

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
    cb.rag_search_ex = [&docs, &seen_scope](const std::string& scope,
                                            const std::string& query, int,
                                            const std::string&) -> std::string {
        seen_scope = scope;
        for (const auto& d : docs) {
            if (d.second.find(query) != std::string::npos)
                return search_hit(d.first, d.second);
        }
        return std::string(R"({"ok":true,"chunks":[]})");
    };
    cb.rag_scope_info = [](const std::string&) {
        return std::string(R"({"ok":true,"ready":true})");
    };
    cb.rag_process_document = nullptr;
    cb.rag_build_prompt = nullptr;
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = data.string();
        engine_state().turn_abort = std::make_shared<AbortToken>();
    }

    const std::string marker = "УНИКАЛЬНЫЙ_КОНТЕНТ_A0_2_полный_вывод";
    json::JsonValue w = json::JsonValue::object();
    w.set("session_id", json::JsonValue("ses_b"));
    w.set("call_id", json::JsonValue("call_9"));
    w.set("text", json::JsonValue(std::string("начало\n") + marker + "\nконец"));
    w.set("file", json::JsonValue("lib/x.php"));
    w.set("line", json::JsonValue(42));
    ToolsRegistry::instance().run_output("archive_write", w);
    /* После записи в сторе ровно один документ — иначе поиск проверял бы
     * пустоту или чужой документ. */
    ASSERT_EQ(docs.size(), (size_t)1);

    /* 1. По содержимому: возвращается локатор, file:line и сам текст. */
    json::JsonValue q1 = json::JsonValue::object();
    q1.set("query", json::JsonValue(marker));
    const ToolOutput r1 =
        ToolsRegistry::instance().run_output("archive_search", q1);
    ASSERT_EQ(seen_scope, std::string("plugin:wp_coder:archive"));
    ASSERT_TRUE(r1.output.find("ses_b:call_9") != std::string::npos);
    ASSERT_TRUE(r1.output.find("lib/x.php:42") != std::string::npos);
    ASSERT_TRUE(r1.output.find(marker) != std::string::npos);

    /* 2. По call_id: тот же локатор. */
    json::JsonValue q2 = json::JsonValue::object();
    q2.set("query", json::JsonValue("call_9"));
    const ToolOutput r2 =
        ToolsRegistry::instance().run_output("archive_search", q2);
    ASSERT_TRUE(r2.output.find("ses_b:call_9") != std::string::npos);

    /* 3. Пустой результат — честное «пусто», а не молчание. */
    json::JsonValue q3 = json::JsonValue::object();
    q3.set("query", json::JsonValue("слова_которого_нет_в_архиве"));
    const ToolOutput r3 =
        ToolsRegistry::instance().run_output("archive_search", q3);
    ASSERT_TRUE(r3.output.find("пусто") != std::string::npos);

    /* 4. Старый хост без RAG v2 — внятный отказ. */
    HostCallbacks old = Engine::instance().callbacks();
    old.rag_search_ex = nullptr;
    Engine::instance().init(old);
    const ToolOutput no_rag =
        ToolsRegistry::instance().run_output("archive_search", q1);
    ASSERT_TRUE(no_rag.output.find("RAG v2 недоступен") != std::string::npos);

    fs::remove_all(data, ec);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir.clear();
    }
}


/* A1.1: длинный вывод инструмента уходит в архив, а в РЕЗУЛЬТАТЕ остаётся
 * локатор; компакшн не трогается (он видит уже короткий результат).
 *
 * Проверяется СКВОЗЬ цикл агента, а не только функция: «результат
 * инструмента» — это то, что цикл кладёт в историю по call_id, и подмена
 * «полный вывод → локатор» живёт именно там. Хост-имитация отдаёт вызов
 * read_file большого файла, затем финальный ответ; RAG-слоты подменены
 * in-memory (как в A0.1/A0.2). */
TEST(a11_длинный_вывод_уходит_в_архив_а_в_результате_локатор) {
    register_base_tools();
    register_rag_tools();
    test_support::approve_all_permissions();

    const fs::path project = fs::temp_directory_path() /
                             ("wp_coder_a11_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(project, ec);
    fs::create_directories(project);
    register_tmp_tree(project.string());
    /* Вывод `seq 1 1000` (~4 КБ, 1000 строк) — больше порога (2000
     * символов), но меньше предела truncate_output (50 КБ / 2000 строк):
     * иначе вывод обрезал бы ещё И4.10, и проверялась бы не подмена A1.1. */
    std::vector<std::pair<std::string, std::string>> docs;
    std::string seen_scope;
    std::vector<std::string> replies = {
        std::string("Смотрю.\n```json\n{\"tool\": \"bash\"")
            + ", \"command\": \"seq 1 1000\"}\n```",
        std::string("Итог по задаче. ") +
            std::string(200, 'я') + " — готово."};
    size_t next = 0;

    HostCallbacks cb;
    cb.llm_chat = [&replies, &next](const std::string&,
                                    const std::vector<ModelMessage>&,
                                    LlmReply& out) -> bool {
        if (next >= replies.size()) { out.error = "нет ответов"; return false; }
        out.content = replies[next++];
        out.finish_reason = "stop";
        out.prompt_tokens = 100;
        out.completion_tokens = 20;
        return true;
    };
    cb.llm_is_connected = []() { return true; };
    cb.path_data_dir = [&project]() -> std::string { return project.string(); };
    cb.settings_set = [](const std::string&, const std::string&) {};
    cb.settings_get = [](const std::string&, const std::string& d) { return d; };
    cb.chat_event = [](const std::string&) {};
    cb.rag_index_document_ex =
        [&](const std::string& scope, const std::string& path,
            const std::string&) -> std::string {
        seen_scope = scope;
        docs.emplace_back(path, read_all(path));
        return std::string(
            R"({"ok":true,"chunks":1,"documents":1,"truncated":false,"reason":""})");
    };
    cb.rag_search_ex = [](const std::string&, const std::string&, int,
                          const std::string&) -> std::string {
        return std::string(R"({"ok":true,"chunks":[]})");
    };
    cb.rag_scope_info = [](const std::string&) {
        return std::string(R"({"ok":true,"ready":true})");
    };
    Engine::instance().init(cb);

    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().session.clear();
        engine_state().session.push_back(Message::user("прочитай большой файл"));
        engine_state().project_dir = project.string();
        engine_state().mode = 0;
        engine_state().plan_mode = false;
        engine_state().use_planning = false;
        engine_state().max_steps = 6;
        engine_state().session_budget = 60000;
        engine_state().session_id = "ses_a11";
        engine_state().todos.clear();
        engine_state().recent_calls.clear();
        engine_state().allowed_external_paths.clear();
        engine_state().measured_input_tokens = 0;
        engine_state().model_limits = compaction::ModelLimits();
        engine_state().compaction_config = compaction::CompactionConfig();
        engine_state().abort_requested.store(false);
        engine_state().shutting_down = false;
        engine_state().state = AgentState::Executing;
        engine_state().steps = 0;
    }

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    loop.run("системный промпт", response);

    /* 1. Результат инструмента — ЛОКАТОР, а не полный текст. */
    std::vector<Message> h;
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        h = engine_state().session;
    }
    const MessagePart* part = nullptr;
    for (const auto& m : h)
        for (const auto& p : m.parts)
            if (p.is(PartKind::Tool) && p.tool_name() == "bash") part = &p;
    ASSERT_TRUE(part != nullptr);
    ASSERT_TRUE(part->has_result());
    ASSERT_TRUE(part->output().output.find("ses_a11:") != std::string::npos);
    ASSERT_TRUE(part->output().output.find("архив сессий") != std::string::npos);
    /* Полного текста в результате НЕТ — иначе подмена не сработала. */
    ASSERT_TRUE(part->output().output.find("1000") == std::string::npos);

    /* 2. Оригинал — в архиве, целиком (то, что компакшн обрезал бы). */
    ASSERT_EQ(seen_scope, std::string("plugin:wp_coder:archive"));
    ASSERT_EQ(docs.size(), (size_t)1);
    ASSERT_TRUE(docs[0].second.find("1000") != std::string::npos);

    /* 3. Мелкий вывод НЕ архивируется (порог работает). */
    ASSERT_TRUE(archive_output_if_large("ses_a11", "call_small", "коротко").empty());

    fs::remove_all(project, ec);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().session.clear();
        engine_state().project_dir.clear();
        engine_state().session_id.clear();
    }
}


/* A1.2: повторный вызов с тем же call_id НЕ дублирует архив.
 *
 * Дедуп держится на двух вещах: путь документа ключуется call_id
 * (archive_file_path), а хост RAG v2 не индексирует один путь дважды
 * (R1.6, RagStore::index_document). Мок ниже повторяет R1.6 (по пути —
 * «уже проиндексировано, chunks:0»), поэтому тест проверяет именно наш
 * вклад: детерминированный путь по call_id. Разъедься путь (случайный
 * суффикс/счётчик) — два вызова дали бы два документа, и тест упал бы. */
TEST(a12_повторный_вызов_не_дублирует_архив_по_call_id) {
    const fs::path data = fs::temp_directory_path() /
                          ("wp_coder_a12_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(data, ec);
    fs::create_directories(data);
    register_tmp_tree(data.string());

    std::set<std::string> seen_paths;
    std::vector<std::pair<std::string, std::string>> docs;
    HostCallbacks cb = Engine::instance().callbacks();
    cb.path_data_dir = [&data]() -> std::string { return data.string(); };
    cb.rag_index_document_ex =
        [&](const std::string&, const std::string& path,
            const std::string&) -> std::string {
        /* R1.6: один путь — один документ. */
        if (seen_paths.count(path))
            return std::string(
                R"({"ok":true,"chunks":0,"documents":1,"truncated":false,"reason":""})");
        seen_paths.insert(path);
        docs.emplace_back(path, read_all(path));
        return std::string(
            R"({"ok":true,"chunks":1,"documents":1,"truncated":false,"reason":""})");
    };
    Engine::instance().init(cb);
    test_support::approve_all_permissions();

    const std::string big(3000, 'ф');   /* заведомо выше порога 2000 */

    /* Два вызова с ОДНИМ call_id — один документ. */
    const std::string a = archive_output_if_large("ses_d", "call_x", big);
    const std::string b = archive_output_if_large("ses_d", "call_x", big);
    ASSERT_TRUE(!a.empty());
    ASSERT_EQ(a, b);   /* тот же локатор — тот же путь */
    ASSERT_EQ(docs.size(), (size_t)1);

    /* Другой call_id — другой документ (дедуп не склеивает разные вызовы). */
    const std::string c = archive_output_if_large("ses_d", "call_y", big);
    ASSERT_TRUE(!c.empty());
    ASSERT_TRUE(c != a);
    ASSERT_EQ(docs.size(), (size_t)2);

    fs::remove_all(data, ec);
}


/* A2.1: archive_search различает «выключено» и «пусто» (R5.2).
 *
 * Пустой результат — это не «не проиндексирован»: «выключенный RAG»
 * (code=disabled) и «пустой архив» (ready, но ничего не найдено) требуют
 * разных действий агента, и выглядеть одинаково они не должны. */
TEST(a21_archive_search_различает_выключено_и_пусто) {
    register_rag_tools();
    test_support::approve_all_permissions();

    HostCallbacks cb = Engine::instance().callbacks();
    cb.rag_search_ex = [](const std::string&, const std::string&, int,
                          const std::string&) -> std::string {
        return std::string(R"({"ok":true,"chunks":[]})");
    };
    /* 1. Выключено: code=disabled → «выключен». */
    cb.rag_scope_info = [](const std::string&) -> std::string {
        return std::string(
            R"({"ok":false,"code":"disabled","error":"RAG is disabled"})");
    };
    Engine::instance().init(cb);

    json::JsonValue q = json::JsonValue::object();
    q.set("query", json::JsonValue("что-нибудь"));
    const ToolOutput off =
        ToolsRegistry::instance().run_output("archive_search", q);
    ASSERT_TRUE(off.output.find("выключен") != std::string::npos);
    ASSERT_TRUE(off.output.find("пусто") != std::string::npos);

    /* 2. Пусто: RAG готов, но ничего не найдено → «пусто», без «выключен». */
    cb.rag_scope_info = [](const std::string&) -> std::string {
        return std::string(R"({"ok":true,"ready":true,"reason":""})");
    };
    Engine::instance().init(cb);
    const ToolOutput empty =
        ToolsRegistry::instance().run_output("archive_search", q);
    ASSERT_TRUE(empty.output.find("пусто") != std::string::npos);
    ASSERT_TRUE(empty.output.find("выключен") == std::string::npos);
}


/* A3.1: архив переживает рестарт.
 *
 * Персистентность — host R4.4 (RagStore хранит индекс на диске и
 * перезагружает его при старте). Вклад плагина: документ пишется в
 * ПОСТОЯННЫЙ файл под data_dir (не во временный), поэтому после
 * «рестарта» (новый Engine + новый стор, поднятый с того же диска) он
 * на месте и находится. Мок стора здесь перезагружается сканированием
 * архивных файлов — ровно то, что делает хост при старте (R4.4). */
TEST(a31_архив_переживает_рестарт) {
    register_rag_tools();
    test_support::approve_all_permissions();
    const fs::path data = fs::temp_directory_path() /
                          ("wp_coder_a31_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(data, ec);
    fs::create_directories(data);
    register_tmp_tree(data.string());

    const std::string marker = "МАРКЕР_A31_после_рестарта";
    const std::string big = std::string("начало\n") + marker + "\n" +
                            std::string(3000, 'п');

    /* Фаза 1: запись. */
    HostCallbacks cb1;
    cb1.path_data_dir = [&data]() -> std::string { return data.string(); };
    cb1.rag_index_document_ex = [](const std::string&, const std::string&,
                                   const std::string&) -> std::string {
        return std::string(
            R"({"ok":true,"chunks":1,"documents":1,"truncated":false,"reason":""})");
    };
    Engine::instance().init(cb1);
    json::JsonValue w = json::JsonValue::object();
    w.set("session_id", json::JsonValue("ses_p"));
    w.set("call_id", json::JsonValue("call_p"));
    w.set("text", json::JsonValue(big));
    const ToolOutput wr =
        ToolsRegistry::instance().run_output("archive_write", w);
    ASSERT_TRUE(wr.output.find("ses_p:call_p") != std::string::npos);

    /* Файл на диске — постоянный, не временный. */
    const fs::path archive_file =
        data / "wp_coder" / "archive" / "ses_p" / "call_p.txt";
    ASSERT_TRUE(fs::exists(archive_file));
    ASSERT_TRUE(read_all(archive_file).find(marker) != std::string::npos);

    /* Фаза 2: «рестарт» — новый Engine и новый стор, поднятый с диска. */
    HostCallbacks cb2;
    cb2.path_data_dir = [&data]() -> std::string { return data.string(); };
    cb2.rag_search_ex = [&data, &ec](const std::string&, const std::string& query,
                                int, const std::string&) -> std::string {
        /* Перезагрузка индекса с диска (R4.4): ищем по архивным файлам. */
        const fs::path root = data / "wp_coder" / "archive";
        if (fs::exists(root)) {
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file()) continue;
                const std::string body = read_all(it->path());
                if (body.find(query) != std::string::npos)
                    return search_hit(it->path().string(), body);
            }
        }
        return std::string(R"({"ok":true,"chunks":[]})");
    };
    cb2.rag_scope_info = [](const std::string&) {
        return std::string(R"({"ok":true,"ready":true})");
    };
    Engine::instance().init(cb2);

    json::JsonValue q = json::JsonValue::object();
    q.set("query", json::JsonValue("call_p"));
    const ToolOutput found =
        ToolsRegistry::instance().run_output("archive_search", q);
    ASSERT_TRUE(found.output.find("ses_p:call_p") != std::string::npos);
    ASSERT_TRUE(found.output.find(marker) != std::string::npos);

    fs::remove_all(data, ec);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir.clear();
    }
}

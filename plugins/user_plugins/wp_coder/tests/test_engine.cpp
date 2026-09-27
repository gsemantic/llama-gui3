#include "test_framework.h"
#include "test_printers.h"
#include "../core/engine.h"
#include "../core/agent_components.h"
#include "../core/base_tools.h"
#include "../core/git_tools.h"
#include "../core/json.h"
#include "../core/tools_registry.h"
#include "../core/shell.h"
#include "../core/json_utils.h"

#include <map>
#include <fstream>
#include <filesystem>
#include <future>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>

using namespace coder;
namespace fs = std::filesystem;

TEST(engine_parse_action_json) {
    std::string block = "{\"tool\": \"read_file\", \"path\": \"wp-config.php\", \"k\": 10}";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("read_file"));
    ASSERT_EQ(act.path, std::string("wp-config.php"));
    ASSERT_EQ(act.k, 10);
}

TEST(engine_parse_action_json_with_content) {
    std::string block = "{\"tool\": \"write_file\", \"path\": \".env\", "
                        "\"content\": \"DB_PASS=\\\"secret\\\\nvalue\\\"\"}";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("write_file"));
    ASSERT_TRUE(act.content.find("DB_PASS=") != std::string::npos);
}

TEST(engine_parse_action_defaults_k_to_zero) {
    Engine::Action act;
    bool ok = Engine::parse_action("{\"tool\":\"read_file\",\"path\":\"main.py\"}", act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.k, 0);
}

TEST(json_utf8_prefix_does_not_split_codepoint) {
    std::string value(1499, 'a');
    value += "\xE2\x94\x80";
    value += "tail";
    std::string prefix = text::utf8_prefix(value, 1500);

    ASSERT_EQ(prefix.size(), (size_t)1499);
    ASSERT_TRUE(text::is_valid_utf8(prefix));
}

TEST(json_escape_repairs_invalid_utf8) {
    std::string value = "ok";
    value.push_back(static_cast<char>(0xE2));
    std::string escaped = json::escape(value);

    ASSERT_TRUE(text::is_valid_utf8(escaped));
    ASSERT_TRUE(escaped.find("\xEF\xBF\xBD") != std::string::npos);
}

TEST(json_str_decodes_unicode_escape) {
    std::string block = "{\"content\":\"\\u041f\\u0440\\u0438\\u0432\\u0435\\u0442\"}";
    ASSERT_EQ(json::str(block, "content"), std::string("Привет"));
}

TEST(engine_extract_action_json_fenced) {
    std::string text = "Посмотрю файл.\n```json\n{\"tool\": \"read_file\", \"path\": \"a.txt\"}\n```\nДалее...";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(!block.empty());
    ASSERT_TRUE(block.find("\"tool\"") != std::string::npos);
    ASSERT_TRUE(rest.find("Посмотрю файл.") != std::string::npos);
    ASSERT_TRUE(rest.find("Далее...") != std::string::npos);
}

TEST(engine_parse_action_json_fallback_to_wp_action) {
    std::string block = "TOOL: grep_search\nPATTERN: \\d+\n";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("grep_search"));
    ASSERT_EQ(act.pattern, std::string("\\d+"));
}

TEST(engine_extract_action_basic) {
    std::string text = "Some text before\n```\nwp_action\nTOOL: read_file\nPATH: test.php\n```\nText after";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(!block.empty());
    ASSERT_TRUE(block.find("TOOL: read_file") != std::string::npos);
    ASSERT_TRUE(block.find("PATH: test.php") != std::string::npos);
    ASSERT_TRUE(rest.find("Some text before") != std::string::npos);
    ASSERT_TRUE(rest.find("Text after") != std::string::npos);
}

TEST(engine_extract_action_with_fenced) {
    std::string text = "Before\n```wp_action\nTOOL: grep_search\nROOT: /src\nPATTERN: TODO\n```\nAfter";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(!block.empty());
    ASSERT_TRUE(block.find("TOOL: grep_search") != std::string::npos);
}

TEST(engine_extract_action_no_block) {
    std::string text = "Just plain text without any action blocks.";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(block.empty());
}

/* Generic ``` fence с JSON (без "json" суффикса) — модель qwen3-30b
 * часто генерирует именно такой формат:
 *   ```{"tool": "read_file", "path": "main.py"}``` */
TEST(engine_extract_action_generic_fence_json_inline) {
    std::string text = "```{\"tool\": \"read_file\", \"path\": \"main.py\"}```";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(!block.empty());
    ASSERT_TRUE(block.find("\"tool\"") != std::string::npos);
    ASSERT_TRUE(block.find("read_file") != std::string::npos);
    ASSERT_TRUE(block.find("main.py") != std::string::npos);
}

TEST(engine_extract_action_generic_fence_json_multiline) {
    std::string text = "```\n{\"tool\": \"grep_search\", \"root\": \"/src\", \"pattern\": \"TODO\"}\n```";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(!block.empty());
    ASSERT_TRUE(block.find("\"tool\"") != std::string::npos);
    ASSERT_TRUE(block.find("grep_search") != std::string::npos);
}

TEST(engine_extract_action_generic_fence_json_with_surrounding_text) {
    std::string text = "Сейчас проверю файл.\n```{\"tool\": \"read_file\", \"path\": \"main.py\"}```\nГотово.";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(!block.empty());
    ASSERT_TRUE(block.find("\"tool\"") != std::string::npos);
    ASSERT_TRUE(rest.find("Сейчас проверю файл.") != std::string::npos);
    ASSERT_TRUE(rest.find("Готово.") != std::string::npos);
}

TEST(engine_extract_action_generic_fence_non_json) {
    /* Generic ``` fence с НЕ-JSON контентом — не должен извлекать инструмент. */
    std::string text = "```\nprint('hello world')\n```";
    std::string rest;
    std::string block = Engine::extract_action(text, rest);

    ASSERT_TRUE(block.empty());
}

TEST(engine_parse_action_simple) {
    std::string block = "TOOL: read_file\nPATH: wp-config.php\n";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("read_file"));
    ASSERT_EQ(act.path, std::string("wp-config.php"));
}

TEST(engine_parse_action_with_content) {
    std::string block = "TOOL: write_file\nPATH: test.txt\nCONTENT_BEGIN\nHello World\nSecond line\nCONTENT_END\n";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("write_file"));
    ASSERT_EQ(act.path, std::string("test.txt"));
    ASSERT_TRUE(act.content.find("Hello World") != std::string::npos);
    ASSERT_TRUE(act.content.find("Second line") != std::string::npos);
}

TEST(engine_parse_action_all_params) {
    std::string block = "TOOL: some_tool\nPATH: /a/b\nROOT: /c\nQUERY: search term\nPATTERN: \\d+\nCLI: --flag\nURL: http://example.com\nK: 10\n";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_TRUE(ok);
    ASSERT_EQ(act.tool, std::string("some_tool"));
    ASSERT_EQ(act.path, std::string("/a/b"));
    ASSERT_EQ(act.root, std::string("/c"));
    ASSERT_EQ(act.query, std::string("search term"));
    ASSERT_EQ(act.pattern, std::string("\\d+"));
    ASSERT_EQ(act.cli, std::string("--flag"));
    ASSERT_EQ(act.url, std::string("http://example.com"));
    ASSERT_EQ(act.k, 10);
}

TEST(engine_parse_action_empty_tool) {
    std::string block = "PATH: some/path\n";
    Engine::Action act;
    bool ok = Engine::parse_action(block, act);

    ASSERT_FALSE(ok);
}

TEST(engine_build_system_prompt) {
    auto& eng = Engine::instance();
    std::string prompt = eng.build_system_prompt();
    ASSERT_TRUE(!prompt.empty());
    ASSERT_TRUE(prompt.find("инструмент") != std::string::npos);
}

TEST(engine_system_prompt_stable_across_steps) {
    auto& eng = Engine::instance();
    /* C1: префикс системного промпта должен оставаться стабильным между шагами
     * (поставщик кэширует его). Повторный вызов без инвалидации — тот же текст. */
    eng.invalidate_prompt_cache();  // сбрасываем кэш
    std::string p1 = eng.build_system_prompt();
    std::string p2 = eng.build_system_prompt();
    ASSERT_TRUE(p1 == p2);
    /* План-режим пометить, что промпт стабилен даже при повторных вызовах. */
    eng.invalidate_prompt_cache();
    std::string p3 = eng.build_system_prompt();
    ASSERT_TRUE(p1 == p3);
}

/* ======================================================================
 * И5.7: сжатие истории сообщений
 *
 * Проверяется не «длина упала», а ЧТО именно упало. Старый код заменял
 * целые сообщения строками-заглушками, а старую реплику ассистента обрубал
 * до 60 символов — вместе с ВЫЗОВОМ, который её породил. Тогда
 * «RESULT [read_file]: …» оставалось в истории без вызова, и модель
 * читала результат, не зная, что его вызвало. Поэтому проверка здесь
 * трёх вещей: объём упал, вызов УЦЕЛ, результат сжат.
 * ====================================================================== */

namespace {

/* Ход с вызовом инструмента и большим результатом. */
Message turn_with_tool_result(const std::string& text,
                              const std::string& result_body) {
    Message m = Message::assistant("msg_parent");
    m.parts.push_back(MessagePart::text(text));
    MessagePart part = MessagePart::tool("call_" + text, "read_file",
                                         json::JsonValue::object());
    ToolOutput out;
    out.title = "read a.txt";
    out.output = result_body;
    part.set_result(out);
    m.parts.push_back(std::move(part));
    return m;
}

/* История, гарантированно превышающая бюджет. */
void fill_over_budget_history(Engine& eng, size_t budget) {
    std::lock_guard<std::mutex> lk(eng.state().mtx);
    eng.state().session_budget = budget;
    eng.state().session.clear();
    eng.state().session.push_back(Message::user("исходная задача"));
    for (int i = 0; i < 60; ++i) {
        eng.state().session.push_back(
            turn_with_tool_result("рассуждение " + std::to_string(i),
                                 std::string(2000, 'y')));
    }
}

}  // namespace

TEST(compress_history_keeps_the_call_and_shrinks_the_result) {
    std::vector<Message> history;
    history.push_back(Message::user("задача"));
    for (int i = 0; i < 60; ++i) {
        history.push_back(turn_with_tool_result("ход " + std::to_string(i),
                                                std::string(2000, 'y')));
    }
    const size_t before = model_history_chars(history);
    ASSERT_TRUE(before > 60000);

    compress_history(history, 60000);
    const size_t after = model_history_chars(history);
    ASSERT_TRUE(after <= 60000);
    ASSERT_TRUE(after < before);

    /* Вызов уцелел: без него результат в транскрипте осиротел бы. */
    const MessagePart* part = nullptr;
    for (const Message& m : history) {
        if (const MessagePart* p = find_tool_part(m, "call_ход 0")) part = p;
    }
    ASSERT_TRUE(part != nullptr);
    ASSERT_EQ(part->tool_name(), std::string("read_file"));
    ASSERT_TRUE(part->has_result());
    ASSERT_TRUE(part->output().output.find("сжат") != std::string::npos);
}

TEST(compress_history_keeps_the_first_and_the_last_turn) {
    std::vector<Message> history;
    history.push_back(Message::user(std::string(2000, 'z')));
    for (int i = 0; i < 60; ++i) {
        history.push_back(
            turn_with_tool_result("ход " + std::to_string(i),
                                 std::string(2000, 'y')));
    }
    compress_history(history, 60000);
    /* Задача пользователя и последний ход — то, над чем модель работает
     * прямо сейчас, — не трогаются. Сжатая задача отняла бы у неё цель. */
    ASSERT_EQ(history.front().text().size(), (size_t)2000);
    const size_t last_body =
        find_tool_part(history.back(), "call_ход 59")->output().output.size();
    ASSERT_EQ(last_body, (size_t)2000);
}

TEST(compress_history_under_budget_is_a_no_op) {
    std::vector<Message> history;
    history.push_back(Message::user("маленькая задача"));
    history.push_back(Message::assistant(""));
    history.back().parts.push_back(MessagePart::text("короткий ответ"));
    const std::string before = history.back().text();
    compress_history(history, 60000);
    ASSERT_EQ(history.size(), (size_t)2);
    ASSERT_EQ(history.front().text(), std::string("маленькая задача"));
    ASSERT_EQ(history.back().text(), before);
}

TEST(compress_history_reports_progress_in_the_model_transcript) {
    /* Объём считается по ТРАНСКРИПТУ для модели: именно он уходит
     * провайдеру, и именно он ограничен бюджетом. */
    std::vector<Message> history;
    history.push_back(Message::user("задача"));
    history.push_back(turn_with_tool_result("ход", std::string(500, 'q')));
    /* Заголовок ToolOutput в транскрипт не попадает, а вызов и «RESULT [x]:»
     * попадают — отсюда и неравенство. */
    ASSERT_TRUE(model_history_chars(history) > 500);
}

/* ======================================================================
 * Регрессии: И0.1/D1 — дедлок сжатия истории
 *
 * state_.mtx — нерекурсивный std::mutex. Раньше AgentLoop::run брал его
 * вокруг блока «посчитать размер сессии», а внутри вызывал trim(), который
 * берёт тот же мьютекс. При превышении session_budget воркер-тред вставал
 * намертво, а UI (он читает то же состояние под тем же мьютексом) — зависал.
 *
 * Тесты ниже воспроизводят ровно этот сценарий: история длиннее бюджета +
 * вызов сжатия без внешнего лока. Если логика вернётся к «захватить лок
 * снаружи», тесты провалятся по таймауту. И5.7 класс SessionStore убран,
 * и проверка осталась прежней — суть дедлока не изменилась, изменилось
 * лишь то, что сжимается (сообщения, а не строки).
 * ====================================================================== */

TEST(history_compression_completes_over_budget) {
    auto& eng = Engine::instance();
    const size_t budget = 60000;
    fill_over_budget_history(eng, budget);

    size_t before = 0;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        before = model_history_chars(eng.state().session);
    }
    ASSERT_TRUE(before > budget);

    /* Тот самый вызов, который раньше вешался. Запускаем в отдельном
     * потоке с таймаутом, чтобы дедлок валил тест, а не весь прогон. */
    auto fut = std::async(std::launch::async, [&eng] {
        eng.trim_history_if_needed();
    });
    ASSERT_TRUE(fut.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    fut.get();

    std::lock_guard<std::mutex> lk(eng.state().mtx);
    ASSERT_TRUE(model_history_chars(eng.state().session) <= budget);
}

TEST(history_compression_is_reentrant_safe_from_loop_pattern) {
    auto& eng = Engine::instance();
    fill_over_budget_history(eng, 40000);

    /* Имитируем точный паттерн AgentLoop::run: сжатие одним вызовом,
     * без захвата state_.mtx снаружи. */
    auto fut = std::async(std::launch::async, [&eng] {
        for (int step = 0; step < 5; ++step) {
            eng.trim_history_if_needed();
        }
    });
    ASSERT_TRUE(fut.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    fut.get();
}

TEST(history_compression_does_not_block_ui_reader) {
    auto& eng = Engine::instance();
    const size_t budget = 60000;
    fill_over_budget_history(eng, budget);

    std::atomic<bool> ui_stuck{false};
    std::atomic<bool> stop{false};

    /* «UI-поток»: читает состояние под state_.mtx каждый кадр. */
    std::thread ui([&] {
        while (!stop.load()) {
            {
                std::lock_guard<std::mutex> lk(eng.state().mtx);
                volatile size_t n = eng.state().session.size();
                (void)n;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    /* «Воркер»: сжимает историю. Если trim() ждёт мьютект, который держит
     * вызывающий код, — воркер зависнет и перестанет освобождать лок,
     * после чего застрянет и «UI». */
    std::atomic<bool> worker_done{false};
    std::thread worker([&] {
        for (int i = 0; i < 20; ++i) eng.trim_history_if_needed();
        worker_done = true;
    });

    auto t0 = std::chrono::steady_clock::now();
    while (!worker_done.load() &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ui_stuck = !worker_done.load();

    stop = true;
    ui.join();
    if (!worker_done.load()) {
        /* Не держим мёртвый поток — тест просто провалится. */
        worker.detach();
    } else {
        worker.join();
    }
    ASSERT_FALSE(ui_stuck);
}

TEST(history_compression_within_budget_changes_nothing) {
    auto& eng = Engine::instance();
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().session_budget = 60000;
        eng.state().session.clear();
        eng.state().session.push_back(Message::user("маленькая задача"));
        Message answer = Message::assistant("msg_0");
        answer.parts.push_back(MessagePart::text("короткий ответ"));
        eng.state().session.push_back(answer);
    }
    size_t before = 0;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        before = model_history_chars(eng.state().session);
    }

    /* Сжатие в пределах бюджета — no-op, ничего не портит. */
    eng.trim_history_if_needed();
    ASSERT_EQ(eng.session_for_test().size(), (size_t)2);
    ASSERT_EQ(eng.session_for_test()[0].text(), std::string("маленькая задача"));
    std::lock_guard<std::mutex> lk(eng.state().mtx);
    ASSERT_EQ(model_history_chars(eng.state().session), before);
}

TEST(engine_settings_deploy_remote_dir_roundtrip) {
    std::map<std::string, std::string> settings;
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.path_data_dir = []() { return std::string(); };
    cb.path_config_dir = []() { return std::string(); };
    cb.settings_get = [&](const std::string& key, const std::string& def) -> std::string {
        auto it = settings.find(key);
        return it != settings.end() ? it->second : def;
    };
    cb.settings_set = [&](const std::string& key, const std::string& value) {
        settings[key] = value;
    };
    cb.chat_event = [](const std::string&) {};

    auto& eng = Engine::instance();
    eng.init(cb);

    /* Сохраняем путь деплоя. */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().deploy_remote_dir = "/var/www/remote";
    }
    eng.save_settings();

    /* Очищаем поле и перезагружаем настройки. */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().deploy_remote_dir.clear();
    }
    eng.load_settings();

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().deploy_remote_dir, std::string("/var/www/remote"));
    }
}

TEST(engine_fsm_state_transitions) {
    std::map<std::string, std::string> settings;
    HostCallbacks cb;
    cb.settings_get = [&](const std::string& key, const std::string& def) -> std::string {
        auto it = settings.find(key);
        return it != settings.end() ? it->second : def;
    };
    cb.settings_set = [&](const std::string& key, const std::string& value) {
        settings[key] = value;
    };
    cb.chat_event = [](const std::string&) {};

    auto& eng = Engine::instance();
    eng.init(cb);

    /* По умолчанию — Idle. */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ((int)eng.state().state, (int)AgentState::Idle);
    }

    /* Каждый реальный переход публикует observer-событие. */
    size_t events_before;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        events_before = eng.state().events.size();
    }
    eng.set_state(AgentState::Planning);
    eng.set_state(AgentState::Executing);
    eng.set_state(AgentState::WaitingPermission);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ((int)eng.state().state, (int)AgentState::WaitingPermission);
        ASSERT_TRUE(eng.state().events.size() >= events_before + 3);
    }

    /* Имена состояний (human-readable). */
    ASSERT_EQ(std::string(agent_state_name(AgentState::Idle)), std::string("Idle"));
    ASSERT_EQ(std::string(agent_state_name(AgentState::Planning)), std::string("План"));
    ASSERT_EQ(std::string(agent_state_name(AgentState::WaitingPermission)),
              std::string("Ожидание разрешения"));
    ASSERT_EQ(std::string(agent_state_name(AgentState::Aborted)), std::string("Прервано"));

    /* Повторный переход в то же состояние не спамит событие. */
    size_t events_now;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        events_now = eng.state().events.size();
    }
    eng.set_state(AgentState::WaitingPermission);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().events.size(), events_now);
    }
}

/* ======================================================================
 * Фаза 3: list / edit_file / undo_edit / web_fetch / git_*
 * ====================================================================== */

static void init_tools_for_phase3(const fs::path& project) {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = project.string();
    }
    register_base_tools();
    register_git_tools();
}

static fs::path make_tmp_project() {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_p3_" + std::to_string(::getpid()) + "_" + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

TEST(read_file_preserves_utf8_at_limit) {
    fs::path tmp = make_tmp_project();
    {
        std::ofstream f(tmp / "boundary.txt", std::ios::binary);
        f << std::string(11999, 'a') << "\xE2\x94\x80" << "tail";
    }
    init_tools_for_phase3(tmp);

    ToolArgs a;
    a.path = "boundary.txt";
    a.k = 0;
    std::string result = ToolsRegistry::instance().run("read_file", a);

    ASSERT_TRUE(text::is_valid_utf8(result));
    fs::remove_all(tmp);
}

TEST(list_lists_files_and_dirs) {
    fs::path tmp = make_tmp_project();
    {
        std::ofstream f(tmp / "alpha.txt"); f << "x";
        std::ofstream f2(tmp / "beta.py");  f2 << "y";
        fs::create_directory(tmp / "subdir");
    }
    init_tools_for_phase3(tmp);

    ToolArgs a;
    a.path = tmp.string();
    std::string r = ToolsRegistry::instance().run("list", a);
    ASSERT_TRUE(r.find("alpha.txt") != std::string::npos);
    ASSERT_TRUE(r.find("beta.py") != std::string::npos);
    ASSERT_TRUE(r.find("subdir/") != std::string::npos);
    fs::remove_all(tmp);
}

TEST(web_fetch_empty_url_rejected) {
    fs::path tmp = make_tmp_project();
    init_tools_for_phase3(tmp);
    /* И1.6: url обязателен — отказ приходит из валидации схемы. */
    ToolArgs a;
    std::string r = ToolsRegistry::instance().run("web_fetch", a);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(r.find("url") != std::string::npos);
    /* Явно переданный пустой url доходит до обработчика. */
    json::JsonValue args = json::JsonValue::object();
    args.set("url", "");
    std::string r2 = ToolsRegistry::instance().run("web_fetch", args);
    ASSERT_TRUE(r2.find("пустой url") != std::string::npos);
    fs::remove_all(tmp);
}

TEST(edit_file_replaces_line_range) {
    fs::path tmp = make_tmp_project();
    {
        std::ofstream f(tmp / "e.txt");
        f << "alpha\nbeta\ngamma\ndelta\n";
    }
    init_tools_for_phase3(tmp);

    ToolArgs a;
    a.path = "e.txt";
    a.k = 2;      // строка 2 (1-based)
    a.query = "3"; // до строки 3 включительно
    a.content = "BETA2\nGAMMA2";  // две новые строки
    std::string r = ToolsRegistry::instance().run("edit_file", a);
    ASSERT_TRUE(r.find("[edit_file]") != std::string::npos);
    ASSERT_TRUE(r.find("2-3") != std::string::npos);

    std::ifstream fin(tmp / "e.txt");
    std::string content((std::istreambuf_iterator<char>(fin)),
                        std::istreambuf_iterator<char>());
    ASSERT_TRUE(content.find("alpha\nBETA2\nGAMMA2\ndelta\n") != std::string::npos);
    fs::remove_all(tmp);
}

TEST(edit_file_out_of_range_rejected) {
    fs::path tmp = make_tmp_project();
    {
        std::ofstream f(tmp / "e.txt");
        f << "one\ntwo\n";
    }
    init_tools_for_phase3(tmp);

    ToolArgs a;
    a.path = "e.txt";
    a.k = 10;  // за пределами файла
    a.content = "x";  // content обязателен по схеме (И1.6)
    std::string r = ToolsRegistry::instance().run("edit_file", a);
    ASSERT_TRUE(r.find("диапазон строк вне файла") != std::string::npos);
    fs::remove_all(tmp);
}

TEST(undo_edit_restores_backup) {
    fs::path tmp = make_tmp_project();
    {
        std::ofstream f(tmp / "u.txt");
        f << "old-content";
    }
    init_tools_for_phase3(tmp);

    /* write_file создаёт .orig-backup (3.5). */
    ToolArgs w;
    w.path = "u.txt";
    w.content = "new-content";
    std::string wr = ToolsRegistry::instance().run("write_file", w);
    ASSERT_TRUE(wr.find("[записано]") != std::string::npos);
    ASSERT_TRUE(fs::exists(tmp / "u.txt.orig"));

    /* undo_edit восстанавливает старую версию. */
    ToolArgs u;
    u.path = "u.txt";
    std::string ur = ToolsRegistry::instance().run("undo_edit", u);
    ASSERT_TRUE(ur.find("[undo_edit]") != std::string::npos);

    std::ifstream fin(tmp / "u.txt");
    std::string content((std::istreambuf_iterator<char>(fin)),
                        std::istreambuf_iterator<char>());
    ASSERT_EQ(content, std::string("old-content"));
    fs::remove_all(tmp);
}

TEST(git_tools_require_project_dir) {
    fs::path tmp = make_tmp_project();
    init_tools_for_phase3(tmp);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir.clear();
    }
    ToolArgs a;
    std::string r = ToolsRegistry::instance().run("git_add", a);
    ASSERT_TRUE(r.find("не задан project_dir") != std::string::npos);

    ToolArgs b;
    std::string r2 = ToolsRegistry::instance().run("git_branch", b);
    ASSERT_TRUE(r2.find("не задан project_dir") != std::string::npos);
    fs::remove_all(tmp);
}

TEST(git_checkout_requires_branch_name) {
    fs::path tmp = make_tmp_project();
    init_tools_for_phase3(tmp);
    /* И1.6: query обязателен по схеме — пустой вызов отсекается валидацией. */
    ToolArgs a;  // query пуст
    std::string r = ToolsRegistry::instance().run("git_checkout", a);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    /* Явно переданный пустой query доходит до обработчика. */
    json::JsonValue args = json::JsonValue::object();
    args.set("query", "");
    std::string r2 = ToolsRegistry::instance().run("git_checkout", args);
    ASSERT_TRUE(r2.find("укажи ветку") != std::string::npos);
    fs::remove_all(tmp);
}

TEST(git_tools_work_in_real_repo) {
    /* Регрессия: git-команды падали с «timeout: failed to run 'cd'»,
     * т.к. команда строилась через «cd ... && git ...», а timeout(1)
     * не понимает встроенные команды shell. Исправлено: git -C <dir>. */
    fs::path tmp = make_tmp_project();
    /* Инициализируем реальный git-репозиторий. */
    {
        std::string out;
        int rc = -1;
        bool ok1 = shell::run_capture_status(
            "git -C " + tmp.string() + " init", out, rc, 30);
        ASSERT_TRUE(ok1 || rc == 0);  // git собран и каталог инициализируется
        /* Identity для commit (в тесте git не знает user). */
        shell::run_capture_status("git -C " + tmp.string() + " config user.email test@example.com", out, rc, 10);
        shell::run_capture_status("git -C " + tmp.string() + " config user.name test", out, rc, 10);
    }
    init_tools_for_phase3(tmp);

    ToolArgs a;
    std::string r = ToolsRegistry::instance().run("git_status", a);
    /* Не должно быть ошибки «cd» — только нормальный git-вывод от состояния.
     * Проверяем САМУ ошибку регрессии, а не подстроку «cd»: git печатает
     * хеш коммита, и примерно в 2 % прогонов он начинается с «cd»
     * (например 3fcd812) — такой тест падал бы по случайной причине и
     * учил бы игнорировать красное. */
    ASSERT_TRUE(r.find("failed to run 'cd'") == std::string::npos);
    ASSERT_TRUE(r.find("git status") != std::string::npos);

    /* git_add + git_commit работают в реальном репо. */
    {
        std::ofstream f(tmp / "a.txt"); f << "x";
    }
    ToolArgs add;
    add.path = "a.txt";
    std::string ra = ToolsRegistry::instance().run("git_add", add);
    ASSERT_TRUE(ra.find("[git add]") != std::string::npos);

    ToolArgs cm;
    cm.query = "test commit";
    std::string rc2 = ToolsRegistry::instance().run("git_commit", cm);
    ASSERT_TRUE(rc2.find("failed to run 'cd'") == std::string::npos);
    ASSERT_TRUE(rc2.find("test commit") != std::string::npos);

    ToolArgs log;
    log.k = 3;
    std::string rl = ToolsRegistry::instance().run("git_log", log);
    ASSERT_TRUE(rl.find("test commit") != std::string::npos);

    fs::remove_all(tmp);
}

/* ======================================================================
 * Фаза 5: resume сессии (5.2), настройки агента (5.3)
 * ====================================================================== */

TEST(save_load_session_roundtrip) {
    fs::path tmp = make_tmp_project();
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    cb.path_data_dir = [&tmp]() -> std::string { return tmp.string(); };

    auto& eng = Engine::instance();
    eng.init(cb);

    {
        /* Синглтон: чистим сессию от предыдущих тестов. */
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().session.clear();
        eng.state().session.push_back(Message::user("привет"));
        Message answer = Message::assistant("");
        answer.parts.push_back(MessagePart::text("привет!\n\"quoted\""));
        eng.state().session.push_back(answer);
    }
    eng.save_session();
    /* И5.6: файл теперь <data_dir>/wp_coder/sessions/<session_id>.json.
     * Старый единый session.json больше не читается и не пишется —
     * формат истории изменился целиком (части, parent_id, состояния). */
    {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(tmp / "wp_coder" / "sessions")) {
            if (e.path().extension() == ".json") files.push_back(e.path());
        }
        ASSERT_EQ(files.size(), (size_t)1);
        ASSERT_TRUE(files[0].filename().string().rfind("ses_", 0) == 0);
    }

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().session.clear();
    }
    eng.load_session();

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().session.size(), (size_t)2);
        ASSERT_EQ(eng.state().session[0].role, std::string("user"));
        ASSERT_EQ(eng.state().session[0].text(), std::string("привет"));
        /* Экранирование кавычек/переводов строк переживает roundtrip. */
        ASSERT_EQ(eng.state().session[1].text(), std::string("привет!\n\"quoted\""));
        /* И5.7: round-trip больше не разбирает историю в строки, поэтому
         * переживает и СТРУКТУРА: части, идентификаторы, родитель. */
        ASSERT_TRUE(!eng.state().session[0].id.empty());
        ASSERT_EQ(eng.state().session[1].parts.size(), (size_t)1);
        ASSERT_TRUE(eng.state().session[1].parts[0].is(PartKind::Text));
    }

    /* clear_session удаляет и файл на диске. */
    eng.clear_session();
    {
        std::vector<fs::path> files;
        std::error_code ec;
        if (fs::is_directory(tmp / "wp_coder" / "sessions", ec)) {
            for (const auto& e : fs::directory_iterator(tmp / "wp_coder" / "sessions")) {
                if (e.path().extension() == ".json") files.push_back(e.path());
            }
        }
        ASSERT_EQ(files.size(), (size_t)0);
    }

    fs::remove_all(tmp);
}

TEST(load_session_empty_when_no_file) {
    fs::path tmp = make_tmp_project();
    HostCallbacks cb;
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    cb.path_data_dir = [&tmp]() -> std::string { return tmp.string(); };
    auto& eng = Engine::instance();
    eng.init(cb);

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().session.clear();
    }
    eng.load_session();  // файла нет — ничего не должно сломаться
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_TRUE(eng.state().session.empty());
    }
    fs::remove_all(tmp);
}

/* И0.5 / D10 — сессия с '}' и '\"' внутри содержимого.
 *
 * Старый load_session искал "{\"role\"" и брал содержимое до ПЕРВОГО '}'.
 * В коде '}' встречается на каждой второй строке (function f() {}),
 * поэтому resume молча обрезал сообщения. */
TEST(save_load_session_survives_braces_and_escapes) {
    fs::path tmp = make_tmp_project();
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    cb.path_data_dir = [&tmp]() -> std::string { return tmp.string(); };

    auto& eng = Engine::instance();
    eng.init(cb);

    const std::string tricky =
        "<?php\n"
        "function wp_demo() {\n"
        "    $obj = ['a' => 1];\n"
        "    if ($x) { return ['ok' => true]; }\n"
        "}\n"
        "/* \"кавычки\" и \\ обратный слэш */\n"
        "wp_json_encode(array(1, 2, 3));\n";
    const std::string plan = "[ПЛАН]\n1. Создать класс\n2. Хук add_action('init', ...)\n";

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().session.clear();
        eng.state().session.push_back(Message::user("напиши функцию"));
        /* Ход с вызовом инструмента: раньше вызов и его результат были
         * двумя СТРОКАМИ, и склейка «RESULT [x]:» была единственным
         * признаком принадлежности (её можно было подделать). */
        Message turn = Message::assistant(eng.state().session.back().id);
        turn.parts.push_back(MessagePart::text(tricky));
        json::JsonValue args = json::JsonValue::object();
        args.set("path", "wp_demo.php");
        args.set("content", tricky);
        MessagePart call = MessagePart::tool(
            "call_0", "write_file", args,
            "{\"tool\": \"write_file\", \"path\": \"wp_demo.php\"}");
        ToolOutput out;
        out.title = "write wp_demo.php";
        out.output = "Записано " + std::to_string(tricky.size()) + " байт";
        call.set_result(out);
        turn.parts.push_back(std::move(call));
        eng.state().session.push_back(turn);

        Message plan_msg = Message::assistant(turn.id);
        plan_msg.parts.push_back(MessagePart::text(plan));
        eng.state().session.push_back(plan_msg);
    }
    eng.save_session();
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().session.clear();
    }
    eng.load_session();

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        const std::vector<Message>& h = eng.state().session;
        ASSERT_EQ(h.size(), (size_t)3);
        ASSERT_EQ(h[1].text(), tricky);
        ASSERT_EQ(h[2].text(), plan);
        /* Вызов пережил round-trip целиком: имя, аргументы, сырой блок и
         * состояние. Раньше всё это терялось — история разбиралась в строки
         * (отклонение 35), и результат возвращался в модель отдельной
         * репликой без единого следа о том, что он вызвал. */
        const MessagePart* call = find_tool_part(h[1], "call_0");
        ASSERT_TRUE(call != nullptr);
        ASSERT_EQ(call->tool_name(), std::string("write_file"));
        ASSERT_EQ(call->args().get_string("content"), tricky);
        ASSERT_EQ(call->state(), ToolState::Completed);
        ASSERT_EQ(call->output().title, std::string("write wp_demo.php"));
        ASSERT_TRUE(call->raw_call().find("wp_demo.php") != std::string::npos);
        /* Идентификаторы и родитель — тоже: по ним UI (И11) строит дерево. */
        ASSERT_EQ(h[1].parent_id, h[0].id);
        ASSERT_EQ(h[2].parent_id, h[1].id);
        /* Транскрипт для модели содержит и вызов, и его результат. */
        const std::vector<ModelMessage> model = to_model_messages(h);
        ASSERT_TRUE(model.size() >= 3);
        std::string transcript;
        for (const ModelMessage& m : model) transcript += m.content + "\n";
        ASSERT_TRUE(transcript.find("RESULT [write_file]:") != std::string::npos);
    }

    eng.clear_session();
    fs::remove_all(tmp);
}

/* Битый файл сессии не должен ронять загрузку — и не должен молча
 * превращаться в пустую сессию с последующей перезаписью. */
TEST(load_session_rejects_corrupt_file) {
    fs::path tmp = make_tmp_project();
    HostCallbacks cb;
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    cb.path_data_dir = [&tmp]() -> std::string { return tmp.string(); };
    auto& eng = Engine::instance();
    eng.init(cb);

    /* И5.6: путь сессии — <data_dir>/wp_coder/sessions/<id>.json, и текущая
     * сессия ищется там по самому свежему файлу. */
    fs::create_directories(tmp / "wp_coder" / "sessions");
    fs::path bad = tmp / "wp_coder" / "sessions" / "ses_000000000099.json";
    {
        /* Обрезанный JSON: строковый литерал не закрыт. */
        std::ofstream f(bad, std::ios::binary | std::ios::trunc);
        f << "{\"version\":1,\"messages\":[{\"role\":\"user\","
             "\"content\":\"незакрытая строка}]";
    }
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().session.clear();
    }
    eng.load_session();
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_TRUE(eng.state().session.empty());
    }
    /* Повреждённый файл убран, чтобы он не ломал следующий запуск. */
    ASSERT_FALSE(fs::exists(bad));

    fs::remove_all(tmp);
}

/* Юнит-тесты самого разборщика (core/json_utils.h). */
TEST(json_parse_message_array_handles_braces) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string s =
        R"([{"role":"user","content":"a}b{c"},{"role":"assistant","content":"d\"e\\f\n"}])";
    ASSERT_TRUE(json::parse_message_array(s, out));
    ASSERT_EQ(out.size(), (size_t)2);
    ASSERT_EQ(out[0].first, std::string("user"));
    ASSERT_EQ(out[0].second, std::string("a}b{c"));
    ASSERT_EQ(out[1].first, std::string("assistant"));
    ASSERT_EQ(out[1].second, std::string("d\"e\\f\n"));
}

TEST(json_parse_message_array_empty_and_invalid) {
    std::vector<std::pair<std::string, std::string>> out;
    ASSERT_TRUE(json::parse_message_array("[]", out));
    ASSERT_TRUE(out.empty());
    ASSERT_TRUE(json::parse_message_array("  [ ]  ", out));
    ASSERT_TRUE(out.empty());
    ASSERT_FALSE(json::parse_message_array("", out));
    ASSERT_FALSE(json::parse_message_array("{}", out));
    ASSERT_FALSE(json::parse_message_array(R"([{"role":"user"}])", out));
    ASSERT_FALSE(json::parse_message_array(R"([{"role":1}])", out));
    ASSERT_FALSE(json::parse_message_array(R"([{"role":"user","content":"x")", out));
}

TEST(json_parse_message_array_unicode_escapes) {
    std::vector<std::pair<std::string, std::string>> out;
    /* \uD83D\uDE00 — сурогатная пара эмодзи (обычная запись JSON). */
    std::string s =
        R"([{"role":"user","content":"\u041f\u0440\u0438\u0432\u0435\u0442 \ud83d\ude00"}])";
    ASSERT_TRUE(json::parse_message_array(s, out));
    ASSERT_EQ(out.size(), (size_t)1);
    ASSERT_TRUE(out[0].second.find("Привет") != std::string::npos);
    ASSERT_TRUE(out[0].second.find("\xF0\x9F\x98\x80") != std::string::npos);
    /* Одиночный сурогат не должен ломать разбор — подменяется на U+FFFD. */
    std::vector<std::pair<std::string, std::string>> out2;
    ASSERT_TRUE(json::parse_message_array(
        R"([{"role":"user","content":"\ud83d"}])", out2));
    ASSERT_EQ(out2.size(), (size_t)1);
    ASSERT_TRUE(out2[0].second.find("\xEF\xBF\xBD") != std::string::npos);
}

TEST(agent_settings_max_steps_and_budget) {
    std::map<std::string, std::string> settings;
    settings["wp_coder.max_steps"] = "21";
    settings["wp_coder.session_budget"] = "16384";
    HostCallbacks cb;
    cb.settings_get = [&](const std::string& k, const std::string& d) -> std::string {
        auto it = settings.find(k);
        return it != settings.end() ? it->second : d;
    };
    cb.settings_set = [&](const std::string& k, const std::string& v) { settings[k] = v; };
    cb.chat_event = [](const std::string&) {};

    auto& eng = Engine::instance();
    eng.init(cb);  // вызывает load_settings()

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().max_steps, 21);
        ASSERT_EQ(eng.state().session_budget, (size_t)16384);
    }
}

TEST(agent_settings_defaults_on_missing) {
    std::map<std::string, std::string> settings;  // пусто
    HostCallbacks cb;
    cb.settings_get = [&](const std::string& k, const std::string& d) -> std::string {
        auto it = settings.find(k);
        return it != settings.end() ? it->second : d;
    };
    cb.settings_set = [&](const std::string& k, const std::string& v) { settings[k] = v; };
    cb.chat_event = [](const std::string&) {};

    auto& eng = Engine::instance();
    eng.init(cb);

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().max_steps, 12);
        ASSERT_EQ(eng.state().session_budget, (size_t)60000);
    }
}

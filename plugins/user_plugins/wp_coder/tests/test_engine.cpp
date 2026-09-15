#include "test_framework.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/git_tools.h"
#include "../core/tools_registry.h"
#include "../core/shell.h"

#include <map>
#include <fstream>
#include <filesystem>
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

TEST(engine_trim_session_compression) {
    auto& eng = Engine::instance();
    /* Наполняем сессию большими RESULT-сообщениями. */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().session.clear();
        eng.state().session.push_back({"user", "задача"});
        for (int i = 0; i < 200; ++i) {
            eng.state().session.push_back({"assistant", "ответ " + std::to_string(i) +
                " " + std::string(500, 'x')});
            eng.state().session.push_back({"user", "RESULT [tool]:\n" +
                std::string(2000, 'y')});
        }
    }
    /* Вызываем сжатие (через тестовый обёртку). */
    eng.trim_session_test();

    /* Итоговый объём должен упасть ниже бюджета (60 Кб). */
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        for (const auto& m : eng.session_for_test()) total += m.content.size();
    }
    ASSERT_TRUE(total <= 60000);
    /* Старые RESULT должны быть заменены сжатой заглушкой с именем инструмента. */
    bool stub_found = false;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        for (const auto& m : eng.session_for_test())
            if (m.content.find("сжат") != std::string::npos)
                stub_found = true;
    }
    ASSERT_TRUE(stub_found);
    /* В сжатой заглушке сохраняется имя инструмента (tool). */
    bool tool_kept = false;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        for (const auto& m : eng.session_for_test())
            if (m.content.find("[RESULT tool сжат]") != std::string::npos)
                tool_kept = true;
    }
    ASSERT_TRUE(tool_kept);
}

TEST(engine_settings_deploy_remote_dir_roundtrip) {
    std::map<std::string, std::string> settings;
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ChatMsg>&, LlmReply&) { return false; };
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
 * Фаза 3: list_dir / edit_file / undo_edit / web_fetch / git_*
 * ====================================================================== */

static void init_tools_for_phase3(const fs::path& project) {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ChatMsg>&, LlmReply&) { return false; };
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

TEST(list_dir_lists_files_and_dirs) {
    fs::path tmp = make_tmp_project();
    {
        std::ofstream f(tmp / "alpha.txt"); f << "x";
        std::ofstream f2(tmp / "beta.py");  f2 << "y";
        fs::create_directory(tmp / "subdir");
    }
    init_tools_for_phase3(tmp);

    ToolArgs a;
    a.path = tmp.string();
    std::string r = ToolsRegistry::instance().run("list_dir", a);
    ASSERT_TRUE(r.find("alpha.txt") != std::string::npos);
    ASSERT_TRUE(r.find("beta.py") != std::string::npos);
    ASSERT_TRUE(r.find("subdir/") != std::string::npos);
    fs::remove_all(tmp);
}

TEST(web_fetch_empty_url_rejected) {
    fs::path tmp = make_tmp_project();
    init_tools_for_phase3(tmp);
    ToolArgs a;
    std::string r = ToolsRegistry::instance().run("web_fetch", a);
    ASSERT_TRUE(r.find("пустой URL") != std::string::npos);
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
    ToolArgs a;  // query пуст
    std::string r = ToolsRegistry::instance().run("git_checkout", a);
    ASSERT_TRUE(r.find("укажи ветку") != std::string::npos);
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
    /* Не должно быть ошибки «cd» — только нормальный git-вывод oт состояния. */
    ASSERT_TRUE(r.find("cd") == std::string::npos);
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
    ASSERT_TRUE(rc2.find("cd") == std::string::npos);

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
    cb.llm_chat = [](const std::string&, const std::vector<ChatMsg>&, LlmReply&) { return false; };
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
        eng.state().session.push_back({"user", "привет"});
        eng.state().session.push_back({"assistant", "привет!\n\"quoted\""});
    }
    eng.save_session();
    ASSERT_TRUE(fs::exists(tmp / "wp_coder" / "session.json"));

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().session.clear();
    }
    eng.load_session();

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().session.size(), (size_t)2);
        ASSERT_EQ(eng.state().session[0].role, std::string("user"));
        ASSERT_EQ(eng.state().session[0].content, std::string("привет"));
        /* Экранирование кавычек/переводов строк переживает roundtrip. */
        ASSERT_EQ(eng.state().session[1].content, std::string("привет!\n\"quoted\""));
    }

    /* clear_session удаляет и файл на диске. */
    eng.clear_session();
    ASSERT_FALSE(fs::exists(tmp / "wp_coder" / "session.json"));

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

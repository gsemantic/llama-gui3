/*
 * test_wp_tools.cpp — безопасность и валидация WP-инструментов (Фаза 4.4).
 *
 * Тестируем ветки ВАЛИДАЦИИ (до запуска реальных команд): shell-инъекции,
 * DDL-запрет, невалидные аргументы. Функции, которые выполняют команды
 * (deploy, verify, php_lint, wp_check_deps), здесь не вызываются.
 */

#include "test_framework.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../modules/wordpress/wp_tools.h"

using namespace coder;

static void init_engine_and_wp() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ChatMsg>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    wp::register_wp_tools();
}

static std::string run_tool(const std::string& name, ToolArgs& a) {
    return ToolsRegistry::instance().run(name, a);
}

/* --- wp_cli: shell-инъекция --- */

TEST(wp_cli_empty_rejected) {
    init_engine_and_wp();
    ToolArgs a;  // cli пуст
    std::string r = run_tool("wp_cli", a);
    ASSERT_TRUE(r.find("пустая команда") != std::string::npos);
}

TEST(wp_cli_semicolon_injection_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.cli = "plugin list; rm -rf /";
    std::string r = run_tool("wp_cli", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

TEST(wp_cli_subshell_injection_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.cli = "plugin list $(rm -rf /)";
    std::string r = run_tool("wp_cli", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

TEST(wp_cli_backtick_injection_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.cli = "plugin list `id`";
    std::string r = run_tool("wp_cli", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

/* --- wp_db: DDL/ACL запрет --- */

TEST(wp_db_empty_query_rejected) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;  // query пуст
    std::string r = run_tool("wp_db", a);
    ASSERT_TRUE(r.find("пустой SQL-запрос") != std::string::npos);
}

TEST(wp_db_drop_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.query = "DROP TABLE wp_options";
    std::string r = run_tool("wp_db", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

TEST(wp_db_grant_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.query = "GRANT ALL ON *.* TO 'hacker'";
    std::string r = run_tool("wp_db", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

TEST(wp_db_select_passes_validation) {
    /* Безопасный SELECT проходит валидацию; дальше команда не выполняется,
     * потому что проект не существует — важен сам факт отсутствия «запрещено». */
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.query = "SELECT * FROM wp_options LIMIT 1";
    std::string r = run_tool("wp_db", a);
    ASSERT_TRUE(r.find("запрещено") == std::string::npos);
}

/* --- wp_rest: валидация endpoint --- */

TEST(wp_rest_invalid_endpoint_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().wp_site_url = "http://localhost";
        engine_state().wp_app_password = "secret";
    }
    ToolArgs a;
    a.query = "posts/;rm -rf /";
    std::string r = run_tool("wp_rest", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

/* --- wp_option --- */

TEST(wp_option_empty_name_rejected) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    std::string r = run_tool("wp_option", a);
    ASSERT_TRUE(r.find("пустое имя опции") != std::string::npos);
}

/* --- headless_render: валидация URL --- */

TEST(headless_render_empty_url_rejected) {
    init_engine_and_wp();
    ToolArgs a;
    std::string r = run_tool("headless_render", a);
    ASSERT_TRUE(r.find("пустой URL") != std::string::npos);
}

TEST(headless_render_without_scheme_rejected) {
    init_engine_and_wp();
    ToolArgs a;
    a.url = "localhost/wordpress";
    std::string r = run_tool("headless_render", a);
    ASSERT_TRUE(r.find("схему") != std::string::npos);
}
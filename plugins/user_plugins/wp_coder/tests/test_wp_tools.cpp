/*
 * test_wp_tools.cpp — безопасность и валидация WP-инструментов (Фаза 4.4).
 *
 * Тестируем ветки ВАЛИДАЦИИ (до запуска реальных команд): shell-инъекции,
 * DDL-запрет, невалидные аргументы. Функции, которые выполняют команды
 * (deploy, verify, php_lint, wp_check_deps), здесь не вызываются.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/agent_registry.h"
#include "../core/base_tools.h"
#include "../core/git_tools.h"
#include "../core/prompts.h"
#include "../modules/wordpress/wp_tools.h"

using namespace coder;

static void init_engine_and_wp() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    wp::register_wp_tools();
}

static std::string run_tool(const std::string& name, ToolArgs& a) {
    return ToolsRegistry::instance().run(name, a);
}

/* И1.6: обязательный параметр пуст — отказ приходит из валидации схемы,
 * а не из проверки внутри инструмента. Обе защиты нужны, поэтому
 * проверяем обе: вторую — явной передачей пустой строки. */
static std::string run_with_arg(const std::string& name, const char* key) {
    json::JsonValue args = json::JsonValue::object();
    args.set(key, "");
    return ToolsRegistry::instance().run(name, args);
}

/* --- wp_cli: shell-инъекция --- */

TEST(wp_cli_empty_rejected) {
    init_engine_and_wp();
    ToolArgs a;  // cli пуст
    std::string r = run_tool("wp_cli", a);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(run_with_arg("wp_cli", "cli").find("пустая команда") != std::string::npos);
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
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(run_with_arg("wp_db", "query").find("пустой SQL-запрос") != std::string::npos);
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
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(run_with_arg("wp_option", "query").find("пустое имя опции") != std::string::npos);
}

/* --- headless_render: валидация URL --- */

TEST(headless_render_empty_url_rejected) {
    init_engine_and_wp();
    ToolArgs a;
    std::string r = run_tool("headless_render", a);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(run_with_arg("headless_render", "url").find("пустой URL") != std::string::npos);
}

TEST(headless_render_without_scheme_rejected) {
    init_engine_and_wp();
    ToolArgs a;
    a.url = "localhost/wordpress";
    std::string r = run_tool("headless_render", a);
    ASSERT_TRUE(r.find("схему") != std::string::npos);
}

/* ======================================================================
 * И8.15: промпты WP-субагентов против настоящих инструментов
 * ====================================================================== */

/* Имена из раздела промпта: строки вида «- имя — …» внутри секций
 * ИНСТРУМЕНТЫ и НАВЫКИ. Формат строки — ТОТ ЖЕ, что у каталога
 * инструментов (tools_registry.cpp: describe_tool печатает «- имя — …»):
 * свой формат в промпте означал бы, что модель видит два вида списков и
 * проверка цепляется за один из них.
 *
 * Берутся только строки раздела, а не вхождения подстрок по всему
 * промпту: в тексте есть и пути (style.css), и имена функций WordPress
 * (add_action), и проверка подстрок была бы проверкой строки, а не
 * объявления. */
std::vector<std::string> declared_names(const std::string& prompt,
                                        const std::string& section) {
    std::vector<std::string> out;
    const size_t at = prompt.find("\n" + section + "\n");
    if (at == std::string::npos) return out;
    size_t pos = at + section.size() + 2;
    /* Раздел — до ближайшей пустой строки: так он заканчивается там же,
     * где и читается человеком. */
    while (pos < prompt.size()) {
        const size_t eol = prompt.find("\n", pos);
        const std::string line = prompt.substr(
            pos, (eol == std::string::npos ? prompt.size() : eol) - pos);
        if (line.rfind("- ", 0) != 0) break;
        const size_t dash = line.find(" — ");
        /* Имя — до тире. Строка без тире («- имя») тоже берётся: это
         * всё ещё объявленное имя, просто без пояснения. */
        const std::string name =
            dash == std::string::npos ? line.substr(2) : line.substr(2, dash - 2);
        if (!name.empty()) out.push_back(name);
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

TEST(the_names_a_wp_prompt_declares_all_exist_in_the_registry_and_skills) {
    register_base_tools();
    wp::register_wp_tools();
    register_git_tools();
    AgentRegistry reg;
    register_builtin_agents(reg);

    /* Переименование инструмента или навыка обязано ломать здесь, а не
     * молча оставлять в промпте имя, которого больше нет: модель
     * получила бы инструкцию «вызови то, чего нет» и тратила бы шаг
     * на заведомо неисполнимый вызов (Д2 — дрейф между промптом и
     * реестром). Поэтому проверяется ОБЪЯВЛЕННОЕ: то, что перечислено в
     * секциях ИНСТРУМЕНТЫ/НАВЫКИ, а не вхождения подстрок по всему
     * тексту. */
    const std::vector<std::string> want = {"wp_theme", "wp_plugin", "wp_hook",
                                           "wp_deploy"};
    for (const std::string& name : want) {
        const auto def = reg.find(name);
        ASSERT_TRUE(def != nullptr);
        const std::string& prompt = def->prompt;

        const std::vector<std::string> tools =
            declared_names(prompt, "ИНСТРУМЕНТЫ");
        if (tools.empty()) {
            std::cerr << "  в промпте " << name
                      << " нет объявленных инструментов" << std::endl;
        }
        ASSERT_FALSE(tools.empty());
        for (const std::string& tool : tools) {
            if (ToolsRegistry::instance().find(tool) == nullptr) {
                std::cerr << "  " << name << " объявляет несуществующий "
                          << "инструмент " << tool << std::endl;
            }
            ASSERT_TRUE(ToolsRegistry::instance().find(tool) != nullptr);
        }

        /* Навыки сверяются с тем, что реально отдаёт модуль WordPress:
         * список навыков — это get_wp_skills, и промпт обязан ссылаться
         * на то, что есть. */
        const std::vector<std::string> skills =
            declared_names(prompt, "НАВЫКИ");
        if (skills.empty()) {
            std::cerr << "  в промпте " << name
                      << " нет объявленных навыков" << std::endl;
        }
        ASSERT_FALSE(skills.empty());
        const std::vector<Skill> wp_skills = wp::get_wp_skills();
        for (const std::string& skill : skills) {
            bool found = false;
            for (const Skill& s : wp_skills) {
                if (s.name == skill) found = true;
            }
            if (!found) {
                std::cerr << "  " << name << " ссылается на навык " << skill
                          << ", которого нет в модуле WordPress" << std::endl;
            }
            ASSERT_TRUE(found);
        }
    }
}

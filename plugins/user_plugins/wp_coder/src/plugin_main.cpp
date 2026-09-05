/*
 * plugin_main.cpp — Точка входа плагина AI Coder (модульная архитектура).
 *
 * Этот плагин является полноценным AI-кодером с модульной архитектурой.
 * WordPress — один из модулей. Другие модули (Python и т.д.) подключаются
 * аналогично.
 *
 * Инициализация:
 *   1. Регистрация модулей (WordPress, Python, ...)
 *   2. Инициализация ядра (Engine + ToolsRegistry + SkillsManager)
 *   3. Регистрация UI (окна, команды, меню)
 *   4. Регистрация agent mode для основного чата
 */

#include "core/engine.h"
#include "core/tools_registry.h"
#include "core/skills_manager.h"
#include "core/base_tools.h"
#include "core/git_tools.h"
#include "core/module_api.h"
#include "ui/coder_window.h"

/* Модули. */
#include "modules/wordpress/wp_module.h"
#include "modules/python/python_module.h"

#include "imgui.h"
#include "plugins/plugin_api.h"

#include <cstdio>
#include <cstring>
#include <iostream>

/* Глобальные хендлы хоста. */
LlamaPluginHost* g_host = nullptr;
const LlamaHostApi* g_api = nullptr;

/* --- Agent mode callbacks --- */

static char* agent_mode_on_message(LlamaPluginHost* host, const char* user_message, void* user_data) {
    if (!user_message || !user_message[0]) return nullptr;

    auto& eng = coder::engine();
    eng.submit(user_message);

    /* Блокирующее ожидание результата (для agent mode в основном чате).
     * В реальном коде стоит сделать async, но для MVP — блокируемся. */
    /* Просто запускаем задачу и ждём первый финальный ответ. */
    std::string task = user_message;
    std::string full_response;

    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().last_agent_task = user_message;
    }

    /* Сборка промпта. */
    std::string sys_prompt = eng.build_system_prompt();

    for (int step = 0; step < 12; ++step) {
        if (!g_api || !g_host) break;
        if (g_api->llm_is_connected(g_host) != 1) {
            full_response = "[ошибка] LLM не подключён";
            break;
        }

        char* response = nullptr;
        int rc = g_api->llm_complete_ex(g_host, sys_prompt.c_str(), task.c_str(), &response);
        if (rc != 1 || !response) {
            full_response = "[ошибка] LLM не ответил";
            break;
        }
        std::string resp(response);
        g_api->free_string(g_host, response);

        std::string rest;
        std::string block = coder::Engine::extract_action(resp, rest);
        if (!rest.empty()) {
            if (!full_response.empty()) full_response += "\n\n";
            full_response += rest;
        }
        if (block.empty()) break;

        coder::Engine::Action act;
        if (!coder::Engine::parse_action(block, act)) {
            full_response += "\n\n[ошибка разбора wp_action]";
            break;
        }

        coder::ToolArgs args;
        args.path = act.path;
        args.root = act.root;
        args.query = act.query;
        args.pattern = act.pattern;
        args.content = act.content;
        args.cli = act.cli;
        args.url = act.url;
        args.k = act.k;

        std::string result = coder::ToolsRegistry::instance().run(act.tool, args);

        /* Проверка разрешения. */
        {
            std::unique_lock<std::mutex> lk(eng.state().mtx);
            if (eng.state().waiting_for_permission) {
                eng.state().waiting_in_sync = true;
                eng.state().permission_cv.wait(lk, [&eng] {
                    return !eng.state().waiting_for_permission;
                });
                eng.state().waiting_in_sync = false;
                lk.unlock();
                if (eng.state().last_agent_task.empty()) {
                    full_response += "\n\nДоступ отклонён.";
                    break;
                }
                result = coder::ToolsRegistry::instance().run(act.tool, args);
            }
        }

        task = "RESULT [" + act.tool + "]:\n" + result;
    }

    if (full_response.empty()) full_response = "(пустой ответ)";
    char* out = (char*)malloc(full_response.size() + 1);
    if (out) memcpy(out, full_response.c_str(), full_response.size() + 1);
    return out;
}

static void agent_mode_render_extras(LlamaPluginHost* host, void* user_data) {
    if (!g_api || !g_host) return;
    coder::ui::render_extras();
}

/* --- Экспортируемые функции плагина --- */

extern "C" {

LLAMA_PLUGIN_EXPORT const char* ll_plugin_api_version(void) {
    return LLAMA_PLUGIN_API_VERSION;
}

LLAMA_PLUGIN_EXPORT const LlamaPluginInfo* ll_plugin_info(void) {
    static const LlamaPluginInfo info = {
        "wp_coder",
        "0.2.0",
        "AI-кодер: модульная архитектура (WordPress, Python, ...)",
        "llama-gui"
    };
    return &info;
}

LLAMA_PLUGIN_EXPORT int ll_plugin_init(LlamaPluginHost* host, const LlamaHostApi* api) {
    if (!host || !api) return 1;
    g_host = host;
    g_api = api;

    /* 1. Регистрируем модули. */
    coder::wp::register_module();
    coder::python::register_module();

    /* 2. Инициализируем движок. */
    coder::HostCallbacks cb;
    cb.llm_is_connected = []() -> bool {
        return g_api && g_api->llm_is_connected(g_host) == 1;
    };
    cb.llm_complete = [](const std::string& sys, const std::string& user,
                         std::string& resp) -> bool {
        if (!g_api || !g_host) return false;
        char* r = nullptr;
        int rc = g_api->llm_complete_ex(g_host, sys.c_str(), user.c_str(), &r);
        if (rc != 1 || !r) return false;
        resp = r;
        g_api->free_string(g_host, r);
        return true;
    };
    cb.path_data_dir = []() -> std::string {
        if (!g_api || !g_host) return "";
        const char* d = g_api->path_data_dir(g_host);
        return d ? std::string(d) : "";
    };
    cb.path_config_dir = []() -> std::string {
        if (!g_api || !g_host) return "";
        const char* d = g_api->path_config_dir(g_host);
        return d ? std::string(d) : "";
    };
    cb.settings_get = [](const std::string& key, const std::string& def) -> std::string {
        if (!g_api || !g_host) return def;
        char* s = g_api->settings_get(g_host, key.c_str());
        if (!s) return def;
        std::string v(s);
        g_api->free_string(g_host, s);
        if (v.empty()) return def;
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        return v;
    };
    cb.settings_set = [](const std::string& key, const std::string& value) {
        if (!g_api || !g_host) return;
        g_api->settings_set(g_host, key.c_str(), value.c_str());
    };
    cb.rag_process_document = [](const std::string& path) -> bool {
        if (!g_api || !g_host) return false;
        return g_api->rag_process_document(g_host, path.c_str()) == 1;
    };
    cb.rag_build_prompt = [](const std::string& query, int k,
                             const std::string& path_filter) -> std::string {
        if (!g_api || !g_host) return "";
        char* p = g_api->rag_build_prompt(g_host, query.c_str(), k,
                                          path_filter.empty() ? nullptr : path_filter.c_str());
        if (!p) return "";
        std::string result(p);
        g_api->free_string(g_host, p);
        return result;
    };
    cb.rag_index_count = []() -> int {
        if (!g_api || !g_host) return 0;
        return g_api->rag_index_count(g_host);
    };

    coder::engine().init(std::move(cb));

    /* 3. Инициализируем модули (регистрация инструментов). */
    coder::ModuleRegistry::instance().init_all();

    /* 4. Регистрируем базовые инструменты. */
    coder::register_base_tools();
    coder::register_git_tools();
    coder::register_rag_tools();

    /* 5. Загружаем навыки (из модулей + .md файлов). */
    coder::SkillsManager::instance().load();
    /* Загружаем .md файлы из каталога плагина. */
    {
        const char* d = api->path_data_dir(host);
        if (d) {
            std::string skills_dir = std::string(d) + "/wp_coder/skills";
            coder::SkillsManager::instance().load_from_directory(skills_dir);
        }
    }
    /* Каталог плагина (рядом с .so). */
    coder::SkillsManager::instance().load_from_directory(
        std::string(WP_CODER_SKILLS_DIR));

    /* 6. Регистрируем UI. */
    coder::ui::init_windows();

    /* 7. Регистрируем agent mode для основного чата. */
    static LlamaPluginAgentMode agent_mode = {};
    agent_mode.name = "ai_coder";
    agent_mode.display_name = "AI Coder";
    agent_mode.on_message = agent_mode_on_message;
    agent_mode.render_extras = agent_mode_render_extras;
    agent_mode.user_data = nullptr;
    api->agent_mode_register(host, &agent_mode);

    /* 8. Запускаем worker-поток движка. */
    coder::engine().start();

    return 0;
}

LLAMA_PLUGIN_EXPORT void ll_plugin_render(void) {
    if (!g_api || !g_host) return;
    coder::ui::render_all_windows();
}

LLAMA_PLUGIN_EXPORT void ll_plugin_shutdown(void) {
    coder::engine().stop();
    coder::ModuleRegistry::instance().shutdown_all();
    g_host = nullptr;
    g_api = nullptr;
}

} // extern "C"

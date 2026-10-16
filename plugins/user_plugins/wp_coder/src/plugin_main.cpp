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
#include "core/json_utils.h"
#include "ui/coder_window.h"
#include "host_bridge/llm_slots.h"

/* Модули. */
#include "modules/wordpress/wp_module.h"
#include "modules/python/python_module.h"
#include "modules/devops/devops_module.h"

#include "imgui.h"
#include "plugins/plugin_api.h"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <cstddef>
#include <iostream>

/* Глобальные хендлы хоста. */
LlamaPluginHost* g_host = nullptr;
const LlamaHostApi* g_api = nullptr;

/* --- Agent mode callbacks --- */

static char* agent_mode_on_message(LlamaPluginHost* host, const char* user_message, void* user_data) {
    if (!user_message || !user_message[0]) return nullptr;
    std::string message = coder::text::sanitize_utf8(user_message);
    std::cerr << "[wp_coder] agent_mode_on_message: " << message << std::endl;

    auto& eng = coder::engine();
    eng.submit(message);

    /* Агент работает в worker-потоке. Ждём результат (таймаут 5 минут). */
    std::cerr << "[wp_coder] agent_mode_on_message: waiting for response..." << std::endl;
    std::string response = eng.wait_response(300000);
    std::cerr << "[wp_coder] agent_mode_on_message response_len=" << response.size()
              << " head=" << coder::text::utf8_prefix(response, 120) << std::endl;

    char* out = (char*)malloc(response.size() + 1);
    if (out) memcpy(out, response.c_str(), response.size() + 1);
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

namespace {

/* Версия плагина в ОДНОМ месте.
 *
 * Раньше она была продублирована в четырёх файлах (plugin.json,
 * plugins/wp_coder.json, ll_plugin_info(), CHANGELOG.md) и разошлась:
 * 0.3.0 / 0.1.0 / 0.3.0 / 0.4.0. При изменении нужно править все четыре.
 *
 * CMake генерирует WP_CODER_VERSION из plugin.json, чтобы JSON остался
 * единственным источником истины; если макрос не определён (сборка вне
 * CMake) — падаем на значение из манифеста. */
#ifdef WP_CODER_VERSION
const char* kPluginVersion = WP_CODER_VERSION;
#else
const char* kPluginVersion = "0.5.0";
#endif

}  // namespace

LLAMA_PLUGIN_EXPORT const LlamaPluginInfo* ll_plugin_info(void) {
    static const LlamaPluginInfo info = {
        "wp_coder",
        kPluginVersion,
        "AI-кодер: модульная архитектура (WordPress, Python, ...)",
        "llama-gui"
    };
    return &info;
}

LLAMA_PLUGIN_EXPORT int ll_plugin_init(LlamaPluginHost* host, const LlamaHostApi* api) {
    std::cerr << "[wp_coder] ll_plugin_init: host=" << (void*)host
              << " api=" << (void*)api
              << " agent_mode_register=" << (void*)(api ? api->agent_mode_register : nullptr)
              << " llm_chat_messages=" << (void*)(api ? api->llm_chat_messages : nullptr)
              << std::endl;
    if (!host || !api) return 1;
    g_host = host;
    g_api = api;

    /* 1. Регистрируем модули. */
    coder::wp::register_module();
    coder::python::register_module();
    coder::devops::register_module();

    /* 2. Инициализируем движок.
     *
     * И13.8: слоты LLM и AST ставит мост (host_bridge/llm_slots.cpp) — там
     * же, где живёт проба полей хоста. Здесь осталось только то, что к
     * возможностям хоста не относится: пути, настройки, rag, рассылка
     * событий в чат. Слот, у которого у хоста нет поля, остаётся пустым,
     * и это НЕ недоделка, а ответ: запасной путь выбирает ядро (LlmClient,
     * llm_source, ast_symbols). До И13.8 слоты ставились здесь, а src/ в
     * сборку тестов не входит — то есть весь слой деградации не был
     * проверен ничем. */
    coder::HostCallbacks cb;
    coder::host::install_llm_slots(cb, g_api, g_host);

    /* Прогресс в чат приложения (agent_mode_push_event). */
    cb.chat_event = [](const std::string& event) {
        if (!g_api || !g_host) return;
        g_api->agent_mode_push_event(g_host, event.c_str());
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
    /* Загружаем .md файлы из каталога плагина (внешние — без привязки к модулю). */
    {
        const char* d = api->path_data_dir(host);
        if (d) {
            std::string skills_dir = std::string(d) + "/wp_coder/skills";
            coder::SkillsManager::instance().load_from_directory(skills_dir);
        }
    }
    /* Каталог плагина (рядом с .so) — WP-навыки. */
    coder::SkillsManager::instance().load_from_directory(
        std::string(WP_CODER_SKILLS_DIR), "wordpress");

    /* И9.7: каталог профилей harness, поставленных с плагином. Строка
     * приходит из CMake (WP_CODER_PROFILES_DIR) — ровно тем же путём, что
     * каталог навыков: путь к дереву плагина известен здесь и больше
     * нигде, а ядро о своём расположении не знает (D-7). Порядок важен
     * только в одном: пусть ДО Engine::init ниже, иначе первый
     * load_settings прочитал бы пустой каталог, и профиль «не нашёлся
     * бы» до первой перезагрузки настроек. */
    coder::engine().set_profiles_bundled_dir(std::string(WP_CODER_PROFILES_DIR));

    /* Активируем навыки выбранного модуля.
     *
     * Раньше здесь стояла проверка «если active_module непустой». На
     * чистой установке настройка пуста, вызов пропускался, и агент
     * оставался вообще без навыков (и без намёка, что skill_detail
     * существует). Теперь set_module/set_active вызывается всегда —
     * refresh_active() сам выберет разумный дефолт. */
    {
        const auto& st = coder::engine().state();
        coder::SkillsManager& sm = coder::SkillsManager::instance();
        if (!st.active_module.empty()) {
            sm.set_module(st.active_module);
        } else {
            /* Модуль не выбран — оставляем active_module_ пустым, чтобы
             * были видны навыки всех модулей. */
            sm.refresh_active();
        }
        std::cerr << "[wp_coder] skills: загружено " << sm.all_skills().size()
                  << ", активно " << sm.active_skills().size() << std::endl;
    }

    /* 6. Регистрируем UI. */
    coder::ui::init_windows();

    /* 7. Регистрируем agent mode для основного чата. */
    static LlamaPluginAgentMode agent_mode = {};
    agent_mode.name = "ai_coder";
    agent_mode.display_name = "AI Coder";
    agent_mode.on_message = agent_mode_on_message;
    agent_mode.render_extras = agent_mode_render_extras;
    agent_mode.user_data = nullptr;
    std::cerr << "[wp_coder] registering agent_mode: api=" << (void*)api
              << " agent_mode_register=" << (void*)(api ? api->agent_mode_register : nullptr)
              << " host=" << (void*)host << std::endl;
    api->agent_mode_register(host, &agent_mode);
    std::cerr << "[wp_coder] agent_mode registered: " << agent_mode.name << std::endl;

    /* Resume сессии (5.2): подхватываем сохранённый диалог до старта worker. */
    coder::engine().load_session();

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

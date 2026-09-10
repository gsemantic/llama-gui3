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
    std::cerr << "[wp_coder] agent_mode_on_message: " << user_message << std::endl;

    auto& eng = coder::engine();
    eng.submit(user_message);

    /* Агент работает в worker-потоке. Ждём результат (таймаут 5 минут). */
    std::string response = eng.wait_response(300000);
    std::cerr << "[wp_coder] agent_mode_on_message response_len=" << response.size()
              << " head=" << response.substr(0, 120) << std::endl;

    char* out = (char*)malloc(response.size() + 1);
    if (out) memcpy(out, response.c_str(), response.size() + 1);
    return out;
}

static void agent_mode_render_extras(LlamaPluginHost* host, void* user_data) {
    if (!g_api || !g_host) return;
    coder::ui::render_extras();
}

/* --- Экспортируемые функции плагина --- */

/* Минимальный JSON-эскейпер (без nlohmann в плагине). */
static std::string json_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    r += buf;
                } else {
                    r += c;
                }
        }
    }
    return r;
}

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
    /* Multi-turn: парсим JSON от llm_chat_messages (ok/content/usage).
     * Fallback на llm_complete_ex при старом хосте без llm_chat_messages. */
    cb.llm_chat = [cb](const std::string& sys_prompt,
                     const std::vector<coder::ChatMsg>& messages,
                     coder::LlmReply& out) -> bool {
        if (!g_api || !g_host) return false;

        /* Проверяем, поддерживает ли хост llm_chat_messages (по size). */
        bool has_chat = false;
        if (g_api->size >= offsetof(LlamaHostApi, llm_chat_messages) +
                              sizeof(decltype(g_api->llm_chat_messages)))
            has_chat = g_api->llm_chat_messages != nullptr;

        if (has_chat) {
            /* Собираем JSON-массив сообщений. */
            std::string json = "[";
            for (size_t i = 0; i < messages.size(); ++i) {
                if (i) json += ",";
                json += "{\"role\":\"" + messages[i].role + "\",\"content\":\"" +
                        json_escape(messages[i].content) + "\"}";
            }
            json += "]";
            char* raw = g_api->llm_chat_messages(g_host,
                sys_prompt.empty() ? nullptr : sys_prompt.c_str(), json.c_str());
            if (!raw) return false;
            std::string resp(raw);
            g_api->free_string(g_host, raw);

            /* Парсим {"ok":1,"content":"...","finish_reason":"...","prompt_tokens":N,"completion_tokens":N}. */
            auto find_str = [](const std::string& s, const char* key) -> std::string {
                std::string pat = std::string("\"") + key + "\":\"";
                size_t p = s.find(pat);
                if (p == std::string::npos) return "";
                p += pat.size();
                size_t e = p;
                while (e < s.size() && s[e] != '"') {
                    if (s[e] == '\\' && e + 1 < s.size()) e += 2;
                    else ++e;
                }
                std::string v = s.substr(p, e - p);
                std::string r;
                for (size_t i = 0; i < v.size(); ++i) {
                    if (v[i] == '\\' && i + 1 < v.size()) {
                        switch (v[i + 1]) {
                            case 'n': r += '\n'; i++; break;
                            case 't': r += '\t'; i++; break;
                            case 'r': r += '\r'; i++; break;
                            case '"': r += '"'; i++; break;
                            case '\\': r += '\\'; i++; break;
                            default: r += v[i]; break;
                        }
                    } else r += v[i];
                }
                return r;
            };
            auto find_int = [](const std::string& s, const char* key) -> int {
                std::string pat = std::string("\"") + key + "\":";
                size_t p = s.find(pat);
                if (p == std::string::npos) return 0;
                p += pat.size();
                int n = 0;
                while (p < s.size() && std::isdigit((unsigned char)s[p])) {
                    n = n * 10 + (s[p] - '0'); ++p;
                }
                return n;
            };
            if (find_int(resp, "ok") != 1) return false;
            out.content = find_str(resp, "content");
            out.finish_reason = find_str(resp, "finish_reason");
            if (out.finish_reason.empty()) out.finish_reason = "stop";
            out.prompt_tokens = find_int(resp, "prompt_tokens");
            out.completion_tokens = find_int(resp, "completion_tokens");
            return true;
        }

        /* Fallback: одногилый (старый хост без llm_chat_messages). */
        std::string last_user;
        for (const auto& m : messages)
            if (m.role == "user") last_user = m.content;
        std::string resp;
        if (!cb.llm_complete || !cb.llm_complete(sys_prompt, last_user, resp))
            return false;
        out.content = resp;
        out.finish_reason = "stop";
        out.prompt_tokens = 0;
        out.completion_tokens = 0;
        return true;
    };

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

    /* Активируем навыки выбранного модуля. */
    {
        const auto& st = coder::engine().state();
        if (!st.active_module.empty())
            coder::SkillsManager::instance().set_module(st.active_module);
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

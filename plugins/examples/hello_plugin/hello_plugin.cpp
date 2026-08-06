/*
 * hello_plugin.cpp — демонстрационный плагин llama-gui.
 *
 * Показывает, как независимый плагин использует возможности приложения:
 *   - меню (menu_add / menu_add_item)
 *   - команды и горячие клавиши (command_register)
 *   - окна (window_register / window_set_visible)
 *   - диалоги (dialog_info / dialog_warning / dialog_confirmation)
 *   - чат (chat_send_message)
 *   - RAG (rag_search / rag_index_count)
 *   - настройки и состояние (settings_set / state_set)
 *
 * Плагин подключает только SDK-заголовок plugin_api.h и Dear ImGui.
 */

#include "plugins/plugin_api.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <string>

// Хост и таблица API (заполняются в ll_plugin_init)
static LlamaPluginHost* g_host = nullptr;
static const LlamaHostApi* g_api = nullptr;

// Хендлы, созданные плагином
static LlamaPluginWindow* g_window = nullptr;
static LlamaPluginMenu* g_menu = nullptr;

// ============================================================================
// Команды плагина
// ============================================================================

static void cmd_open_window(void*) {
    if (g_api && g_host && g_window) {
        g_api->window_set_visible(g_host, g_window, 1);
    }
}

static void cmd_show_info(void*) {
    if (g_api && g_host) {
        g_api->dialog_info(g_host, "Hello Plugin",
                           "Это диалог, показанный плагином через host API.");
    }
}

static void cmd_send_message(void*) {
    if (g_api && g_host) {
        const char* msg = "Привет! Это сообщение отправлено плагином через chat_send_message.";
        if (g_api->chat_send_message(g_host, msg) != 1) {
            g_api->dialog_warning(g_host, "Hello Plugin",
                                  "Не удалось отправить сообщение в чат.");
        }
    }
}

static void cmd_settings(void*) {
    if (!g_api || !g_host) return;
    g_api->settings_set(g_host, "hello_plugin.counter", "{\"count\":1}");
    char* val = g_api->settings_get(g_host, "hello_plugin.counter");
    std::string text = val ? val : "(null)";
    if (val) g_api->free_string(g_host, val);
    g_api->dialog_info(g_host, "Hello Plugin", text.c_str());
}

static void cmd_rag_search(void*) {
    if (!g_api || !g_host) return;
    char* results = g_api->rag_search(g_host, "пример поиска", 3, "");
    if (!results) {
        g_api->dialog_warning(g_host, "Hello Plugin",
                              "RAG недоступен (не инициализирован или нет индекса).");
        return;
    }
    std::string text = results;
    if (text.size() > 500) text = text.substr(0, 500) + "...";
    g_api->free_string(g_host, results);
    g_api->dialog_info(g_host, "RAG Search Results", text.c_str());
}

static void cmd_confirm(void*) {
    if (g_api && g_host) {
        g_api->dialog_confirmation(g_host, "Hello Plugin",
                                   "Нажмите OK, чтобы отправить сообщение.",
                                   cmd_send_message, nullptr);
    }
}

// ============================================================================
// Экспортируемые функции плагина
// ============================================================================

extern "C" {

LLAMA_PLUGIN_EXPORT const char* ll_plugin_api_version(void) {
    return LLAMA_PLUGIN_API_VERSION;
}

LLAMA_PLUGIN_EXPORT const LlamaPluginInfo* ll_plugin_info(void) {
    static const LlamaPluginInfo info = {
        "hello_plugin",      // name
        "1.0.0",             // version
        "Демонстрационный плагин: меню, окно, команды, диалоги, чат, RAG",
        "llama-gui"          // author
    };
    return &info;
}

LLAMA_PLUGIN_EXPORT int ll_plugin_init(LlamaPluginHost* host, const LlamaHostApi* api) {
    if (!host || !api) {
        return 1;
    }
    g_host = host;
    g_api = api;

    // Логирование
    if (g_api->log) {
        const std::string msg =
            "hello_plugin: инициализация, host " + std::string(g_api->app_version);
        g_api->log(g_host, LLAMA_LOG_INFO, msg.c_str());
    }

    // Регистрируем команды
    g_api->command_register(g_host, "hello_plugin_open_window", cmd_open_window, nullptr,
                            "Open Hello Plugin window", "Ctrl+Shift+H");
    g_api->command_register(g_host, "hello_plugin_show_info", cmd_show_info, nullptr,
                            "Show info dialog", nullptr);
    g_api->command_register(g_host, "hello_plugin_send_message", cmd_send_message, nullptr,
                            "Send a message to chat", nullptr);
    g_api->command_register(g_host, "hello_plugin_rag_search", cmd_rag_search, nullptr,
                            "Search RAG index", nullptr);
    g_api->command_register(g_host, "hello_plugin_settings", cmd_settings, nullptr,
                            "Read/write plugin setting", nullptr);
    g_api->command_register(g_host, "hello_plugin_confirm", cmd_confirm, nullptr,
                            "Show confirmation dialog", nullptr);

    // Добавляем меню плагина
    g_menu = g_api->menu_add(g_host, "Hello Plugin");
    if (g_menu) {
        g_api->menu_add_item(g_host, g_menu, "Open Window", "hello_plugin_open_window",
                             "Ctrl+Shift+H");
        g_api->menu_add_item(g_host, g_menu, "Send Message", "hello_plugin_send_message", nullptr);
        g_api->menu_add_item(g_host, g_menu, "RAG Search", "hello_plugin_rag_search", nullptr);
        g_api->menu_add_separator(g_host, g_menu);
        g_api->menu_add_item(g_host, g_menu, "Show Info", "hello_plugin_show_info", nullptr);
        g_api->menu_add_item(g_host, g_menu, "Confirm", "hello_plugin_confirm", nullptr);
    }

    // Регистрируем окно (рисуется в ll_plugin_render)
    g_window = g_api->window_register(g_host, "hello_plugin", "Hello Plugin");

    // Пути
    if (g_api->path_plugins_dir) {
        const std::string msg = std::string("plugins dir: ") + g_api->path_plugins_dir(g_host);
        g_api->log(g_host, LLAMA_LOG_DEBUG, msg.c_str());
    }

    return 0; // успех
}

LLAMA_PLUGIN_EXPORT void ll_plugin_render(void) {
    if (!g_api || !g_host || !g_window) return;
    if (g_api->window_is_visible(g_host, g_window) != 1) return;

    ImGui::Begin("Hello Plugin");
    ImGui::Text("Это окно нарисовано плагином через Dear ImGui.");
    ImGui::Separator();

    if (ImGui::Button("Отправить сообщение в чат")) {
        cmd_send_message(nullptr);
    }
    if (ImGui::Button("Поиск по RAG")) {
        cmd_rag_search(nullptr);
    }

    const int index_count = g_api->rag_index_count(g_host);
    ImGui::Text("Чанков в RAG-индексе: %d", index_count);

    const int connected = g_api->llm_is_connected(g_host);
    ImGui::Text("Сервер LLM: %s", connected ? "подключен" : "не подключен");

    ImGui::End();
}

LLAMA_PLUGIN_EXPORT void ll_plugin_shutdown(void) {
    if (g_api && g_host) {
        g_api->log(g_host, LLAMA_LOG_INFO, "hello_plugin: выгрузка");
    }
    g_host = nullptr;
    g_api = nullptr;
    g_window = nullptr;
    g_menu = nullptr;
}

} // extern "C"

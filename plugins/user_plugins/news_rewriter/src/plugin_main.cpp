/*
 * plugin_main.cpp — точка входа плагина news_rewriter.
 *
 * Экспортирует обязательные функции SDK (ll_plugin_*) и связывает модули:
 * конфиг (settings_*), worker (фоновый поток), UI (Dear ImGui).
 *
 * Потоковая модель (см. docs/ARCHITECTURE.md):
 *   main-поток  — ll_plugin_init/render/shutdown, host-настройки, ImGui;
 *   worker-поток — конвейер (пока заглушка), llm_complete (с этапа 3).
 */

#include "plugins/plugin_api.h"

#include <memory>
#include <string>

#include "config.h"
#include "profile.h"
#include "sink.h"
#include "ui.h"
#include "worker.h"

using namespace news_rewriter;

static LlamaPluginHost* g_host = nullptr;
static const LlamaHostApi* g_api = nullptr;

static LlamaPluginWindow* g_window = nullptr;
static LlamaPluginMenu* g_menu = nullptr;

static std::unique_ptr<Worker> g_worker;
static UiDeps g_ui_deps;
static Config g_config;

// Каталог данных (data_dir) для автономных профилей настроек и Storage.
static std::string g_profiles_data_dir;
// Имя активного профиля настроек плагина.
static std::string g_active_profile;

// ============================================================================
// Конфигурация (persist в профилях плагина, автономно от профилей хоста)
// ============================================================================

static Config load_config() {
    Config cfg = default_config();
    if (g_profiles_data_dir.empty()) return cfg;
    std::string active = active_profile_name(g_profiles_data_dir);
    if (active.empty()) {
        // Миграция со старого хранилища в настройках хоста (если есть).
        if (g_api && g_host) {
            char* raw = g_api->settings_get(g_host, kConfigKey);
            if (raw) {
                bool ok = false;
                std::string error;
                Json j = Json::parse(raw, &ok, &error);
                g_api->free_string(g_host, raw);
                if (ok) {
                    cfg = config_from_json(j);
                } else if (g_api->log) {
                    const std::string msg =
                        "news_rewriter: ошибка парсинга старого конфига: " + error;
                    g_api->log(g_host, LLAMA_LOG_WARNING, msg.c_str());
                }
                save_profile(g_profiles_data_dir, "default", cfg);
                active = "default";
            }
        }
    }
    if (active.empty()) {
        // Первый запуск без профилей — создаём дефолтный.
        save_profile(g_profiles_data_dir, "default", cfg);
        active = "default";
    }
    g_active_profile = active;
    return load_profile(g_profiles_data_dir, active);
}

static void save_config(const Config& cfg) {
    if (g_profiles_data_dir.empty()) return;
    if (g_active_profile.empty()) {
        g_active_profile = active_profile_name(g_profiles_data_dir);
    }
    if (g_active_profile.empty()) g_active_profile = "default";
    save_profile(g_profiles_data_dir, g_active_profile, cfg);
}

// ============================================================================
// Команды
// ============================================================================

static void cmd_open_window(void*) {
    if (g_api && g_host && g_window) {
        g_api->window_set_visible(g_host, g_window, 1);
    }
}

static void cmd_run(void*) {
    if (g_worker) {
        g_worker->post(Command{CmdType::RunNow});
    }
}

static void cmd_about(void*) {
    if (g_api && g_host) {
        g_api->dialog_info(g_host, "News Rewriter",
                           "Агент: обход адресов по расписанию, рерайт новостей "
                           "через LLM, сохранение локально. v0.1.0 "
                           "(этап 5: scheduler, retry).");
    }
}

// ============================================================================
// Экспортируемые функции SDK
// ============================================================================

extern "C" {

LLAMA_PLUGIN_EXPORT const char* ll_plugin_api_version(void) {
    return LLAMA_PLUGIN_API_VERSION;
}

LLAMA_PLUGIN_EXPORT const LlamaPluginInfo* ll_plugin_info(void) {
    static const LlamaPluginInfo info = {
        "news_rewriter",
        "0.1.1",
        "Агент: сбор новостей с указанных адресов, рерайт через LLM, "
        "сохранение локально",
        "llama-gui"
    };
    return &info;
}

LLAMA_PLUGIN_EXPORT int ll_plugin_init(LlamaPluginHost* host, const LlamaHostApi* api) {
    if (!host || !api) return 1;
    g_host = host;
    g_api = api;

    if (g_api->log) {
        const std::string msg =
            "news_rewriter: инициализация (host " + std::string(g_api->app_version) + ")";
        g_api->log(g_host, LLAMA_LOG_INFO, msg.c_str());
    }

    // Каталог данных (для Storage/Sink и автономных профилей настроек).
    if (g_api->path_data_dir) {
        const char* data_dir = g_api->path_data_dir(g_host);
        if (data_dir) {
            g_profiles_data_dir = data_dir;
        }
    }

    // Конфигурация (persist в профилях плагина, автономно от профилей хоста).
    g_config = load_config();
    if (g_api->log) {
        g_api->log(g_host, LLAMA_LOG_DEBUG,
                   ("news_rewriter: источников: " + std::to_string(g_config.sources.size())).c_str());
    }

    // Воркер: фоновый поток, очередь команд. Хост-настройки не вызывает.
    g_worker = std::make_unique<Worker>();
    g_worker->set_config(g_config);

    // Каталог данных (для Storage/Sink). path_data_dir доступен всегда.
    if (g_api->path_data_dir) {
        const char* data_dir = g_api->path_data_dir(g_host);
        if (data_dir) {
            g_worker->set_data_dir(data_dir);
            g_ui_deps.data_dir = data_dir;
        }
    }

    // Реестр sink-ов: v1 — запись на диск; этап 6 — отправка на сервер.
    // Новые sink-ы добавляются здесь без правок ядра конвейера.
    SinkRegistry::instance().register_factory("local_file", make_local_file_sink);
    SinkRegistry::instance().register_factory("http", make_http_sink);
    SinkRegistry::instance().register_factory("wordpress", make_wordpress_sink);

    g_worker->set_log_callback([](const std::string& msg) {
        if (g_api && g_host) {
            g_api->log(g_host, LLAMA_LOG_INFO, msg.c_str());
        }
    });
    g_worker->set_llm([](const std::string& system, const std::string& user,
                          std::string& response, std::string& error) -> bool {
        if (!g_api || !g_host) {
            error = "хост недоступен";
            return false;
        }
        if (g_api->llm_is_connected && g_api->llm_is_connected(g_host) != 1) {
            error = "LLM не подключён (нет активного сервера/облака)";
            return false;
        }
        char* out = nullptr;
        // Промпт-роль: шлём роль (system) и контент статьи (user) раздельно,
        // если хост поддерживает llm_complete_ex. Иначе (старый хост) —
        // объединяем в один промпт для обратной совместимости.
        const int rc = (g_api->llm_complete_ex != nullptr)
            ? g_api->llm_complete_ex(g_host, system.c_str(), user.c_str(), &out)
            : g_api->llm_complete(g_host, (system + "\n\n" + user).c_str(), &out);
        if (rc != 1 || !out) {
            // Хост может вернуть текст ошибки облака в out_response даже при
            // неудаче (rc=0): используем его, чтобы распознать rate-limit.
            if (out) {
                error = out;
                g_api->free_string(g_host, out);
            } else {
                error = "LLM вернул ошибку (код " + std::to_string(rc) + ")";
            }
            return false;
        }
        response = out;
        g_api->free_string(g_host, out);
        return true;
    });
    g_worker->start();

    g_ui_deps.worker = g_worker.get();
    g_ui_deps.on_save = [](const Config& cfg) {
        g_config = cfg;
        save_config(cfg);
        if (g_worker) {
            g_worker->post(Command{CmdType::ReloadConfig, config_to_json(cfg).dump()});
        }
    };
    g_ui_deps.on_close = []() {
        if (g_api && g_host && g_window) {
            g_api->window_set_visible(g_host, g_window, 0);
        }
    };

    // Профили настроек плагина (автономны от профилей основного приложения).
    g_ui_deps.active_profile = g_active_profile;
    g_ui_deps.list_profiles = []() {
        return list_profiles(g_profiles_data_dir);
    };
    g_ui_deps.profile_load = [](const std::string& name) -> Config {
        g_active_profile = name;
        set_active_profile(g_profiles_data_dir, name);
        return load_profile(g_profiles_data_dir, name);
    };
    g_ui_deps.profile_save = [](const std::string& name, const Config& cfg) {
        g_active_profile = name;
        save_profile(g_profiles_data_dir, name, cfg);
    };
    g_ui_deps.profile_delete = [](const std::string& name) {
        delete_profile(g_profiles_data_dir, name);
        if (name == g_active_profile) {
            const std::vector<std::string> list = list_profiles(g_profiles_data_dir);
            g_active_profile = list.empty() ? "" : list[0];
        }
    };
    g_ui_deps.profile_active = []() {
        return g_active_profile;
    };

    // Команды
    g_api->command_register(g_host, "news_rewriter_run", cmd_run, nullptr,
                            "Run News Rewriter crawl", nullptr);
    g_api->command_register(g_host, "news_rewriter_open", cmd_open_window, nullptr,
                            "Open News Rewriter window", nullptr);
    g_api->command_register(g_host, "news_rewriter_about", cmd_about, nullptr,
                            "About News Rewriter", nullptr);

    // Меню
    g_menu = g_api->menu_add(g_host, "Agents");
    if (g_menu) {
        g_api->menu_add_item(g_host, g_menu, "Open News Rewriter",
                             "news_rewriter_open", nullptr);
        g_api->menu_add_item(g_host, g_menu, "Run Crawl Now",
                             "news_rewriter_run", nullptr);
        g_api->menu_add_separator(g_host, g_menu);
        g_api->menu_add_item(g_host, g_menu, "About", "news_rewriter_about", nullptr);
    }

    // Окно (рисуется в ll_plugin_render)
    g_window = g_api->window_register(g_host, "news_rewriter", "News Rewriter");

    return 0;
}

LLAMA_PLUGIN_EXPORT void ll_plugin_render(void) {
    if (!g_api || !g_host || !g_window) return;
    if (g_api->window_is_visible(g_host, g_window) != 1) return;
    render_news_rewriter_window(g_ui_deps);
}

LLAMA_PLUGIN_EXPORT void ll_plugin_shutdown(void) {
    if (g_worker) {
        g_worker->stop_and_join();
        g_worker.reset();
    }
    if (g_api && g_host) {
        save_config(g_config);
        g_api->log(g_host, LLAMA_LOG_INFO, "news_rewriter: выгрузка");
    }
    g_host = nullptr;
    g_api = nullptr;
    g_window = nullptr;
    g_menu = nullptr;
    g_ui_deps.worker = nullptr;
    g_ui_deps.on_save = nullptr;
    g_ui_deps.on_close = nullptr;
}

} // extern "C"

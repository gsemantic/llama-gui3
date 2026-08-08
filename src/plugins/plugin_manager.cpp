#include "plugins/plugin_manager.h"
#include "plugins/plugin_api.h"

#include "ui/command_manager.h"
#include "ui/window_manager.h"
#include "ui/advanced_menu_system.h"
#include "ui/dialog_manager.h"
#include "ui/chat_interface.h"
#include "core/state_manager.h"
#include "core/settings.h"
#include "core/llama_interface.h"
#include "core/rag_manager.h"
#include "core/version.h"
#include "core/openrouter_client.h"
#include "core/env_manager.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unordered_map>

#ifdef __linux__
#include <unistd.h>
#endif

#ifdef _WIN32
    #include <windows.h>
    #define DL_HANDLE HMODULE
    #define DL_LOAD(name) LoadLibraryA(name)
    #define DL_SYM(handle, name) ((void*)GetProcAddress((HMODULE)(handle), (name)))
    #define DL_CLOSE(handle) FreeLibrary((HMODULE)(handle))
    #define DL_ERR() "Windows error"
#else
    #include <dlfcn.h>
    #define DL_HANDLE void*
    #define DL_LOAD(name) dlopen((name), RTLD_LAZY | RTLD_GLOBAL)
    #define DL_SYM(handle, name) dlsym((handle), (name))
    #define DL_CLOSE(handle) dlclose((handle))
    #define DL_ERR() dlerror()
#endif

namespace llama_gui {
namespace plugin {

// Внутренние структуры хоста (одна единица трансляции — plugin_manager.cpp).

// Первый (закрытый) блок: forward-объявления внутренних структур.
namespace {
    struct PluginHandle;
    struct LoadedPlugin;
    struct PluginHostData;
}

// Полное определение менеджера — требуется функциям хоста ниже.
class PluginImpl {
public:
    PluginSubsystems subsystems;
    std::vector<std::unique_ptr<LoadedPlugin>> plugins;
    std::unordered_map<std::string, std::string> kv_store;
    bool initialized = false;
};

namespace {

/** Общий хендл, хранящий имя сущности (меню/окна/команды). */
struct PluginHandle {
    std::string name;
};

struct LoadedPlugin {
    PluginInfo info;
    DL_HANDLE handle = nullptr;

    const char* (*api_version_fn)() = nullptr;
    const LlamaPluginInfo* (*info_fn)() = nullptr;
    int (*init_fn)(LlamaPluginHost*, const LlamaHostApi*) = nullptr;
    void (*render_fn)() = nullptr;
    void (*shutdown_fn)() = nullptr;

    PluginHostData* host_data = nullptr;

    // Хендлы, созданные плагином (для корректной выгрузки).
    std::vector<std::string> menu_names;
    std::vector<std::unique_ptr<PluginHandle>> menu_handles;
    std::vector<std::unique_ptr<PluginHandle>> window_handles;
    std::vector<std::unique_ptr<PluginHandle>> command_handles;

    // Пункты меню, добавленные плагином, сгруппированные по ключу меню.
    // Хранятся, чтобы восстановить их после перестройки меню приложением
    // (rebuildModernMenu, например при смене языка), когда пункты плагина
    // пропадают из пересозданных меню.
    std::unordered_map<std::string, std::vector<ui::AdvancedMenuItem>> menu_items;
};

/** Непрозрачный хост-хендл плагина (передаётся как LlamaPluginHost*). */
struct PluginHostData {
    PluginImpl* manager = nullptr;
    LoadedPlugin* plugin = nullptr;
};

const char* log_level_str(int level) {
    switch (level) {
        case LLAMA_LOG_DEBUG:   return "DEBUG";
        case LLAMA_LOG_INFO:    return "INFO";
        case LLAMA_LOG_WARNING: return "WARNING";
        default:                return "ERROR";
    }
}

inline PluginHostData* to_pd(LlamaPluginHost* host) {
    return reinterpret_cast<PluginHostData*>(host);
}

// ============================================================================
// Реализация функций хоста (таблица LlamaHostApi)
// ============================================================================

void host_log(LlamaPluginHost* host, int level, const char* message) {
    auto* pd = to_pd(host);
    const std::string name = (pd && pd->plugin) ? pd->plugin->info.name : "?";
    std::cerr << "[Plugin:" << name << "] " << log_level_str(level)
              << ": " << (message ? message : "") << std::endl;
}

LlamaPluginMenu* host_menu_add(LlamaPluginHost* host, const char* menu_name) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !menu_name) return nullptr;
    auto* ms = pd->manager->subsystems.menu_system;
    if (!ms) return nullptr;

    const std::string name = menu_name;

    auto handle = std::make_unique<PluginHandle>();
    // Пытаемся найти существующее меню: сначала по стабильному ключу
    // (например "Agents"), затем по локализованному имени.
    ui::AdvancedMenu* existing = ms->getMenuByKey(name);
    if (!existing) existing = ms->getMenu(name);
    if (existing) {
        // Плагин добавляет пункты в существующее меню — новое верхнеуровневое
        // меню не создаём. В хендле храним ключ меню для дальнейших вызовов.
        handle->name = existing->menu_key.empty() ? existing->name : existing->menu_key;
    } else {
        // Новое верхнеуровневое меню плагина
        ms->addMenu(name, {});
        handle->name = name;
    }

    LlamaPluginMenu* result = reinterpret_cast<LlamaPluginMenu*>(handle.get());
    if (pd->plugin) {
        pd->plugin->menu_names.push_back(name);
        pd->plugin->menu_handles.push_back(std::move(handle));
    }
    return result;
}

void host_menu_add_item(LlamaPluginHost* host, LlamaPluginMenu* menu,
                        const char* item_name, const char* command_name,
                        const char* shortcut) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !menu || !item_name) return;
    auto* ms = pd->manager->subsystems.menu_system;
    if (!ms) return;

    auto* handle = reinterpret_cast<PluginHandle*>(menu);
    ui::AdvancedMenuItem item;
    item.name = item_name;
    if (command_name) item.command = command_name;
    if (shortcut) item.shortcut = shortcut;
    item.type = ui::AdvancedMenuItemType::Item;
    ms->addMenuItem(handle->name, item);

    // Сохраняем пункт для восстановления после перестройки меню приложением.
    if (pd->plugin) {
        pd->plugin->menu_items[handle->name].push_back(std::move(item));
    }
}

void host_menu_add_separator(LlamaPluginHost* host, LlamaPluginMenu* menu) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !menu) return;
    auto* ms = pd->manager->subsystems.menu_system;
    if (!ms) return;

    auto* handle = reinterpret_cast<PluginHandle*>(menu);
    // Разделитель помечаем именем-маркером по имени плагина, чтобы при
    // восстановлении/удалении отличать его от разделителей приложения и других
    // плагинов. Маркер не отображается: тип Separator рендерится как
    // ImGui::Separator(), имя игнорируется.
    ui::AdvancedMenuItem separator;
    separator.name = pd->plugin
                         ? ("__plugin_sep__" + pd->plugin->info.name)
                         : "__plugin_sep__";
    separator.type = ui::AdvancedMenuItemType::Separator;
    ms->addMenuItem(handle->name, separator);

    // Сохраняем разделитель для восстановления после перестройки меню приложением.
    if (pd->plugin) {
        pd->plugin->menu_items[handle->name].push_back(std::move(separator));
    }
}

LlamaPluginCommand* host_command_register(LlamaPluginHost* host, const char* name,
                                          LlamaPluginCallback callback,
                                          void* user_data,
                                          const char* description,
                                          const char* shortcut) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !name || !callback) return nullptr;
    auto* cm = pd->manager->subsystems.command_manager;
    if (!cm) return nullptr;

    const std::string cmd_name = name;
    if (cm->isCommandRegistered(cmd_name)) {
        if (pd->manager) host_log(host, LLAMA_LOG_WARNING,
            "command name is already registered, skipping");
        return nullptr;
    }

    auto command = std::make_unique<ui::FunctionalCommand>(
        cmd_name,
        [callback, user_data]() { callback(user_data); },
        description ? description : "",
        shortcut ? shortcut : "");
    if (!cm->registerCommand(cmd_name, std::move(command))) return nullptr;

    if (shortcut && *shortcut) {
        cm->registerShortcut(shortcut, cmd_name);
    }

    auto handle = std::make_unique<PluginHandle>();
    handle->name = cmd_name;
    LlamaPluginCommand* result = reinterpret_cast<LlamaPluginCommand*>(handle.get());
    if (pd->plugin) {
        pd->plugin->command_handles.push_back(std::move(handle));
    }
    return result;
}

int host_command_execute(LlamaPluginHost* host, const char* name) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !name) return 0;
    auto* cm = pd->manager->subsystems.command_manager;
    if (!cm) return 0;
    auto result = cm->executeCommand(name);
    return result.success ? 1 : 0;
}

LlamaPluginWindow* host_window_register(LlamaPluginHost* host,
                                        const char* wm_name,
                                        const char* imgui_title) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !wm_name) return nullptr;
    auto* wm = pd->manager->subsystems.window_manager;
    if (!wm) return nullptr;

    const std::string wname = wm_name;
    wm->addWindow(wname, false);
    if (imgui_title && *imgui_title) {
        wm->setImGuiName(wname, imgui_title);
    }

    auto handle = std::make_unique<PluginHandle>();
    handle->name = wname;
    LlamaPluginWindow* result = reinterpret_cast<LlamaPluginWindow*>(handle.get());
    if (pd->plugin) {
        pd->plugin->window_handles.push_back(std::move(handle));
    }
    return result;
}

void host_window_set_visible(LlamaPluginHost* host, LlamaPluginWindow* window,
                             int visible) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !window) return;
    auto* wm = pd->manager->subsystems.window_manager;
    if (!wm) return;
    auto* handle = reinterpret_cast<PluginHandle*>(window);
    wm->setWindowVisible(handle->name, visible != 0);
}

int host_window_is_visible(LlamaPluginHost* host, LlamaPluginWindow* window) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !window) return 0;
    auto* wm = pd->manager->subsystems.window_manager;
    if (!wm) return 0;
    auto* handle = reinterpret_cast<PluginHandle*>(window);
    return wm->isWindowVisible(handle->name) ? 1 : 0;
}

void host_dialog_info(LlamaPluginHost* host, const char* title, const char* message) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager) return;
    auto* dm = pd->manager->subsystems.dialog_manager;
    if (!dm) return;
    dm->showInfo(title ? title : "", message ? message : "");
}

void host_dialog_warning(LlamaPluginHost* host, const char* title, const char* message) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager) return;
    auto* dm = pd->manager->subsystems.dialog_manager;
    if (!dm) return;
    dm->showWarning(title ? title : "", message ? message : "");
}

void host_dialog_error(LlamaPluginHost* host, const char* title, const char* message) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager) return;
    auto* dm = pd->manager->subsystems.dialog_manager;
    if (!dm) return;
    dm->showError(title ? title : "", message ? message : "");
}

void host_dialog_confirmation(LlamaPluginHost* host, const char* title,
                              const char* message,
                              LlamaPluginCallback callback, void* user_data) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager) return;
    auto* dm = pd->manager->subsystems.dialog_manager;
    if (!dm) return;
    dm->showConfirmation(
        title ? title : "", message ? message : "",
        [callback, user_data](bool) {
            if (callback) callback(user_data);
        });
}

char* host_settings_get(LlamaPluginHost* host, const char* key) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !key) return nullptr;
    auto* s = pd->manager->subsystems.settings;
    if (!s) return nullptr;
    if (!s->has_custom_setting(key)) return nullptr;
    return strdup(s->get_custom_setting(key).c_str());
}

int host_settings_set(LlamaPluginHost* host, const char* key, const char* json_value) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !key || !json_value) return 0;
    auto* s = pd->manager->subsystems.settings;
    if (!s) return 0;
    s->set_custom_setting(key, json_value);
    return 1;
}

char* host_state_get(LlamaPluginHost* host, const char* key) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !key) return nullptr;
    auto it = pd->manager->kv_store.find(key);
    if (it == pd->manager->kv_store.end()) return nullptr;
    return strdup(it->second.c_str());
}

int host_state_set(LlamaPluginHost* host, const char* key, const char* json_value) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !key || !json_value) return 0;
    pd->manager->kv_store[key] = json_value;
    return 1;
}

int host_chat_send_message(LlamaPluginHost* host, const char* message) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !message) return 0;
    auto* ci = pd->manager->subsystems.chat_interface;
    if (!ci) return 0;
    return ci->submit_message(message) ? 1 : 0;
}

int host_chat_add_message(LlamaPluginHost* host, const char* role, const char* content) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !content) return 0;
    auto* ci = pd->manager->subsystems.chat_interface;
    if (!ci) return 0;
    const std::string r = role ? role : "";
    if (r == "assistant") {
        ci->add_assistant_message(content);
    } else {
        ci->add_user_message(content);
    }
    return 1;
}

int host_llm_is_connected(LlamaPluginHost* host) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager) return 0;

    // Локальный сервер здоров — LLM доступен.
    auto* li = pd->manager->subsystems.llama_interface;
    if (li && li->is_server_healthy()) return 1;

    // Локальный сервер недоступен — проверяем облачного провайдера
    // (те же условия, что и в host_llm_complete_local_or_cloud).
    auto* settings = pd->manager->subsystems.settings;
    if (settings) {
        const auto& cp = settings->cloud_provider();
        if (cp.enabled && !cp.model_id.empty()) {
            const std::string key_name =
                core::EnvManager::cloud_provider_api_key_name(cp.provider_name, cp.endpoint_url);
            const std::string api_key =
                core::EnvManager::read_key(key_name, settings->get_profiles_directory());
            if (!api_key.empty()) return 1;
        }
    }
    return 0;
}

namespace {

// Если локальный сервер недоступен — пробуем облачного провайдера.
int host_llm_complete_local_or_cloud(LlamaPluginHost* host, PluginHostData* pd,
                                     const char* prompt, char** out_response) {
    auto* li = pd->manager->subsystems.llama_interface;

    if (li && li->is_server_healthy()) {
        try {
            core::ChatCompletionRequest req;
            req.model = "local";
            req.messages.emplace_back(core::MessageRole::User, prompt);
            req.stream = false;
            auto future = li->create_chat_completion_async(req);
            auto response = future.get();
            if (!response.choices.empty() &&
                !response.choices[0].message.content.empty()) {
                *out_response = strdup(response.choices[0].message.content.c_str());
                return 1;
            }
        } catch (...) {
            // Локальный сервер упал во время запроса — пробуем облако.
        }
    }

    auto* settings = pd->manager->subsystems.settings;
    if (!settings) return 0;
    const auto& cp = settings->cloud_provider();
    if (!cp.enabled || cp.model_id.empty()) return 0;

    const std::string key_name =
        core::EnvManager::cloud_provider_api_key_name(cp.provider_name, cp.endpoint_url);
    const std::string api_key =
        core::EnvManager::read_key(key_name, settings->get_profiles_directory());
    if (api_key.empty()) {
        host_log(host, LLAMA_LOG_WARNING,
                 "cloud fallback: API key not set for cloud provider");
        return 0;
    }

    core::OpenRouterClient client(api_key);
    client.set_timeout(cp.timeout_ms);
    if (!cp.endpoint_url.empty()) client.set_base_url(cp.endpoint_url);

    core::OpenRouterRequestParams params;
    params.model = cp.model_id;
    params.max_tokens = cp.max_output_tokens;  // 0 = не ограничено (см. build_completion_body)
    params.temperature = settings->chat().temperature;
    params.top_p = settings->chat().top_p;
    params.stream = false;

    if (!settings->chat().default_system_prompt.empty()) {
        core::OpenRouterRequestParams::Message sys;
        sys.role = "system";
        sys.content = settings->chat().default_system_prompt;
        params.messages.push_back(std::move(sys));
    }
    core::OpenRouterRequestParams::Message user;
    user.role = "user";
    user.content = prompt;
    params.messages.push_back(std::move(user));

    try {
        auto response = client.complete(params);
        if (response.success && !response.content.empty()) {
            *out_response = strdup(response.content.c_str());
            return 1;
        }
        if (!response.error.empty()) {
            host_log(host, LLAMA_LOG_WARNING,
                     ("cloud fallback failed: " + response.error).c_str());
        }
    } catch (const std::exception& e) {
        host_log(host, LLAMA_LOG_WARNING,
                 ("cloud fallback failed: " + std::string(e.what())).c_str());
    }
    return 0;
}

} // namespace

int host_llm_complete(LlamaPluginHost* host, const char* prompt, char** out_response) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !prompt || !out_response) return 0;
    return host_llm_complete_local_or_cloud(host, pd, prompt, out_response);
}

char* host_rag_search(LlamaPluginHost* host, const char* query, int k,
                      const char* path_filter) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !query) return nullptr;
    auto* rm = pd->manager->subsystems.rag_manager;
    if (!rm) return nullptr;

    auto chunks = rm->search_hybrid(query, k > 0 ? k : 5, path_filter ? path_filter : "");
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& c : chunks) {
        nlohmann::json j;
        j["content"] = c.content;
        j["document_id"] = c.document_id;
        j["chunk_index"] = c.chunk_index;
        j["file_path"] = c.file_path;
        j["symbol_name"] = c.symbol_name;
        j["start_line"] = c.start_line;
        j["end_line"] = c.end_line;
        arr.push_back(std::move(j));
    }
    return strdup(arr.dump().c_str());
}

int host_rag_process_document(LlamaPluginHost* host, const char* path) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !path) return 0;
    auto* rm = pd->manager->subsystems.rag_manager;
    if (!rm) return 0;
    return rm->process_document(path) ? 1 : 0;
}

int host_rag_embedding(LlamaPluginHost* host, const char* text,
                       float* out_vec, int max_dim, int* out_dim) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !text || !out_vec || !out_dim) return 0;
    auto* rm = pd->manager->subsystems.rag_manager;
    if (!rm) return 0;
    auto vec = rm->generate_embedding(text);
    if (vec.empty()) return 0;
    int n = static_cast<int>(vec.size());
    if (max_dim < n) n = max_dim;
    std::memcpy(out_vec, vec.data(), static_cast<size_t>(n) * sizeof(float));
    *out_dim = n;
    return 1;
}

int host_rag_index_count(LlamaPluginHost* host) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager) return 0;
    auto* rm = pd->manager->subsystems.rag_manager;
    if (!rm) return 0;
    return static_cast<int>(rm->get_external_chunks_count());
}

char* host_rag_build_prompt(LlamaPluginHost* host, const char* query, int k,
                            const char* path_filter) {
    auto* pd = to_pd(host);
    if (!pd || !pd->manager || !query) return nullptr;
    auto* rm = pd->manager->subsystems.rag_manager;
    if (!rm) return nullptr;

    auto chunks = rm->search_hybrid(query, k > 0 ? k : 5, path_filter ? path_filter : "");
    bool is_cloud = false;
    if (pd->manager->subsystems.settings) {
        is_cloud = pd->manager->subsystems.settings->cloud_provider().enabled;
    }
    auto prompt = rm->build_rag_prompt(query, chunks, is_cloud);
    return strdup(prompt.c_str());
}

const char* host_path_config_dir(LlamaPluginHost* host) {
    auto* pd = to_pd(host);
    return (pd && pd->manager) ? pd->manager->subsystems.config_dir.c_str() : "";
}

const char* host_path_data_dir(LlamaPluginHost* host) {
    auto* pd = to_pd(host);
    return (pd && pd->manager) ? pd->manager->subsystems.data_dir.c_str() : "";
}

const char* host_path_plugins_dir(LlamaPluginHost* host) {
    auto* pd = to_pd(host);
    return (pd && pd->manager) ? pd->manager->subsystems.plugins_dir.c_str() : "";
}

void host_free_string(LlamaPluginHost*, char* str) {
    free(str);
}

void host_free_float_array(LlamaPluginHost*, float* arr) {
    free(arr);
}

const char* host_app_version() {
    static const std::string version = core::getVersionFull();
    return version.c_str();
}

const LlamaHostApi& host_api_table() {
    static const LlamaHostApi api = []() {
        LlamaHostApi a{};
        a.size = sizeof(LlamaHostApi);
        a.app_version = host_app_version();

        a.log = host_log;

        a.menu_add = host_menu_add;
        a.menu_add_item = host_menu_add_item;
        a.menu_add_separator = host_menu_add_separator;

        a.command_register = host_command_register;
        a.command_execute = host_command_execute;

        a.window_register = host_window_register;
        a.window_set_visible = host_window_set_visible;
        a.window_is_visible = host_window_is_visible;

        a.dialog_info = host_dialog_info;
        a.dialog_warning = host_dialog_warning;
        a.dialog_error = host_dialog_error;
        a.dialog_confirmation = host_dialog_confirmation;

        a.settings_get = host_settings_get;
        a.settings_set = host_settings_set;

        a.state_get = host_state_get;
        a.state_set = host_state_set;

        a.chat_send_message = host_chat_send_message;
        a.chat_add_message = host_chat_add_message;
        a.llm_is_connected = host_llm_is_connected;
        a.llm_complete = host_llm_complete;

        a.rag_search = host_rag_search;
        a.rag_process_document = host_rag_process_document;
        a.rag_embedding = host_rag_embedding;
        a.rag_index_count = host_rag_index_count;
        a.rag_build_prompt = host_rag_build_prompt;

        a.path_config_dir = host_path_config_dir;
        a.path_data_dir = host_path_data_dir;
        a.path_plugins_dir = host_path_plugins_dir;

        a.free_string = host_free_string;
        a.free_float_array = host_free_float_array;
        return a;
    }();
    return api;
}

} // namespace (host impl)

// ============================================================================
// Внутренняя реализация менеджера
// ============================================================================

namespace {

std::string plugin_extension() {
#ifdef _WIN32
    return ".dll";
#else
    return ".so";
#endif
}

std::vector<std::string> scan_plugin_files(const std::string& dir) {
    std::vector<std::string> result;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return result;
    }
    const std::string ext = plugin_extension();
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string filename = entry.path().filename().string();
        if (filename.size() > ext.size() &&
            filename.compare(filename.size() - ext.size(), ext.size(), ext) == 0) {
            result.push_back(entry.path().string());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string executable_directory() {
#ifdef __linux__
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        std::string path(buf);
        auto pos = path.find_last_of('/');
        if (pos != std::string::npos) return path.substr(0, pos);
    }
#endif
    return ".";
}

std::string strip_lib_prefix(const std::string& filename) {
    if (filename.size() > 3 && filename.rfind("lib", 0) == 0) {
        return filename.substr(3);
    }
    return filename;
}

// Ищем plugin.json рядом с библиотекой: <name>.json, <name>.plugin.json
std::string find_manifest_path(const std::string& plugin_path) {
    const std::string dir = std::filesystem::path(plugin_path).parent_path().string();
    const std::string stem = std::filesystem::path(plugin_path).stem().string();
    const std::string base = strip_lib_prefix(stem);

    std::vector<std::string> candidates;
    candidates.push_back(dir + "/" + stem + ".json");
    candidates.push_back(dir + "/" + base + ".json");
    candidates.push_back(dir + "/" + base + ".plugin.json");
    candidates.push_back(dir + "/plugin.json");

    std::error_code ec;
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c, ec)) return c;
    }
    return "";
}

PluginManifest parse_manifest(const std::string& path) {
    PluginManifest m;
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) return m;

    try {
        std::ifstream file(path);
        if (!file.is_open()) return m;

        nlohmann::json j;
        file >> j;

        if (!j.is_object()) {
            std::cerr << "[PluginManager] Invalid manifest (not an object): " << path << std::endl;
            return m;
        }

        m.present = true;
        if (j.contains("name") && j["name"].is_string()) m.name = j["name"].get<std::string>();
        if (j.contains("version") && j["version"].is_string()) m.version = j["version"].get<std::string>();
        if (j.contains("description") && j["description"].is_string()) m.description = j["description"].get<std::string>();
        if (j.contains("author") && j["author"].is_string()) m.author = j["author"].get<std::string>();
        if (j.contains("api_version") && j["api_version"].is_string()) m.api_version = j["api_version"].get<std::string>();

        if (j.contains("permissions") && j["permissions"].is_array()) {
            for (const auto& p : j["permissions"]) {
                if (p.is_string()) m.permissions.push_back(p.get<std::string>());
            }
        }
        if (j.contains("capabilities") && j["capabilities"].is_array()) {
            for (const auto& c : j["capabilities"]) {
                if (c.is_string()) m.capabilities.push_back(c.get<std::string>());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[PluginManager] Failed to parse manifest " << path
                  << ": " << e.what() << std::endl;
        return PluginManifest{};
    }

    return m;
}

} // namespace

// ============================================================================
// PluginManager
// ============================================================================

PluginManager::PluginManager() : impl_(std::make_unique<PluginImpl>()) {}

PluginManager::~PluginManager() {
    shutdown();
}

bool PluginManager::initialize(const PluginSubsystems& subsystems) {
    if (impl_->initialized) return true;
    impl_->subsystems = subsystems;

    // Ищем директорию плагинов в нескольких местах (cwd / рядом с exe).
    std::vector<std::string> candidate_dirs;
    if (!subsystems.plugins_dir.empty()) candidate_dirs.push_back(subsystems.plugins_dir);
    candidate_dirs.push_back("plugins");
    candidate_dirs.push_back(executable_directory() + "/plugins");

    std::vector<std::string> files;
    for (const auto& dir : candidate_dirs) {
        auto found = scan_plugin_files(dir);
        if (!found.empty()) {
            files = std::move(found);
            impl_->subsystems.plugins_dir = dir;
            break;
        }
    }

    int loaded = 0;
    int failed = 0;
    for (const auto& path : files) {
        if (load_plugin_file(path)) {
            loaded++;
        } else {
            failed++;
        }
    }

    impl_->initialized = true;
    std::cout << "[PluginManager] Loaded " << loaded << " plugin(s)"
              << (failed ? ", " + std::to_string(failed) + " failed" : "")
              << " from " << impl_->subsystems.plugins_dir << std::endl;
    return true;
}

bool PluginManager::load_plugin_file(const std::string& path) {
    DL_HANDLE handle = DL_LOAD(path.c_str());
    if (!handle) {
        std::cerr << "[PluginManager] Failed to load " << path << ": " << DL_ERR() << std::endl;
        return false;
    }

    auto api_version_fn = reinterpret_cast<const char* (*)()>(DL_SYM(handle, "ll_plugin_api_version"));
    auto info_fn = reinterpret_cast<const LlamaPluginInfo* (*)()>(DL_SYM(handle, "ll_plugin_info"));
    auto init_fn = reinterpret_cast<int (*)(LlamaPluginHost*, const LlamaHostApi*)>(DL_SYM(handle, "ll_plugin_init"));
    auto render_fn = reinterpret_cast<void (*)()>(DL_SYM(handle, "ll_plugin_render"));
    auto shutdown_fn = reinterpret_cast<void (*)()>(DL_SYM(handle, "ll_plugin_shutdown"));

    if (!api_version_fn || !info_fn || !init_fn) {
        std::cerr << "[PluginManager] Missing required exports in " << path << std::endl;
        DL_CLOSE(handle);
        return false;
    }

    const char* plugin_api_version = api_version_fn();
    if (!plugin_api_version || std::string(plugin_api_version) != LLAMA_PLUGIN_API_VERSION) {
        std::cerr << "[PluginManager] API version mismatch for " << path
                  << " (plugin: " << (plugin_api_version ? plugin_api_version : "?")
                  << ", host: " << LLAMA_PLUGIN_API_VERSION << ")" << std::endl;
        DL_CLOSE(handle);
        return false;
    }

    const LlamaPluginInfo* info = info_fn();
    if (!info || !info->name || !*info->name) {
        std::cerr << "[PluginManager] Invalid plugin info from " << path << std::endl;
        DL_CLOSE(handle);
        return false;
    }

    auto plugin = std::make_unique<LoadedPlugin>();
    plugin->info.name = info->name;
    plugin->info.version = info->version ? info->version : "0.0.0";
    plugin->info.description = info->description ? info->description : "";
    plugin->info.author = info->author ? info->author : "";
    plugin->info.path = path;
    plugin->info.is_loaded = true;
    plugin->handle = handle;
    plugin->api_version_fn = api_version_fn;
    plugin->info_fn = info_fn;
    plugin->init_fn = init_fn;
    plugin->render_fn = render_fn;
    plugin->shutdown_fn = shutdown_fn;

    // Манифест plugin.json рядом с библиотекой (информационный; пермиссии не применяются)
    const std::string manifest_path = find_manifest_path(path);
    if (!manifest_path.empty()) {
        plugin->info.manifest = parse_manifest(manifest_path);
        if (plugin->info.manifest.present) {
            if (!plugin->info.manifest.api_version.empty() &&
                plugin->info.manifest.api_version != LLAMA_PLUGIN_API_VERSION) {
                std::cerr << "[PluginManager] Manifest API version mismatch for " << path
                          << " (manifest: " << plugin->info.manifest.api_version
                          << ", host: " << LLAMA_PLUGIN_API_VERSION << ")" << std::endl;
            }
            if (!plugin->info.manifest.name.empty() &&
                plugin->info.manifest.name != plugin->info.name) {
                std::cerr << "[PluginManager] Manifest name '" << plugin->info.manifest.name
                          << "' differs from ll_plugin_info name '" << plugin->info.name
                          << "' for " << path << std::endl;
            }
            std::cout << "[PluginManager] Manifest: " << plugin->info.manifest.name
                      << " v" << plugin->info.manifest.version
                      << " (api " << plugin->info.manifest.api_version << ")"
                      << " from " << manifest_path << std::endl;
        }
    }

    plugin->host_data = new PluginHostData{impl_.get(), plugin.get()};

    const int rc = plugin->init_fn(
        reinterpret_cast<LlamaPluginHost*>(plugin->host_data), &host_api_table());
    if (rc != 0) {
        std::cerr << "[PluginManager] Plugin " << plugin->info.name
                  << " failed to initialize (code " << rc << ")" << std::endl;
        if (plugin->shutdown_fn) plugin->shutdown_fn();
        DL_CLOSE(plugin->handle);
        delete plugin->host_data;
        return false;
    }

    const std::string plugin_name = plugin->info.name;
    const std::string plugin_version = plugin->info.version;
    impl_->plugins.push_back(std::move(plugin));
    std::cout << "[PluginManager] Loaded plugin: " << plugin_name
              << " v" << plugin_version << " from " << path << std::endl;
    return true;
}

void PluginManager::render_plugins() {
    if (!impl_ || !impl_->initialized) return;

    for (auto& up : impl_->plugins) {
        LoadedPlugin* p = up.get();
        if (!p || !p->render_fn) continue;

        // Перерегистрация меню — идемпотентна, устойчива к перестройке меню
        // приложением (например, при смене языка). Ищем и по ключу, и по имени,
        // чтобы не создавать дубликат существующего меню (например "Agents").
        if (impl_->subsystems.menu_system) {
            auto* ms = impl_->subsystems.menu_system;
            for (const auto& menu_name : p->menu_names) {
                if (!ms->getMenuByKey(menu_name) && !ms->getMenu(menu_name)) {
                    ms->addMenu(menu_name, {});
                }
            }

            // Восстанавливаем пункты меню плагина: после перестройки меню
            // приложением (rebuildModernMenu, например при смене языка) пункты
            // плагина пропадают. Чтобы не дублировать их (в т.ч. когда несколько
            // плагинов добавляют пункты в одно меню), каждый кадр сначала
            // удаляем ранее добавленные пункты плагина, затем дозаписываем блок
            // в конец меню заново — структура меню детерминирована и не растёт.
            for (const auto& [menu_key, items] : p->menu_items) {
                if (items.empty()) continue;
                ui::AdvancedMenu* menu = ms->getMenuByKey(menu_key);
                if (!menu) menu = ms->getMenu(menu_key);
                if (!menu) continue;

                // Удаляем ранее добавленные пункты плагина. Обычные пункты
                // ищем по типу/имени/команде, разделители — по имени-маркеру.
                for (const auto& item : items) {
                    auto& list = menu->items;
                    for (auto it = list.begin(); it != list.end(); ++it) {
                        const bool match =
                            (it->type == item.type) &&
                            (item.type == ui::AdvancedMenuItemType::Separator
                                 ? (it->name == item.name)
                                 : (it->name == item.name && it->command == item.command));
                        if (match) {
                            list.erase(it);
                            break;
                        }
                    }
                }

                // Дозаписываем блок пунктов плагина в конец меню.
                for (const auto& item : items) {
                    menu->items.push_back(item);
                }
            }
        }

        p->render_fn();
    }
}

void PluginManager::shutdown() {
    if (!impl_) return;

    for (auto it = impl_->plugins.rbegin(); it != impl_->plugins.rend(); ++it) {
        LoadedPlugin* p = it->get();

        // Убираем команды плагина, чтобы их колбэки не остались висеть.
        if (impl_->subsystems.command_manager) {
            for (auto& h : p->command_handles) {
                impl_->subsystems.command_manager->unregisterCommand(h->name);
            }
        }

        if (p->shutdown_fn) {
            try {
                p->shutdown_fn();
            } catch (...) {}
        }
        if (p->handle) {
            DL_CLOSE(p->handle);
        }
        delete p->host_data;
    }
    impl_->plugins.clear();
    impl_->kv_store.clear();
    impl_->initialized = false;
    std::cout << "[PluginManager] All plugins unloaded" << std::endl;
}

bool PluginManager::is_initialized() const {
    return impl_ && impl_->initialized;
}

std::vector<PluginInfo> PluginManager::list_plugins() const {
    std::vector<PluginInfo> result;
    if (!impl_) return result;
    result.reserve(impl_->plugins.size());
    for (const auto& up : impl_->plugins) {
        result.push_back(up->info);
    }
    return result;
}

bool PluginManager::is_plugin_loaded(const std::string& name) const {
    if (!impl_) return false;
    for (const auto& up : impl_->plugins) {
        if (up->info.name == name) return true;
    }
    return false;
}

} // namespace plugin
} // namespace llama_gui

#include "agents/plugin_loader.h"
#include "agents/plugin_c_api.h"
#include <iostream>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
    #include <windows.h>
    #define DLOPEN_HANDLE HMODULE
#else
    #include <dlfcn.h>
    #define DLOPEN_HANDLE void*
#endif

namespace agents {

/**
 * @brief Внутренняя реализация PluginLoader
 */
class PluginLoader::Impl {
public:
    mutable std::mutex mutex;
    std::unordered_map<std::string, PluginInfo> plugins;
};

// ============================================================================

PluginLoader::PluginLoader() : impl_(std::make_unique<Impl>()) {}

PluginLoader::~PluginLoader() {
    unload_all();
}

std::string PluginLoader::get_plugin_extension() {
#ifdef _WIN32
    return ".dll";
#else
    return ".so";
#endif
}

bool PluginLoader::load_plugin(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    // Проверяем существование файла
    if (!std::filesystem::exists(path)) {
        std::cerr << "[PluginLoader] File not found: " << path << std::endl;
        return false;
    }

    // Загружаем библиотеку
    DLOPEN_HANDLE handle = nullptr;
#ifdef _WIN32
    handle = LoadLibraryA(path.c_str());
    if (!handle) {
        std::cerr << "[PluginLoader] Failed to load DLL: " << path << std::endl;
        return false;
    }
#else
    handle = dlopen(path.c_str(), RTLD_LAZY);
    if (!handle) {
        std::cerr << "[PluginLoader] Failed to load .so: " << path 
                  << " - " << dlerror() << std::endl;
        return false;
    }
#endif

    // Получаем функцию экспорта
    typedef PluginExports* (*GetExportsFn)();
    GetExportsFn get_exports = nullptr;

#ifdef _WIN32
    get_exports = (GetExportsFn)GetProcAddress(handle, "plugin_get_exports");
#else
    get_exports = (GetExportsFn)dlsym(handle, "plugin_get_exports");
#endif

    if (!get_exports) {
        std::cerr << "[PluginLoader] Missing plugin_get_exports: " << path << std::endl;
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return false;
    }

    // Получаем экспорты
    PluginExports* exports = get_exports();
    if (!exports) {
        std::cerr << "[PluginLoader] plugin_get_exports returned null: " 
                  << path << std::endl;
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return false;
    }

    // Проверяем версию API
    const char* api_version = exports->get_api_version();
    if (api_version == nullptr || std::string(api_version) != AGENT_PLUGIN_API_VERSION) {
        std::cerr << "[PluginLoader] API version mismatch for " << path 
                  << " (plugin: " << (api_version ? api_version : "unknown") 
                  << ", expected: " << AGENT_PLUGIN_API_VERSION << ")" << std::endl;
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return false;
    }

    // Получаем информацию о плагине
    PluginInfo info;
    info.name = exports->get_name ? exports->get_name() : "unknown";
    info.version = exports->get_version ? exports->get_version() : "0.0.0";
    info.api_version = api_version;
    info.path = path;
    info.description = "";  // TODO: Загрузка из plugin.json
    info.is_loaded = true;
    info.handle = handle;

    // Извлекаем имя файла из пути
    std::string filename = std::filesystem::path(path).filename().string();
    
    // Используем имя из плагина или имя файла
    std::string plugin_key = info.name;
    if (plugin_key == "unknown") {
        plugin_key = filename;
    }

    impl_->plugins[plugin_key] = info;

    std::cout << "[PluginLoader] Loaded plugin: " << info.name 
              << " v" << info.version << " from " << path << std::endl;

    return true;
}

bool PluginLoader::unload_plugin(const std::string& name) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto it = impl_->plugins.find(name);
    if (it == impl_->plugins.end()) {
        return false;
    }

    PluginInfo& info = it->second;

    if (info.handle) {
#ifdef _WIN32
        FreeLibrary((HMODULE)info.handle);
#else
        dlclose(info.handle);
#endif
    }

    impl_->plugins.erase(it);

    std::cout << "[PluginLoader] Unloaded plugin: " << name << std::endl;
    return true;
}

void PluginLoader::unload_all() {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    for (auto& pair : impl_->plugins) {
        PluginInfo& info = pair.second;
        if (info.handle) {
#ifdef _WIN32
            FreeLibrary((HMODULE)info.handle);
#else
            dlclose(info.handle);
#endif
        }
    }

    impl_->plugins.clear();
    std::cout << "[PluginLoader] Unloaded all plugins" << std::endl;
}

bool PluginLoader::is_plugin_loaded(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->plugins.count(name) > 0;
}

const PluginInfo* PluginLoader::get_plugin_info(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto it = impl_->plugins.find(name);
    if (it != impl_->plugins.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<PluginInfo> PluginLoader::list_plugins() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    std::vector<PluginInfo> result;
    result.reserve(impl_->plugins.size());

    for (const auto& pair : impl_->plugins) {
        result.push_back(pair.second);
    }

    return result;
}

bool PluginLoader::create_agent_from_plugin(const std::string& plugin_name,
                                             AgentRegistry* registry) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto it = impl_->plugins.find(plugin_name);
    if (it == impl_->plugins.end()) {
        std::cerr << "[PluginLoader] Plugin not found: " << plugin_name << std::endl;
        return false;
    }

    PluginInfo& info = it->second;
    if (!info.handle) {
        std::cerr << "[PluginLoader] Plugin not loaded: " << plugin_name << std::endl;
        return false;
    }

    // Получаем функцию создания агента
    typedef IAgent* (*CreateAgentFn)();
    CreateAgentFn create_agent = nullptr;

#ifdef _WIN32
    create_agent = (CreateAgentFn)GetProcAddress((HMODULE)info.handle, 
                                                  "plugin_create_agent");
#else
    create_agent = (CreateAgentFn)dlsym(info.handle, "plugin_create_agent");
#endif

    if (!create_agent) {
        std::cerr << "[PluginLoader] Missing plugin_create_agent in " 
                  << plugin_name << std::endl;
        return false;
    }

    // Создаём и регистрируем агента
    IAgent* agent = create_agent();
    if (!agent) {
        std::cerr << "[PluginLoader] Failed to create agent from " << plugin_name << std::endl;
        return false;
    }

    // Создаём unique_ptr с простым deleter
    auto agent_ptr = std::unique_ptr<IAgent>(agent);
    
    return registry->register_agent(std::move(agent_ptr), false, info.path);
}

std::vector<std::string> PluginLoader::scan_directory(const std::string& dir) {
    std::vector<std::string> result;
    std::string ext = get_plugin_extension();

    if (!std::filesystem::exists(dir)) {
        std::cerr << "[PluginLoader] Directory not found: " << dir << std::endl;
        return result;
    }

    // Рекурсивное сканирование поддиректорий
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();

            // Проверяем расширение
            if (filename.size() > ext.size() &&
                filename.compare(filename.size() - ext.size(), ext.size(), ext) == 0) {
                result.push_back(entry.path().string());
            }
        }
    }

    std::cout << "[PluginLoader] Found " << result.size()
              << " plugin(s) in " << dir << std::endl;

    return result;
}

int PluginLoader::load_plugins_from_directory(const std::string& dir,
                                               AgentRegistry* registry) {
    auto files = scan_directory(dir);
    int loaded = 0;

    for (const auto& file : files) {
        if (load_plugin(file)) {
            // Извлекаем имя плагина из пути
            std::string name = std::filesystem::path(file).stem().string();
            
            // Регистрируем агента
            if (create_agent_from_plugin(name, registry)) {
                loaded++;
            }
        }
    }

    return loaded;
}

} // namespace agents

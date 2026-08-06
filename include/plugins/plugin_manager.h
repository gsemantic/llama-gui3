#pragma once

#include <memory>
#include <string>
#include <vector>

// ============================================================================
// PluginManager — хост системы плагинов llama-gui.
//
// Загружает разделяемые библиотеки плагинов из директории плагинов,
// проверяет версию API, вызывает ll_plugin_init с таблицей функций хоста
// (см. include/plugins/plugin_api.h), каждый кадр вызывает ll_plugin_render
// и корректно выгружает плагины при завершении приложения.
// ============================================================================

namespace llama_gui {
namespace core {
    class StateManager;
    class Settings;
    class LlamaInterface;
    class RagManager;
}
namespace ui {
    class CommandManager;
    class WindowManager;
    class AdvancedMenuSystem;
    class DialogManager;
    class ChatInterface;
}
}

namespace llama_gui {
namespace plugin {

/**
 * Срезы подсистем приложения, которые открываются плагинам.
 * Заполняется MainWindow перед инициализацией PluginManager.
 */
struct PluginSubsystems {
    llama_gui::ui::CommandManager* command_manager = nullptr;
    llama_gui::ui::WindowManager* window_manager = nullptr;
    llama_gui::ui::AdvancedMenuSystem* menu_system = nullptr;
    llama_gui::ui::DialogManager* dialog_manager = nullptr;
    llama_gui::core::StateManager* state_manager = nullptr;
    llama_gui::core::Settings* settings = nullptr;
    llama_gui::core::LlamaInterface* llama_interface = nullptr;
    llama_gui::ui::ChatInterface* chat_interface = nullptr;
    llama_gui::core::RagManager* rag_manager = nullptr;

    std::string config_dir;   // директория конфигурации приложения
    std::string data_dir;     // директория данных приложения
    std::string plugins_dir;  // директория для поиска плагинов
};

/** Метаданные из манифеста plugin.json (рядом с библиотекой плагина). */
struct PluginManifest {
    bool present = false;       // найден ли и корректно ли распарсен plugin.json
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::string api_version;
    std::vector<std::string> permissions;
    std::vector<std::string> capabilities;
};

/** Публичная информация о загруженном плагине. */
struct PluginInfo {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::string path;
    bool is_loaded = false;

    /** Данные из plugin.json, если манифест присутствует рядом с .so. */
    PluginManifest manifest;
};

/** Внутренняя реализация менеджера (определена в plugin_manager.cpp). */
class PluginImpl;

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /**
     * Инициализация: запоминает подсистемы, сканирует директорию
     * плагинов и загружает все найденные библиотеки.
     */
    bool initialize(const PluginSubsystems& subsystems);

    /**
     * Вызывается каждый кадр после рендера основного UI.
     * Перерегистрирует меню плагинов (устойчиво к перестройке меню
     * при смене языка) и вызывает ll_plugin_render() каждого плагина.
     */
    void render_plugins();

    /** Выгрузка всех плагинов (ll_plugin_shutdown + dlclose). */
    void shutdown();

    bool is_initialized() const;

    std::vector<PluginInfo> list_plugins() const;
    bool is_plugin_loaded(const std::string& name) const;

private:
    bool load_plugin_file(const std::string& path);

    std::unique_ptr<PluginImpl> impl_;
};

} // namespace plugin
} // namespace llama_gui

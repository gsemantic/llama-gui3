#pragma once

#include <memory>
#include <string>
#include <vector>
#include "plugins/plugin_api.h"
#include "core/llama_interface.h"

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
    class WindowCoordinator;
}
}

namespace llama_gui {
namespace plugin {

/**
 * @brief Разбор messages_json (JSON-массив OpenAI-формата) в реплики.
 *
 * Единственная точка разбора для ОБОИХ вызовов хоста (llm_chat_messages и
 * llm_chat_stream): раскладка одна, и две копии разошлись бы при первом же
 * новом поле — то есть один и тот же ход агента уезжал бы в модель с разной
 * историей в зависимости от того, блокирующий путь или стриминговый.
 *
 * Принимает и `"content":"строка"` (исторический формат), и
 * `"content":[{"type":"text",…},{"type":"image_url",…}]` (OpenAI-формат с
 * изображениями). Возвращает false при мусоре, называя причину в error.
 *
 * Объявлена ЗДЕСЬ, а не оставлена внутренней, по одной причине: это
 * граница ABI между хостом и плагином, и именно на ней терялись вложения
 * (Мультимодальность). Непроверяемая граница возвращается к состоянию
 * «картинка не доехала, и никто не знает, где».
 */
bool parse_messages_json(const char* messages_json,
                         std::vector<llama_gui::core::ChatMessage>& out,
                         std::string& error);

/**
 * Срезы подсистем приложения, которые открываются плагинам.
 * Заполняется MainWindow перед инициализацией PluginManager.
 */
struct PluginSubsystems {
    llama_gui::ui::CommandManager* command_manager = nullptr;
    llama_gui::ui::WindowManager* window_manager = nullptr;
    llama_gui::ui::WindowCoordinator* window_coordinator = nullptr;
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

/**
 * @brief Разрешён ли плагинам RAG в текущей конфигурации хоста.
 *
 * Единственное место, где решается «отвечать ли на rag_*». До R0.2 каждый
 * из пяти `host_rag_*` проверял только наличие `rag_manager`, и RAG,
 * выключенный пользователем в настройках, продолжал работать для плагинов:
 * в собственном UI чата он был выключен, а плагин с `rag_access` находил
 * документы как ни в чём не бывало. Два поведения одного переключателя —
 * это два поведения.
 *
 * Правило простое и ровно одно:
 *   - RAG разрешён, если он включён в настройках (`settings->rag().enable_rag`)
 *     И менеджер RAG существует.
 *   - `settings == nullptr` (плагин-тест, усечённый хост) → RAG выключен:
 *     «не знаю, включён ли» безопаснее, чем «разрешаю», потому что второе
 *     — это доступ к данным без решения пользователя.
 *   - `rag_manager == nullptr` → RAG выключен: отвечать нечем.
 *
 * Объявлена ЗДЕСЬ, как и `parse_messages_json`, по той же причине: это
 * граница ABI (доступ плагина к данным пользователя), и непроверяемая
 * граница возвращается к состоянию «выключатель есть, но не работает».
 * Проверяется тестом `test_plugin_rag_gate` без загрузки плагина.
 */
bool plugin_rag_allowed(const PluginSubsystems& subsystems);

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
    /* Имя, каким его объявил сам плагин (`ll_plugin_info()->name`), до
     * санитизации (R0.3). Заполняется, только если имя пришлось менять;
     * пусто — плагин назвался валидно и менять было нечего.
     *
     * Отдельное поле, а не только лог: лог теряется при ротации, а
     * «почему плагин в меню называется не так, как написано у него в
     * исходнике» — вопрос, на который отвечают спустя недели. */
    std::string declared_name;
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

    /* Зарегистрированные режимы агентов (для интеграции с ChatInterface). */
    struct AgentModeInfo {
        const LlamaPluginAgentMode* mode;
        LlamaPluginHost* host;
    };
    std::vector<AgentModeInfo> get_agent_modes() const;

private:
    bool load_plugin_file(const std::string& path);

    std::unique_ptr<PluginImpl> impl_;
};

} // namespace plugin
} // namespace llama_gui

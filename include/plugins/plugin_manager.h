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
    class RagRegistry;
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
    /* R2.2: реестр скоупов RAG (R1.2). Владелец — хост (MainWindow), а не
     * менеджер плагинов: реестр живёт дольше отдельного плагина, иначе
     * выгрузка плагина уносила бы индексы остальных. nullptr = RAG v2
     * недоступен, и все rag_*_ex отвечают {"ok":false,"code":"unavailable"}
     * (легаси-функции это не задевает). */
    std::shared_ptr<llama_gui::core::RagRegistry> rag_registry;

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

/**
 * @brief Настройки RAG плагина из `plugin.<name>.rag.*` (R3.3).
 *
 * Читает host-owned настройки плагина (их пишет UI/хост, плагин — нет,
 * см. namespace-гард R3.2). Значения хранятся в `Settings::custom_settings_`
 * строками, поэтому парсятся здесь.
 *
 * Почему в plugin-слое, а не в `core::RagRegistry`: реестр намеренно не
 * зависит от `Settings` (создаётся из root + эмбеддинг-бэкенда и тестируется
 * без настроек приложения). «Включён ли RAG плагину» — решение хоста, и
 * живёт рядом с `plugin_rag_allowed`.
 *
 * `enabled` по умолчанию true (§3.2): отсутствие настройки — «включено».
 * Настройка читается НА КАЖДОМ вызове, поэтому правка применяется без
 * перезапуска. Ключ конкретного плагина: чужой `enabled=false` не влияет.
 */
struct PluginRagConfig {
    bool enabled = true;
};

PluginRagConfig plugin_rag_config(const core::Settings* settings,
                                  const std::string& plugin_name);

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

/**
 * Таблица ABI, которую хост передаёт плагинам в ll_plugin_init.
 *
 * Единственный источник правды о размере структуры: плагин сверяет
 * `api->size` с `offsetof` нового поля и по этому решает, можно ли его
 * звать. Поэтому таблица обязана быть ОДНОЙ на процесс и доступной тесту
 * ABI: проверка на копии проверяла бы не то, что реально уходит в .so
 * (host_api_table() в plugin_manager.cpp).
 */
const LlamaHostApi& host_api_table();

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

    /* R3.1: есть ли у плагина право из манифеста (`rag_access`,
     * `rag_write`, …). Плагина нет или манифест не объявляет право →
     * false. Единственный источник правды для хоста; сами `host_rag_*`
     * ходят сюда через plugin_has_permission (чистую функцию), чтобы
     * решение можно было проверить без загрузки .so. */
    bool has_permission(const std::string& name, const std::string& perm) const;

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

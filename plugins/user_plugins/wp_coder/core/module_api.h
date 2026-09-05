#pragma once

/*
 * module_api.h — Интерфейс модулей AI-кодера.
 *
 * Каждый модуль (WordPress, Python, DevOps и т.д.) реализует этот интерфейс
 * и регистрирует свои инструменты, навыки и промпты через callbacks.
 *
 * Модуль — это набор:
 *   - инструментов (tools), которые агент может вызывать
 *   - навыков (skills), инжектируемых в системный промпт
 *   - системного промпта (system prompt), описывающего домен
 *   - UI-панели (опционально), отображаемой в окне настроек
 */

#include <string>
#include <vector>
#include <functional>
#include <map>

namespace coder {

/* --- Тип инструмента --- */

/* Параметры, передаваемые агентом в инструмент. */
struct ToolArgs {
    std::string path;      // PATH
    std::string root;      // ROOT
    std::string query;     // QUERY
    std::string pattern;   // PATTERN
    std::string content;   // CONTENT (between CONTENT_BEGIN/END)
    std::string cli;       // CLI
    std::string url;       // URL
    int k = 6;             // K
};

/* Обработчик инструмента: принимает ToolArgs, возвращает текст-результат. */
using ToolHandler = std::function<std::string(const ToolArgs&)>;

/* Описание зарегистрированного инструмента. */
struct ToolInfo {
    std::string name;
    std::string description;  // для документации/подсказок
    ToolHandler handler;
};

/* --- Навык (skill) --- */

struct Skill {
    std::string name;
    std::string description;
    std::string body;         // текст инструкции, инжектируемый в промпт
};

/* --- Интерфейс модуля --- */

/*
 * Модуль — это динамически подключаемая логика (не .so, а линкуемая статически).
 * Каждый модуль реализует CoderModule и регистрируется через ModuleRegistry.
 *
 * Жизненный цикл:
 *   1. ModuleRegistry::register_module() — при инициализации плагина
 *   2. module->init() — один раз
 *   3. module->get_tools() / get_skills() / get_system_prompt() — при сборке промпта
 *   4. module->shutdown() — при выгрузке
 */
struct CoderModule {
    const char* name;
    const char* display_name;
    const char* description;

    /* Инициализация модуля (вызывается один раз). */
    void (*init)();

    /* Возвращает список инструментов, доступных в этом модуле. */
    std::vector<ToolInfo> (*get_tools)();

    /* Возвращает навыки модуля (инструкции для промпта). */
    std::vector<Skill> (*get_skills)();

    /* Возвращает доменный системный промпт (добавляется к базовому). */
    const char* (*get_system_prompt)();

    /* Рендер UI-панели модуля (вызывается каждый кадр в ImGui). Опционально. */
    void (*render_panel)();

    /* Настройки модуля (сериализация/десериализация). */
    void (*load_settings)(const std::string& data_dir);
    void (*save_settings)();

    /* Очистка ресурсов. */
    void (*shutdown)();
};

/* --- Реестр модулей --- */

/*
 * Глобальный реестр модулей. Заполняется при инициализации плагина.
 * Движок (engine) собирает инструменты/навыки/промпты из всех модулей.
 */
class ModuleRegistry {
public:
    static ModuleRegistry& instance();

    void register_module(const CoderModule* module);
    const std::vector<const CoderModule*>& modules() const;

    /* Найти модуль по имени. */
    const CoderModule* find(const std::string& name) const;

    /* Собрать все инструменты из всех модулей. */
    std::vector<ToolInfo> all_tools() const;

    /* Собрать все навыки из всех модулей. */
    std::vector<Skill> all_skills() const;

    /* Собрать промпты всех модулей. */
    std::string combined_system_prompt() const;

    /* Инициализировать все модули. */
    void init_all();

    /* Shutdown все модули. */
    void shutdown_all();

private:
    std::vector<const CoderModule*> modules_;
};

} // namespace coder

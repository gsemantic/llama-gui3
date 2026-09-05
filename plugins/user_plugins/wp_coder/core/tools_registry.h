#pragma once

/*
 * tools_registry.h — Реестр инструментов AI-кодера.
 *
 * Централизованное хранилище обработчиков инструментов.
 * Модули регистрируют свои инструменты, движок вызывает через tool_run().
 *
 * Потокобезопасность: регистрация — только при инициализации (single-threaded).
 * Вызов tool_run() — из worker-потока (read-only access к registry).
 */

#include "module_api.h"
#include <string>
#include <map>

namespace coder {

class ToolsRegistry {
public:
    static ToolsRegistry& instance();

    /* Регистрация инструмента (вызывается модулем при init). */
    void register_tool(const std::string& name, ToolHandler handler,
                       const std::string& description = "");

    /* Регистрация всех инструментов из модуля. */
    void register_tools(const std::vector<ToolInfo>& tools);

    /* Вызов инструмента по имени. Возвращает результат или сообщение об ошибке. */
    std::string run(const std::string& tool_name, const ToolArgs& args);

    /* Проверка: инструмент зарегистрирован? */
    bool has(const std::string& tool_name) const;

    /* Список всех зарегистрированных инструментов (для документации/UI). */
    std::vector<std::string> list_tools() const;

    /* Очистка (для shutdown). */
    void clear();

private:
    std::map<std::string, ToolInfo> tools_;
};

} // namespace coder

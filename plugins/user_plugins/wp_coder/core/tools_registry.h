#pragma once

/*
 * tools_registry.h — Реестр инструментов AI-кодера.
 *
 * И1.5: реестр хранит ToolDef (core/tool.h), то есть описание, JSON-схему
 * параметров и флаги, а не пару «имя + обработчик». Из схемы строится
 * каталог инструментов для системного промпта — ручной список в
 * core/prompts.h:45-67 больше не источник истины и удалён.
 *
 * Следствие, ради которого всё и делалось: добавление параметра к
 * инструменту = правка одного файла (объявление схемы рядом с
 * обработчиком). Больше не нужно править ToolArgs, разбор протокола,
 * строку в промпте и проверки режима.
 *
 * Потокобезопасность: регистрация — только при инициализации
 * (single-threaded). Вызов run() — из worker-потока (read-only).
 */

#include "json.h"
#include "module_api.h"   /* ToolArgs — вход для run() со старыми слотами */
#include "tool.h"

#include <map>
#include <string>
#include <vector>

namespace coder {

class ToolsRegistry {
public:
    static ToolsRegistry& instance();

    /* --- Регистрация --- */

    /* Единственный способ зарегистрировать инструмент (И1.3).
     * Старый register_tool(name, ToolHandler, description) жил только
     * на время миграции И1.3–И1.7 и удалён вместе с ней: он не мог
     * ни описать схему, ни классифицировать инструмент, поэтому
     * невалидировал аргументы и был невидим для политики режимов. */
    void register_def(ToolDef def);

    /* --- Вызов --- */

    /* Полный результат: текст для модели плюс title/metadata/truncated
     * для UI (И11) и будущей событийной модели (И5). */
    ToolOutput run_output(const std::string& tool_name, const json::JsonValue& args);

    /* Вызов с типизированными аргументами: валидация по схеме (И1.6)
     * и затем обработчик. Возвращает сообщение об ошибке, если
     * инструмента нет или аргументы не прошли схему. */
    std::string run(const std::string& tool_name, const json::JsonValue& args);

    /* Удобство для вызывающего кода и тестов: старые 8 слотов
     * ToolArgs → JsonValue → тот же путь валидации и вызова. */
    std::string run(const std::string& tool_name, const ToolArgs& args);

    /* --- Доступ к описаниям --- */

    bool has(const std::string& tool_name) const;
    const ToolDef* find(const std::string& tool_name) const;
    std::vector<std::string> list_tools() const;
    std::vector<ToolDef> defs() const;

    /* Каталог инструментов для системного промпта, построенный из схем
     * (И1.5). Возвращает одну строку на инструмент: имя, назначение и
     * параметры. Неизвестные одному источнику истины здесь быть не
     * может: описание и параметры берутся из той же схемы, по которой
     * идёт валидация. */
    std::string build_tool_catalogue() const;

    /* Описание одного инструмента — для точечного запроса и тестов. */
    std::string describe_tool(const std::string& tool_name) const;

    /* Список инструментов одной строкой через запятую (для сообщений
     * об ошибках). */
    std::string join_tools() const;

    void clear();

private:
    std::map<std::string, ToolDef> tools_;
};

} // namespace coder

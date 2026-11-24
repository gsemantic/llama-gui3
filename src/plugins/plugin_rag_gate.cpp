/*
 * plugin_rag_gate.cpp — доступ плагинов к RAG (R0.2).
 *
 * Одна функция и одно правило: отвечать ли плагину на rag_*.
 *
 * ПОЧЕМУ ОТДЕЛЬНЫЙ ФАЙЛ, А НЕ plugin_manager.cpp. Функция объявлена в
 * plugin_manager.h и обязана иметь внешнюю линковку, иначе её не видит
 * тест. В plugin_manager.cpp все host_* лежат внутри анонимного
 * namespace, и определение там получило бы внутреннюю линковку:
 * `nm` показал бы `(anonymous namespace)::plugin_rag_allowed`, а линковка
 * теста — undefined reference. Та же причина, по которой разбор символов
 * живёт в ast_symbols.cpp, а не в plugin_manager.cpp: тестируемая логика
 * не должна зависеть от того, куда её положили в большом файле.
 *
 * ЧТО ИМЕННО ПРОВЕРЯЕТСЯ (обоснование — в plugin_manager.h):
 *   - менеджера RAG нет          → RAG выключен (отвечать нечем);
 *   - настроек нет               → RAG выключен («не знаю» = «нет», потому
 *     что «разрешаю» дало бы доступ к данным без решения пользователя);
 *   - settings->rag().enable_rag → единственный выключатель пользователя.
 *
 * До R0.2 каждый из пяти host_rag_* проверял только наличие rag_manager.
 * Пользователь выключал RAG в настройках — в собственном чате он
 * замолкал, а плагин с правом rag_access продолжал искать по документам.
 * Один переключатель с двумя поведениями: это два переключателя.
 */

#include "plugins/plugin_manager.h"

// Settings нужен полным типом: plugin_rag_allowed читает
// settings->rag().enable_rag, а в plugin_manager.h он только
// forward-объявлен. Без этого — «invalid use of incomplete type».
#include "core/settings.h"

namespace llama_gui {
namespace plugin {

bool plugin_rag_allowed(const PluginSubsystems& subsystems) {
    if (!subsystems.rag_manager) return false;
    if (!subsystems.settings) return false;
    return subsystems.settings->rag().enable_rag;
}

} // namespace plugin
} // namespace llama_gui
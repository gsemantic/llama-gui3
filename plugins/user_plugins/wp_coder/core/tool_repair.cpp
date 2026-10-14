/*
 * tool_repair.cpp — тело починки вызова инструмента (И12.3).
 * Обоснование — в tool_repair.h; здесь только код.
 */

#include "tool_repair.h"
#include "tools_registry.h"

namespace coder {
namespace {

/* Приведение к нижнему регистру — ТОЛЬКО ASCII, и намеренно своими
 * руками, а не std::tolower:
 *
 *   * std::tolower(char) требует значения, представимого как unsigned
 *     char, а на не-ASCII байтах (кириллица в имени инструмента, если
 *     модель его выдумала) это UB — то есть починка имени гоняла бы
 *     неопределённое поведение по кругу каждый вызов;
 *   * ::tolower с локалью меняет поведение от LC_ALL окружения, а
 *     результат починки должен быть ОДИН И ТОТ ЖЕ на любой машине.
 *
 * Имена инструментов в реестре — ASCII в нижнем регистре, поэтому
 * не-ASCII байты просто остаются как есть и не находят совпадения. */
std::string ascii_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/* Текст для модели. Список доступных берётся из реестра, а не из
 * захардкоженного списка: иначе починка предлагала бы вызвать то, чего
 * в этой сборке нет, — то же расхождение каталога и реальности, только
 * в сообщении об ошибке. */
std::string unknown_tool_message(const std::string& requested) {
    return "Инструмент «" + requested + "» не существует. "
           "Доступные инструменты: " + ToolsRegistry::instance().join_tools();
}

} // namespace

ToolRepair repair_tool_call(const std::string& tool_name) {
    ToolRepair repair;
    repair.requested = tool_name;

    ToolsRegistry& reg = ToolsRegistry::instance();

    /* Шаг 0: точное совпадение. Приведение регистра идёт только после
     * него — иначе «Read» переименовывался бы в «read» даже там, где
     * инструмент с именем «Read» есть, и тихо вызывался бы не тот. */
    if (const ToolDef* exact = reg.find(tool_name)) {
        repair.tool = tool_name;
        repair.def = exact;
        return repair;
    }

    /* Шаг 1: имя в нижнем регистре, если такое есть. */
    const std::string lowered = ascii_lower(tool_name);
    if (const ToolDef* lower = reg.find(lowered)) {
        repair.tool = lowered;
        repair.def = lower;
        /* repaired_case здесь безусловно true, и проверять «lowered !=
         * tool_name» было бы условием, которое не может быть ложным:
         * до этого шага мы уже вернулись, если точное имя нашлось, то
         * есть lowered == tool_name означал бы, что не нашлось и оно
         * тоже. Условие, всегда истинное, читается как проверка чего-то
         * и со временем станет проверкой не того. */
        repair.repaired_case = true;
        return repair;
    }

    /* Шаг 2: скрытый `invalid`. */
    repair.tool = kInvalidTool;
    repair.def = nullptr;
    repair.routed_to_invalid = true;
    repair.invalid_args.set("tool", json::JsonValue(tool_name));
    repair.invalid_args.set("error",
                             json::JsonValue(unknown_tool_message(tool_name)));
    return repair;
}

ToolOutput invalid_tool_output(const ToolRepair& repair) {
    ToolOutput out;
    out.title = "неизвестный инструмент";
    out.output = unknown_tool_message(repair.requested);
    return out;
}

} // namespace coder
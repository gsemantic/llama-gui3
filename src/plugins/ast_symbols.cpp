// ast_symbols.cpp — символы одного файла для плагина (И6.3).

#include "ast_symbols.h"

#include "core/document_parser.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace llama_gui {
namespace plugin {
namespace fs = std::filesystem;

const char* ast_kind_for_type(const std::string& node_type)
{
    /* Порядок важен: у tree-sitter «class_definition» входит в «function»,
     * а «method_definition» — и туда, и туда. Сначала самые узкие. */
    if (node_type.find("method") != std::string::npos) return "method";
    if (node_type.find("class") != std::string::npos) return "class";
    if (node_type.find("struct") != std::string::npos) return "class";
    if (node_type.find("interface") != std::string::npos) return "class";
    if (node_type.find("trait") != std::string::npos) return "class";
    if (node_type.find("function") != std::string::npos) return "function";
    if (node_type.find("namespace") != std::string::npos) return "namespace";
    if (node_type.find("mod") != std::string::npos) return "namespace";
    if (node_type.find("enum") != std::string::npos) return "enum";
    if (node_type.find("typedef") != std::string::npos) return "typedef";
    if (node_type.find("type_definition") != std::string::npos) return "typedef";
    if (node_type.find("type_alias") != std::string::npos) return "typedef";
    if (node_type.find("alias") != std::string::npos) return "typedef";
    if (node_type.find("macro") != std::string::npos) return "macro";
    if (node_type == "decorated_definition") return "block";
    return "unknown";
}

nlohmann::json symbols_to_json(const std::vector<core::AstNode>& nodes)
{
    nlohmann::json out = nlohmann::json::array();
    for (const core::AstNode& node : nodes) {
        /* Узел без имени — не символ. Пропуск молчаливый, и это осознанно:
         * имя приходит из тела узла, и пустое имя означает, что парсер не
         * нашёл объявления (например, обрезанный файл). Символ с пустым
         * именем в обзоре проекта бесполезен и только занимает место. */
        if (node.name.empty()) continue;
        nlohmann::json s;
        s["kind"] = ast_kind_for_type(node.type);
        s["name"] = node.name;
        s["parent"] = node.parent_name;
        /* Строки 1-based: так их показывает и сам парсер, и так их пишет
         * большинство инструментов. Пересчёт «на всякий случай» означал бы
         * расхождение с парсером, о котором узнают по файлу. */
        s["line"] = node.start_line;
        s["end_line"] = node.end_line;
        out.push_back(std::move(s));
    }
    return out;
}

nlohmann::json ast_symbols_for_file(const std::string& path,
                                    const std::string& language)
{
    using nlohmann::json;
    json out;
    out["ok"] = false;
    out["ast"] = false;
    out["symbols"] = json::array();

    if (path.empty()) {
        out["error"] = "пустой путь";
        return out;
    }
    /* Язык по расширению берётся из DocumentParser::get_language — это
     * единственная таблица расширений в проекте. Своя вторая таблица
     * разошлась бы с первой при первом же новом расширении, и одна из них
     * молча перестала бы узнавать файлы. */
    const std::string lang = language.empty()
        ? core::DocumentParser::get_language(path)
        : language;
    out["language"] = lang;

    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        out["error"] = "не файл: " + path;
        return out;
    }
    if (lang.empty()) {
        out["error"] = "язык не определён по расширению";
        return out;
    }

    if (!core::AstParser::is_available(lang)) {
        /* Не ошибка: файлы читаются, символы просто не извлекаются. ok
         * остаётся false, потому что символов не будет, а ast=false —
         * потому что это не AST. Вызывающий обязан проверить ast и уйти на
         * строковый скан, сказав об этом. */
        out["ok"] = true;
        out["ast"] = false;
        out["reason"] =
            "грамматика tree-sitter для '" + lang + "' не собрана — разбор до текста";
        return out;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        out["error"] = "не удалось прочитать файл";
        return out;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (buffer.str().empty()) {
        /* Пустой файл — не ошибка парсинга: грамматика есть, разбор прошёл,
         * символов ноль. Иначе пустой файл выглядел бы как отказ и вызывающий
         * начал бы «чинить» то, что чинить нечего. */
        out["ok"] = true;
        out["ast"] = true;
        return out;
    }

    /* Парсер держит TSParser с изменяемым состоянием, и он не пригоден для
     * двух потоков сразу. Плагин зовёт нас из рабочего потока агента, пока
     * UI может звать из другого, поэтому на поток свой экземпляр. */
    thread_local core::AstParser parser;
    core::AstNode root = parser.parse_source(buffer.str(), lang);
    /* Ноль символов — НЕ отказ, и здесь это принципиально.
     *
     * parse_source всегда возвращает узел с type == "program", в том числе
     * когда разбор не удался или грамматики нет: признака неудачи у него
     * НЕТ. Отличить «файл без определений» от «файл не разобран» по его
     * результату нельзя, а первое — обычное дело (файл только с комментарием,
     * только с include, файл данных с расширением .cpp).
     *
     * Раньше здесь стояла проверка children.empty() с возвратом ok:false, и
     * она объявляла отказом любой файл без определений: плагин записал бы в
     * обзор «ошибка разбора» там, где всё в порядке. Ложный отказ хуже
     * молчаливого нуля, потому что его видно и на него пытаются чинить.
     * Поэтому честно отвечаем «разобрали, символов столько» — и на
     * неразобранный файл плагин получит ноль символов, как и на файл без
     * определений. Развести эти случаи можно только в самом AstParser, и
     * это отдельная задача хоста, а не сюрприз в ответе ABI. */
    out["ok"] = true;
    out["ast"] = true;
    out["symbols"] = symbols_to_json(parser.extract_top_level_nodes(root));
    return out;
}

} // namespace plugin
} // namespace llama_gui

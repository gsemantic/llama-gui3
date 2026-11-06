// ast_symbols.cpp — символы файла: AST хоста или строковый скан (И13.5).
//
// Правило и его границы — в ast_symbols.h. Здесь только разбор ответа
// хоста, строковый скан (перенесён из base_tools.cpp без изменения
// правил) и сборка результата.

#include "ast_symbols.h"

#include "json.h"

#include <cctype>
#include <fstream>
#include <utility>

namespace coder {
namespace ast_symbols {
namespace {

/*
 * Вид символа хоста → метка обзора. Таблица закрытая и равна словарю
 * ast_kind_for_type на стороне хоста (И6.3): function, class, method,
 * namespace, enum, typedef, macro, block, unknown.
 *
 * method и block отмечены той же буквой f, что и function, не по недосмотру:
 * и то и другое — имя, по которому вызывают, а не объявление типа. На
 * живом материале они в ответе не встречаются (хост отдаёт верхний
 * уровень), но вид в словаре есть, и не назначить ему метку значило бы
 * молча ронять символы, если хост начнёт отдавать вложенные.
 *
 * unknown метки не имеет: это «вид не опознан», и печатать его с
 * придуманной буквой значило бы отдать модели символ, смысл которого
 * никто не проверял. Если после фильтра ничего не осталось — список пуст,
 * а пустой список уводит на скан (см. take_ast).
 */
const char* tag_for_kind(const std::string& kind) {
    if (kind == "function" || kind == "method" || kind == "block") return "f";
    if (kind == "class") return "c";
    if (kind == "namespace") return "n";
    if (kind == "enum") return "e";
    if (kind == "typedef") return "t";
    if (kind == "macro") return "m";
    return nullptr;
}

/* Слово, начинающееся с pos: буквы, цифры и подчёркивание. */
std::string word_from(const std::string& line, size_t pos) {
    size_t end = pos;
    while (end < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_')) {
        ++end;
    }
    return line.substr(pos, end - pos);
}

/*
 * Один проход по файлу: определения строковым сканом И хуки.
 *
 * Правила определений перенесены из base_tools.cpp как есть, включая
 * else-if (строка даёт не больше одного определения) и требование непустого
 * слова после «function »/«class ». Менять их здесь нельзя: этот скан —
 * запасной путь, и «стало аккуратнее» означало бы, что сравнивать
 * деградацию с прежним поведением нечем.
 *
 * Правила хуков тоже прежние: первое сработавшее из add_action /
 * add_filter / add_shortcode на строке, имя — между кавычками после
 * открывающей скобки. Хук, у которого на строке больше одного вызова,
 * даёт первый: обзор показывает, ЧТО зарегистрировано, а сколько раз —
 * видно в том же файле через grep_search.
 */
void scan_file(const std::string& path, std::vector<Symbol>& defs,
               std::vector<Symbol>& hooks) {
    static const char* kHooks[] = {"add_action", "add_filter", "add_shortcode"};
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t kw = line.find("function ");
        if (kw != std::string::npos) {
            Symbol s;
            s.tag = "f";
            s.name = word_from(line, kw + 9);
            if (!s.name.empty()) defs.push_back(std::move(s));
        } else if ((kw = line.find("class ")) != std::string::npos) {
            Symbol s;
            s.tag = "c";
            s.name = word_from(line, kw + 6);
            if (!s.name.empty()) defs.push_back(std::move(s));
        }
        for (const char* hook : kHooks) {
            const size_t hk = line.find(hook);
            if (hk == std::string::npos) continue;
            const size_t lp = line.find('(', hk);
            if (lp == std::string::npos) continue;
            size_t q1 = line.find('\'', lp + 1);
            if (q1 == std::string::npos) q1 = line.find('"', lp + 1);
            if (q1 == std::string::npos) continue;
            const char q = line[q1];
            const size_t q2 = line.find(q, q1 + 1);
            if (q2 == std::string::npos) continue;
            Symbol s;
            s.tag = "h";
            s.name = line.substr(q1 + 1, q2 - q1 - 1);
            hooks.push_back(std::move(s));
            break;
        }
    }
}

/* Ответ хоста не годится: причина в словах плагина, не цитатой хоста. */
std::string degrade_reason(const std::string& raw) {
    json::JsonValue v;
    if (raw.empty()) return "хост не ответил";
    if (!json::JsonValue::parse(raw, v) || !v.is_object())
        return "ответ хоста не разобран";
    if (!v.get_bool("ok", false))
        return "хост отказал: " + v.get_string("error", "без причины");
    if (!v.get_bool("ast", false))
        return "хост без грамматики: " + v.get_string("reason", "без причины");
    const json::JsonValue* arr = v.find("symbols");
    if (!arr || !arr->is_array())
        return "хост без поля symbols";
    /* ast:true и ноль символов. Для вызывающего это неотличимо от «файл без
     * определений», поэтому причина названа прямо: доверять такому ответу
     * нельзя, и проверка строкой — не перестраховка, а единственный способ
     * отличить пустой файл от неразобранного. */
    return "хост вернул ноль символов вместо разбора файла";
}

/* Символы из разобранного ответа хоста. Пустой результат — не успех.
 *
 * Собираются в свой список и кладутся в out только разом: out.symbols к
 * этому моменту уже содержит результат скана, и частичная запись поверх
 * него смешала бы два источника в одном списке — ровно то, из-за чего
 * список не печатается двумя колонками.
 *
 * ГРАНИЦА, из-за которой этот список пришлось переписать: пустой ответ хоста
 * НЕ отличается от «файл без определений», и проверять его надо ЗДЕСЬ, на
 * границе «список пригоден или нет», а не только на входе по размеру
 * массива. Списка-«нет» и списка-«вычеркнут целиком» касается одна и та же
 * проверка в конце, и второй на месте неё стоял бы guards, который не
 * решает ничего: мутация «убрать size()==0» была эквивалентна (журнал §4,
 * И13.5). */
bool take_ast(const std::string& raw, FileSymbols& out) {
    json::JsonValue v;
    if (!json::JsonValue::parse(raw, v) || !v.is_object()) return false;
    if (!v.get_bool("ok", false)) return false;
    /* ast:false проверяется ДО списка и независимо от него: хост, который
     * отвечает «грамматики нет», может приложить к такому ответу любой
     * список, и поверить символам значило бы поверить разбору, которого не
     * было. Поле ast обязано быть проверено — это написано и в ABI. */
    if (!v.get_bool("ast", false)) return false;
    const json::JsonValue* arr = v.find("symbols");
    /* is_array — проверка ТИПА, а не пустоты: JsonValue::at() для не-массива
     * отдаёт null, size() — ноль, и проверка в конце поймала бы такой ответ
     * сама. Но это свойство JsonValue, а не обещание ответа: «symbols —
     * список» написано в ABI, и здесь оно проверяется на границе, где
     * символы берутся из этого поля. Убрать её «как дубликат» — значит
     * сделать вид, будто формат ответа не проверяется вовсе. */
    if (!arr || !arr->is_array()) return false;
    std::vector<Symbol> syms;
    for (size_t i = 0; i < arr->size(); ++i) {
        const json::JsonValue& s = arr->at(i);
        if (!s.is_object()) continue;
        const std::string name = s.get_string("name");
        if (name.empty()) continue;
        const char* tag = tag_for_kind(s.get_string("kind"));
        if (!tag) continue;
        Symbol sym;
        sym.tag = tag;
        sym.name = name;
        syms.push_back(std::move(sym));
    }
    /* Один отказ на оба случая: хост вернул пустой список (разбор не
     * удался, а признака неудачи у него нет) и хост вернул список, из
     * которого не уцелел ни один символ. Для вызывающего они неразличимы и
     * оба означают «разбора нет». */
    if (syms.empty()) return false;
    out.symbols = std::move(syms);
    out.from_ast = true;
    out.degrade.clear();
    return true;
}

} // namespace

std::string symbol_text(const Symbol& s) { return s.tag + ":" + s.name; }

FileSymbols file_symbols(const HostCallbacks& cb, const std::string& path) {
    FileSymbols out;

    /* Один проход по файлу в любом случае: хуки нужны всегда, а при отказе
     * хоста к ним добавляются ещё и определения. */
    scan_file(path, out.symbols, out.hooks);

    if (!cb.ast_symbols) {
        out.degrade = "хоста без AST-разбора";
        return out;
    }
    /* Язык — пустым: ход по расширению делает хост, и у него одна таблица
     * расширений на весь проект. */
    const std::string raw = cb.ast_symbols(path, std::string());
    if (take_ast(raw, out)) return out;

    out.degrade = degrade_reason(raw);
    return out;
}

} // namespace ast_symbols
} // namespace coder

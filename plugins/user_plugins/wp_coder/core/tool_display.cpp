// tool_display.cpp — И11.4: виджет вызова по типу инструмента.

#include "tool_display.h"

#include "engine.h"
#include "json_utils.h"

#include <algorithm>

namespace coder {
namespace tool_display {
namespace {

/* ---------------------------------------------------------------------------
 * ПЕРЕЧЕНЬ: ИМЯ ИНСТРУМЕНТА → ВИД
 * ---------------------------------------------------------------------------
 *
 * Ключ вида — ИМЯ, а не флаги инструмента. Флаги отвечают на вопрос «что
 * инструмент УМЕЕТ» (правило 7 SESSION_START), а вид отвечает на вопрос
 * «что у него в ответе»: `grep_search` и `glob` оба TF_READ_ONLY, но у
 * первого в метаданных нет НИЧЕГО, а у второго есть счётчики, и виджет их
 * показывает по-разному. По флагам пришлось бы звать реестр на каждом
 * кадре окна.
 *
 * Таблица ПОЛНАЯ по живому реестру, и полноту проверяет тест: новый
 * инструмент без вида уронит сверку с его именем. Инструменты, оставленные
 * откату `Text`, перечислены здесь же — молчаливый откат означал бы, что
 * человек не может отличить «я не описал этот инструмент» от «инструменту
 * нечего показать».
 */
struct Entry {
    const char* name;
    Kind kind;
};

const Entry kEntries[] = {
    /* --- Свой вид (11 инструментов) --- */
    {"bash",          Kind::Command},
    {"read_file",     Kind::Preview},
    {"write_file",    Kind::Diff},
    {"search_replace", Kind::Diff},
    {"edit_file",     Kind::Diff},
    {"apply_patch",   Kind::Diff},
    {"grep_search",   Kind::List},
    {"glob",          Kind::List},
    {"list",          Kind::List},
    {"task",          Kind::Task},
    {"todowrite",     Kind::Checklist},
    /* --- Откат «текст вывода» (46 инструментов) ---
     *
     * Ни у одного из них в метаданных НИЧЕГО нет: `core/git_tools.cpp`,
     * `modules/python`, `modules/devops` и `modules/wordpress` не пишут ни
     * одного поля (проверено чтением), а `core/tools_registry.cpp` добавляет
     * `truncated`/`output_path` только при общем усечении вывода. Поэтому
     * «код возврата» для них недоступен в принципе, и виджет показывает
     * заголовок, который поставил инструмент, и его текст вывода.
     *
     * `todoread` — здесь, а не в `Checklist`, и это решение, а не забывка:
     * у него нет ни аргументов, ни метаданных с пунктами — план лежит в
     * состоянии сессии, а не в вызове. Чтобы показать его чек-листом,
     * пришлось бы разбирать ОТРЕНДЕРЕННЫЙ текст `render_todos`, то есть
     * завести второй разбор одного формата (Д2, Д12); а показывать под этим
     * вызовом ТЕКУЩИЙ план означало бы подписать его «план на этом шаге»,
     * хотя это план на сейчас. */
    {"repo_map",        Kind::Text},
    {"todoread",        Kind::Text},
    {"list_skills",     Kind::Text},
    {"skill_detail",    Kind::Text},
    {"web_fetch",       Kind::Text},
    /* `revert`, `undo` и `redo` меняют файлы, но diff НЕ НЕСУТ: патч к ним
     * не строится, и показывать им виджет diff было бы значило рисовать
     * пустую правку. Слово «изменилось» у них есть в заголовке вывода. */
    {"revert",          Kind::Text},
    {"undo",            Kind::Text},
    {"redo",            Kind::Text},
    {"rag_index",       Kind::Text},
    {"rag_query",       Kind::Text},
    /* git */
    {"git_status",      Kind::Text},
    {"git_diff",        Kind::Text},
    {"git_log",         Kind::Text},
    {"git_commit",      Kind::Text},
    {"git_add",         Kind::Text},
    {"git_branch",      Kind::Text},
    {"git_checkout",    Kind::Text},
    /* python */
    {"python_run",      Kind::Text},
    {"pip_install",     Kind::Text},
    {"django_manage",   Kind::Text},
    {"pytest_run",      Kind::Text},
    {"venv_create",     Kind::Text},
    {"python_lint",     Kind::Text},
    /* devops */
    {"docker_build",    Kind::Text},
    {"docker_run",      Kind::Text},
    {"docker_ps",       Kind::Text},
    {"docker_logs",     Kind::Text},
    {"systemd_status",  Kind::Text},
    {"systemd_restart", Kind::Text},
    {"nginx_test",      Kind::Text},
    {"nginx_reload",    Kind::Text},
    {"cron_list",       Kind::Text},
    {"cron_add",        Kind::Text},
    {"ssh_exec",        Kind::Text},
    /* wordpress и прочее */
    {"wp_cli",          Kind::Text},
    {"wp_db",           Kind::Text},
    {"wp_media",        Kind::Text},
    {"wp_option",       Kind::Text},
    {"wp_rest",         Kind::Text},
    {"wp_check_deps",   Kind::Text},
    {"wp_create_site",  Kind::Text},
    {"deploy",          Kind::Text},
    {"verify",          Kind::Text},
    {"php_lint",        Kind::Text},
    {"headless_render", Kind::Text},
    {"validate",        Kind::Text},
};

/* Строки вывода, по пределу. Пропущенное СЧИТАЕТСЯ, а не выводится
 * предположением: тело, которое молча закончилось на сорока строке, читалось
 * бы как «вывод был такой».
 *
 * Хвост ПОСЛЕ последнего перевода строки — не строка: он возникает из того,
 * что текст на нём кончился, и пустая строка в конце виджета была бы мусором,
 * а в счётчике пропущенных — лишней единицей. Настоящие пустые строки внутри
 * текста при этом сохраняются. */
void append_lines(std::vector<std::string>* out, size_t* omitted,
                  const std::string& text) {
    size_t start = 0;
    while (true) {
        const size_t nl = text.find('\n', start);
        const bool tail_after_newline =
            nl == std::string::npos && start == text.size() && start > 0;
        if (!tail_after_newline) {
            const std::string line = text.substr(
                start, nl == std::string::npos ? std::string::npos : nl - start);
            if (out->size() < limits::kMaxToolDisplayLines) {
                out->push_back(line);
            } else {
                ++(*omitted);
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

/* Обрезка одного значения для подписи (команда, название задачи). По
 * СИМВОЛАМ, с признаком из самого обреза — то же основание, что у якоря
 * реплики в дереве (отклонение 155). */
std::string clip(const std::string& s, size_t max_chars, bool* clipped) {
    return text::utf8_prefix_chars(s, max_chars, clipped);
}

void add_note(std::string* note, const std::string& add) {
    if (add.empty()) return;
    if (!note->empty()) *note += "; ";
    *note += add;
}

/* Общая оговорка про обрез снимка — одна на все виды, иначе её пришлось бы
 * повторять в шести местах, а забыли бы в одном. */
void note_clipping(Widget& w, const CallView& view) {
    if (!view.clipped) return;
    add_note(&w.note,
             "вывод показан частично: " + std::to_string(view.clipped_chars) +
                 " символов из " + std::to_string(view.output_chars));
}

/* Строки вместо вывода, когда вывода нет. Четыре разных случая — четыре
 * разных ответа: молчаливая пустая строка читалась бы как «инструмент
 * промолчал», а это не то же самое, что «ещё работает» или «отказали». */
bool append_missing_output(Widget& w, const CallView& view) {
    if (view.has_output) return false;
    if (view.status == ToolState::Pending || view.status == ToolState::Running) {
        w.lines.push_back("вывод ещё не пришёл");
    } else if (!view.error.empty()) {
        w.lines.push_back("отказ: " + view.error);
    } else if (view.output_cleared) {
        w.lines.push_back("вывод очищен прореживанием (И7.9)");
    } else {
        w.lines.push_back("инструмент не вернул вывода");
    }
    return true;
}

/* --- тело по видам --- */

Widget build_command(const CallView& view) {
    Widget w;
    w.kind = Kind::Command;
    const json::JsonValue& md = view.metadata;
    const bool has_exit = md.is_object() && md.has("exit_code");
    w.head = "bash — exit " +
             (has_exit ? std::to_string(md.get_int("exit_code", 0))
                       : std::string("неизвестен"));
    if (md.get_bool("timed_out", false)) w.head += ", прервано по таймауту";
    if (view.truncated || md.get_bool("truncated", false)) {
        w.head += ", вывод усечён";
    }
    /* Признак отмены человеком (`cancelled`) лежит только в ТЕКСТЕ ответа
     * bash, в метаданные инструмент его не кладёт. Ключа нет — значит и
     * показывать нечего, а выдумывать поле было бы враньём. */
    const std::string full_out = md.get_string("output_path", "");
    if (!full_out.empty()) w.head += ", полный вывод: " + full_out;

    const std::string cli = view.args.get_string("cli", "");
    if (cli.empty()) {
        add_note(&w.note, "команда не пришла в аргументах");
    } else {
        bool cut = false;
        const std::string short_cli =
            clip(cli, limits::kMaxToolDisplayTaskChars, &cut);
        if (cut) add_note(&w.note, "команда обрезана для показа");
        /* Каждая строка команды со знаком `$`: так показывают команды в
         * терминале, и многнострочная команда не склеилась бы в одну. */
        size_t start = 0;
        while (true) {
            const size_t nl = short_cli.find('\n', start);
            w.lines.push_back("$ " + short_cli.substr(
                                          start, nl == std::string::npos
                                                     ? std::string::npos
                                                     : nl - start));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    if (!append_missing_output(w, view)) {
        append_lines(&w.lines, &w.omitted_lines, view.output);
    }
    return w;
}

Widget build_preview(const CallView& view) {
    Widget w;
    w.kind = Kind::Preview;
    const json::JsonValue& md = view.metadata;
    /* Путь берётся из метаданных, а не из аргументов: инструмент кладёт
     * ТОТ ЖЕ путь, что и вернул человеку в заголовке, и расхождение двух
     * источников читалось бы как «прочитал не то». */
    const std::string path = md.get_string("path", "");
    w.head = view.tool + " — ";
    w.head += path.empty() ? std::string("путь не пришёл")
                            : path;
    if (md.has("lines")) {
        w.head += ": строк " + std::to_string(md.get_int("lines", 0));
    }
    if (md.get_int("offset", 1) > 1) {
        w.head += ", с " + std::to_string(md.get_int("offset", 1));
    }
    if (md.get_bool("more", false)) w.head += ", есть ещё";
    if (md.has("limit")) {
        w.head += ", порция " + std::to_string(md.get_int("limit", 0));
    }
    if (path.empty()) add_note(&w.note, "путь файла не пришёл в метаданных");

    if (!append_missing_output(w, view)) {
        append_lines(&w.lines, &w.omitted_lines, view.output);
    }
    return w;
}

Widget build_diff(const CallView& view) {
    Widget w;
    w.kind = Kind::Diff;
    /* ОДИН вызов чтения формы (11.2, отклонение 149): второй разбор
     * развёл бы номера строк и счётчики (Д2, Д12). */
    std::string read_note;
    diff::filediff_from_metadata(view.metadata, &w.files, &read_note);
    size_t additions = 0, deletions = 0;
    for (const diff::FileDiff& fd : w.files) {
        additions += fd.additions;
        deletions += fd.deletions;
    }
    w.head = view.tool + " — файлов: " + std::to_string(w.files.size()) +
             ", +" + std::to_string(additions) + " −" +
             std::to_string(deletions);
    add_note(&w.note, read_note);
    if (w.files.empty()) {
        /* Правки не несёт отказ: «ничего не изменилось» и «прав не было» —
         * это разные вещи, и обе должны быть видны словами, а не пустым
         * виджетом. Причина у них в тексте ответа. */
        add_note(&w.note, "diff пришёл пустым — видно только то, что ответил "
                          "инструмент");
        if (!append_missing_output(w, view)) {
            append_lines(&w.lines, &w.omitted_lines, view.output);
        }
    }
    return w;
}

Widget build_list(const CallView& view) {
    Widget w;
    w.kind = Kind::List;
    const json::JsonValue& md = view.metadata;
    w.head = view.tool + " — ";
    if (md.is_object() && md.has("count")) {
        w.head += "записей: " + std::to_string(md.get_int("count", 0));
        if (md.has("matched") && md.get_int("matched", 0) > md.get_int("count", 0)) {
            w.head += " из " + std::to_string(md.get_int("matched", 0));
        }
        if (md.get_bool("truncated", false)) w.head += ", список усечён";
    } else {
        /* `grep_search` не пишет в метаданные НИЧЕГО, поэтому числа у него
         * нет. Разбирать его текст на число значило бы завести второй
         * источник того же факта (Д2): строки совпадений показываются как
         * есть, а сколько их — инструмент не отдал. */
        w.head += "список из вывода, счётчиков инструмент не отдаёт";
    }
    if (!append_missing_output(w, view)) {
        append_lines(&w.lines, &w.omitted_lines, view.output);
    }
    return w;
}

Widget build_task(const CallView& view) {
    Widget w;
    w.kind = Kind::Task;
    const json::JsonValue& md = view.metadata;
    const std::string agent = md.get_string("agent", "");
    w.head = "task — " + (agent.empty() ? std::string("агент не назван")
                                         : agent);
    if (md.get_bool("background", false)) {
        /* Фоновая постановка возвращается сразу, работы ещё не было: шагов
         * нет и показывать нечего. Ноль шагов означал бы «субагент отработал
         * мгновенно». */
        w.head += ", поставлен в фон";
    } else if (md.has("steps")) {
        w.head += ", шагов " + std::to_string(md.get_int("steps", 0));
    }
    if (agent.empty()) add_note(&w.note, "имя субагента не пришло в метаданных");

    const std::string title = md.get_string("task", "");
    if (title.empty()) {
        add_note(&w.note, "название задачи не пришло");
    } else {
        bool cut = false;
        w.lines.push_back("задача: " +
                          clip(title, limits::kMaxToolDisplayTaskChars, &cut));
        if (cut) add_note(&w.note, "название задачи обрезано для показа");
    }
    if (md.has("depth")) {
        w.lines.push_back("глубина: " + std::to_string(md.get_int("depth", 0)));
    }
    w.child_session = md.get_string("session_id", "");
    if (w.child_session.empty()) {
        add_note(&w.note, "идентификатор сессии субагента не пришёл — "
                          "вложенное дерево построить нечем");
    } else {
        w.lines.push_back("сессия субагента: " + w.child_session);
        /* У фоновой постановки `session_saved` НЕТ: работа ещё идёт, и файл
         * появится после неё. Отсутствие поля и `false` — разные вещи, и
         * сводить их к «не записано» значило бы сказать человеку «дерева не
         * будет» там, где оно ещё не выросло. */
        w.child_pending = md.get_bool("background", false);
        w.child_saved = md.get_bool("session_saved", false);
        if (md.has("session_saved") && !w.child_saved) {
            add_note(&w.note, "сессия субагента не записана на диск — "
                              "вложенное дерево построить нечем");
        }
    }
    return w;
}

/* Знак пункта по статусу. Список статусов — ИЗ engine.h, тот же, что у
 * инструмента: свой список значил бы, что панель считает готовым не то, что
 * посчитал агент (Д2). Нераспознанный статус НЕ становится «pending»
 * молча — он назван, потому что инструмент его тоже нормализовал и сказал
 * об этом в `metadata.normalized`. */
const char* todo_marker(const std::string& status, bool* known) {
    if (status == "completed")   { *known = true; return "[x]"; }
    if (status == "in_progress") { *known = true; return "[~]"; }
    if (status == "cancelled")   { *known = true; return "[-]"; }
    if (status == "pending")     { *known = true; return "[ ]"; }
    *known = false;
    return "[?]";
}

Widget build_checklist(const CallView& view) {
    Widget w;
    w.kind = Kind::Checklist;
    const json::JsonValue& list = view.args;
    if (!list.is_array() || list.size() == 0) {
        w.head = view.tool + " — пункты не пришли в аргументах";
        add_note(&w.note, "чек-лист строится по аргументам `todos`, а их нет");
        if (!append_missing_output(w, view)) {
            append_lines(&w.lines, &w.omitted_lines, view.output);
        }
        return w;
    }

    size_t done = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const json::JsonValue& el = list.at(i);
        if (!el.is_object()) {
            add_note(&w.note, "пункт " + std::to_string(i + 1) +
                                  " не объект — показан как есть");
            if (w.lines.size() < limits::kMaxToolDisplayLines) {
                w.lines.push_back("[?] (пункт не объект)");
            } else {
                ++w.omitted_lines;
            }
            continue;
        }
        const std::string status = el.get_string("status", "pending");
        bool known = true;
        const char* marker = todo_marker(status, &known);
        if (!known) {
            add_note(&w.note, "статус «" + status + "» не распознан — " +
                                  "показан как есть");
        }
        if (status == "completed") ++done;
        if (w.lines.size() < limits::kMaxToolDisplayLines) {
            w.lines.push_back(std::string(marker) + " " +
                              el.get_string("content", ""));
        } else {
            ++w.omitted_lines;
        }
    }
    w.head = view.tool + " — пунктов " + std::to_string(list.size()) +
             ", выполнено " + std::to_string(done);
    const long long normalized = view.metadata.get_int("normalized", 0);
    if (normalized > 0) {
        add_note(&w.note, "инструмент нормализовал " +
                              std::to_string(normalized) +
                              " полей: статус или приоритет не распознан");
    }
    if (view.metadata.has("count") &&
        view.metadata.get_int("count", 0) != static_cast<long long>(list.size())) {
        add_note(&w.note, "инструмент насчитал пунктов " +
                              std::to_string(view.metadata.get_int("count", 0)) +
                              ", а разобрано " + std::to_string(list.size()));
    }
    return w;
}

Widget build_text(const CallView& view) {
    Widget w;
    w.kind = Kind::Text;
    /* Заголовок ставит САМ инструмент («записано core/x.cpp», «git status»):
     * он знает, что сделал, и подставить вместо него имя инструмента значило
     * бы выбросить единственное, что в заголовке есть. */
    w.head = view.title.empty() ? view.tool : view.title;
    if (view.title.empty()) {
        add_note(&w.note, "инструмент не оставил заголовка");
    }
    if (!append_missing_output(w, view)) {
        append_lines(&w.lines, &w.omitted_lines, view.output);
    }
    return w;
}

} // namespace

const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::Command:   return "команда";
        case Kind::Preview:   return "превью";
        case Kind::Diff:      return "правка";
        case Kind::List:      return "список";
        case Kind::Task:      return "субагент";
        case Kind::Checklist: return "чек-лист";
        case Kind::Text:      return "текст вывода";
    }
    return "неизвестно";
}

Kind kind_of(const std::string& tool_name) {
    for (const Entry& e : kEntries) {
        if (tool_name == e.name) return e.kind;
    }
    /* Инструмент, которого в перечне нет (следующая итерация, модуль
     * стороннего плагина), обязан показаться текстом вывода, а не
     * исчезнуть из дерева. */
    return Kind::Text;
}

const std::vector<std::string>& text_kind_tools() {
    static const std::vector<std::string>* names = [] {
        auto* v = new std::vector<std::string>();
        for (const Entry& e : kEntries) {
            if (e.kind == Kind::Text) v->push_back(e.name);
        }
        return v;
    }();
    return *names;
}

size_t classified_tool_count() {
    size_t n = 0;
    for (const Entry& e : kEntries) {
        if (e.kind != Kind::Text) ++n;
    }
    return n;
}

CallView snapshot(const MessagePart& part, size_t max_chars) {
    CallView v;
    v.tool = part.tool_name();
    v.call_id = part.call_id();
    v.status = part.state();
    v.output_cleared = part.output_cleared();
    if (part.has_result()) {
        const ToolOutput& out = part.output();
        v.title = out.title;
        v.truncated = out.truncated;
        /* Признак обреза берётся ИЗ САМОГО ОБРЕЗА, а не из сравнения длин
         * (отклонение 155): на битом тексте результат обреза длиннее
         * исходника, потому что починенный байт занимает три байта. */
        v.output = text::utf8_prefix_chars(out.output, max_chars, &v.clipped);
        v.clipped_chars = text::utf8_chars(v.output);
        v.output_chars = text::utf8_chars(out.output);
        v.has_output = !v.output.empty();
    }
    if (v.status == ToolState::Error) v.error = part.error();

    /* Метаданные копируются для всех видов, кроме отката: у отката их и
     * так нет (проверено чтением), а копировать пустой объект — значило бы
     * платить за то, чего виджет не читает. */
    const Kind kind = kind_of(v.tool);
    if (kind != Kind::Text && part.has_result() &&
        part.output().metadata.is_object()) {
        v.metadata = part.output().metadata;
    }
    /* АРГУМЕНТЫ читаются ровно у двух видов и ровно по одному полю:
     * команда у `bash` и пункты плана у `todowrite`. У остальных виджет
     * показывает то, что инструмент вернул, — выдумывать источник в
     * аргументах значило бы печатать то, чего вызов не делал. */
    if (part.args().is_object()) {
        if (kind == Kind::Command) {
            v.args.set("cli", part.args().get_string("cli", ""));
        } else if (kind == Kind::Checklist) {
            const json::JsonValue* todos = part.args().find("todos");
            if (todos != nullptr) v.args = *todos;
        }
    }
    return v;
}

Snapshots snapshot_open_calls(const std::vector<Message>& history,
                              const std::vector<std::string>& open_call_ids,
                              size_t max_open,
                              size_t max_chars) {
    Snapshots out;
    /* Раскрытых обычно один-два, поэтому поиск линейный: сортировка и
     * бинарный поиск ради списка такого размера — это код без выгоды. */
    const auto is_open = [&open_call_ids](const std::string& id) {
        return std::find(open_call_ids.begin(), open_call_ids.end(), id) !=
               open_call_ids.end();
    };
    /* Один проход по истории, а не поиск каждого вызова отдельно: список
     * раскрытых короткий, а история длинная. Порядок — ПОРЯДОК ЧАСТЕЙ, то
     * есть порядок, в котором вызовы шли по ходу (то же основание, что у
     * дерева). */
    for (const Message& m : history) {
        for (const MessagePart& p : m.parts) {
            if (!p.is(PartKind::Tool)) continue;
            if (!is_open(p.call_id())) continue;
            /* ОДИН снимок на идентификатор, сколько бы частей с ним ни
             * было. Окно ищет тело ПЕРВЫМ совпадением (render_call_body),
             * поэтому вторая часть с тем же идентификатором не нарисовалась
             * бы никогда, а слот предела занимала бы — и часть нужных тел не
             * доехала бы. Повтор и не считается пропущенным: пропущенным
             * остаётся вызов, который ДОЛЖЕН был показаться, но не влез.
             * Идентификаторы в истории повторяются: вызов закрывается по
             * нему (И11.3), и пара частей с одним `call_0` — не выдумка. */
            const bool already_shot =
                std::find_if(out.views.begin(), out.views.end(),
                             [&p](const CallView& v) {
                                 return v.call_id == p.call_id();
                             }) != out.views.end();
            if (already_shot) continue;
            /* Предел применяется ЗДЕСЬ и считается здесь же: второе место,
             * где применяется предел, дало бы рядом второе число, и
             * разошлись бы они тихо. */
            if (out.views.size() >= max_open) {
                ++out.omitted;
                continue;
            }
            out.views.push_back(snapshot(p, max_chars));
        }
    }
    return out;
}

Widget build(const CallView& view) {
    Widget w;
    switch (kind_of(view.tool)) {
        case Kind::Command:   w = build_command(view); break;
        case Kind::Preview:   w = build_preview(view); break;
        case Kind::Diff:      w = build_diff(view); break;
        case Kind::List:      w = build_list(view); break;
        case Kind::Task:      w = build_task(view); break;
        case Kind::Checklist: w = build_checklist(view); break;
        case Kind::Text:      w = build_text(view); break;
    }
    note_clipping(w, view);
    /* Прореживание (И7.9) убирает вывод из КОНТЕКСТА МОДЕЛИ, но в части он
     * остаётся — иначе перезагруженная сессия потеряла бы то, чего не было
     * никогда. Факт очистки виден человеку и без слов читался бы как «вывод
     * урезан по лимиту», то есть как ошибка инструмента. Оговорка одна на
     * все виды и ставится только когда вывод есть и его видно. */
    if (view.output_cleared && view.has_output) {
        add_note(&w.note, "вывод при этом вычеркнут из контекста модели (И7.9)");
    }
    /* Число строк, оставшихся за пределом, и оговорка о нём — в одном
     * месте: оговорка без числа и число без оговорки оба выглядели бы как
     * «показано всё». */
    if (w.omitted_lines > 0) {
        add_note(&w.note, "строк за пределом: " + std::to_string(w.omitted_lines));
    }
    return w;
}

std::string panel_head(size_t shown, size_t omitted) {
    std::string out = "Виджеты вызовов: раскрыто " + std::to_string(shown);
    if (omitted > 0) {
        out += ", за кадром " + std::to_string(omitted) +
               " (предел — показывать, сколько влезло)";
    }
    return out;
}

std::string kinds_line() {
    return "свой вид у " + std::to_string(classified_tool_count()) +
           " инструментов, текстом вывода — " +
           std::to_string(text_kind_tools().size());
}

} // namespace tool_display
} // namespace coder
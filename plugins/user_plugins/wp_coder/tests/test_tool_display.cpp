/*
 * test_tool_display.cpp — И11.4: виджет вызова по типу инструмента.
 *
 * Предмет проверки — РЕШЕНИЯ, а не окно: виджет рисуется ImGui, тестового
 * харнесса для него нет, и 11.13 (golden-снапшоты рендера) сверяет готовую
 * строку и её вид. Поэтому всё, что можно решить без окна, лежит в
 * core/tool_display.h и проверяется здесь, а ui/coder_window.cpp остаётся
 * тонким слоем поверх — как в 11.1, 11.2 и 11.3.
 *
 * Что проверяется и почему именно это:
 *
 *   1. ПЕРЕЧЕНЬ ПОЛОН ПО ЖИВОМУ РЕЕСТРУ. Инструментов 57, а своего вида у
 *      11, и остальные 46 показаны общим «текстом вывода». Проверяется не
 *      «функция вернула что-то», а равенство множеств в обе стороны: новый
 *      инструмент уронит проверку с его именем — ровно как счётчики
 *      инструментов (правило 12).
 *   2. ЧИСЛА И КЛЮЧИ — ИЗ МЕТАДАННЫХ, А НЕ ИЗ ТЕКСТА. Проверяется на живых
 *      вызовах через `ToolsRegistry::run_output` (не `def.handler`, как
 *      требует правило 8 SESSION_START) и на фикстурах, где текст ответа
 *      ПРОТИВОРЕЧИТ метаданным: показанное отличает «прочитал ключ» от
 *      «нашёл число в тексте».
 *   3. ЧЕГО НЕТ — НАЗЫВАЕТСЯ СЛОВАМИ. `grep_search` не пишет счётчиков, у
 *      `bash` нет ключа отмены, `todoread` не несёт пунктов плана: все три
 *      случая проверяются утверждением на ОТСУТСТВИЕ, а не «вроде показано».
 *   4. ГРАНИЦЫ ПОКАЗА. Обрез по символам, а не по байтам; признак обреза из
 *      самого обреза, а не из сравнения длин (отклонение 155); предел
 *      применяет сбор снимков, и он же считает пропущенное.
 *
 * Фикстуры двух сортов, и они не взаимозаменяемы: ЖИВЫЕ вызовы — там, где
 * важно доказать, что ключи читаются с настоящего результата инструмента
 * (инвентарь в шапке core/tool_display.h снят именно с них); ФИКТУРЫ — там,
 * где состояние нельзя вызвать или вызывать нечем: субагент без провайдера,
 * мусор в метаданных, четыре состояния отсутствующего вывода, битый байт в
 * выводе.
 */

#include "test_framework.h"
#include "test_support.h"

#include "../core/base_tools.h"
#include "../core/engine.h"
#include "../core/git_tools.h"
#include "../core/json.h"
#include "../core/limits.h"
#include "../core/message.h"
#include "../core/tool_display.h"
#include "../core/tools_registry.h"
#include "../modules/devops/devops_tools.h"
#include "../modules/python/python_tools.h"
#include "../modules/wordpress/wp_tools.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace coder;

namespace {

/* Полный набор инструментов. Модули регистрируются напрямую (как в
 * test_tool.cpp), чтобы сверка полноты перечня видела тот же реестр, что и
 * счётчики инструментов. */
void register_all_tools() {
    static bool done = false;
    if (done) return;
    done = true;
    test_support::approve_all_permissions();
    register_base_tools();
    register_rag_tools();
    register_git_tools();
    wp::register_wp_tools();
    python::register_python_tools();
    devops::register_devops_tools();
}

/* Настоящие инструменты реестра: синтетические `test_*` исключены
 * префиксом — иначе счётчик зависел бы от того, какие тесты уже
 * отработали (правило 12). */
std::vector<std::string> live_tools() {
    std::vector<std::string> out;
    for (const ToolDef& d : ToolsRegistry::instance().defs()) {
        if (d.name.empty() || d.name.rfind("test_", 0) == 0) continue;
        out.push_back(d.name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

fs::path make_tmp_tree(const char* tag) {
    const fs::path tmp = fs::temp_directory_path() /
        ("wp_coder_tdisp_" + std::string(tag) + "_" + std::to_string(::getpid()) +
         "_" + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    /* И11.5: каталог убирается в конце прогона (test_framework.h), а не остаётся в /tmp до следующего. */
    register_tmp_tree(tmp.string());
    return tmp;
}

void put_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

/* Engine для живых вызовов: тот же порядок, что в test_diff_tool.cpp. */
void init_engine(const fs::path& project) {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&,
                         std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = project.string();
        engine_state().allowed_external_paths.clear();
        engine_state().plan_mode = false;
        engine_state().pending.clear();
    }
}

/* Вызов инструмента по-настоящему: с полным результатом и метаданными. */
ToolOutput call(const std::string& name, const json::JsonValue& args) {
    return ToolsRegistry::instance().run_output(name, args);
}

json::JsonValue args_of(std::initializer_list<std::pair<const char*, std::string>> kv) {
    json::JsonValue a = json::JsonValue::object();
    for (const auto& p : kv) a.set(p.first, p.second);
    return a;
}

/* Снимок из ЖИВОГО вызова: ровно то, что увидит панель. */
tool_display::CallView live_view(const std::string& name,
                                 const json::JsonValue& args,
                                 size_t max_chars = limits::kMaxToolDisplayChars) {
    MessagePart p = MessagePart::tool("prt_00000000001", name, args);
    p.set_running();
    p.set_result(call(name, args));
    return tool_display::snapshot(p, max_chars);
}

/* Снимок из ФИКТУРЫ: состояние и содержимое заданы руками, потому что живого
 * вызова с таким содержимым не бывает (субагент без провайдера, мусор в
 * метаданных, четыре состояния отсутствующего вывода, битый байт). Снимок
 * всё равно снимает ПРОИЗВОДСТВЕННЫЙ код — иначе проверка обреза и снимка
 * проверяла бы фикстуру, а не код. */
tool_display::CallView fixture_view(const std::string& tool,
                                    const json::JsonValue& metadata,
                                    const std::string& output = std::string(),
                                    const json::JsonValue& args = json::JsonValue(),
                                    ToolState state = ToolState::Completed) {
    MessagePart p = MessagePart::tool("prt_00000000002", tool, args);
    if (state == ToolState::Error) {
        p.set_running();
        p.set_error("отказ по правам");
        return tool_display::snapshot(p, limits::kMaxToolDisplayChars);
    }
    if (state == ToolState::Pending || state == ToolState::Running) {
        if (state == ToolState::Running) p.set_running();
        return tool_display::snapshot(p, limits::kMaxToolDisplayChars);
    }
    p.set_running();
    ToolOutput o;
    o.title = "заголовок " + tool;
    o.output = output;
    if (metadata.is_object()) o.metadata = metadata;
    p.set_result(o);
    return tool_display::snapshot(p, limits::kMaxToolDisplayChars);
}

bool has_line(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& l : lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string line_starting(const std::vector<std::string>& lines,
                          const std::string& prefix) {
    for (const std::string& l : lines) {
        if (l.rfind(prefix, 0) == 0) return l;
    }
    return std::string();
}

/* Символы UTF-8 в строке — счёт САМОЙ ПРОВЕРКИ, а не вызов того же кода,
 * который проверяют: ожидание, посчитанное тем же счётчиком, ничего не
 * проверяет. */
size_t chars_of(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

/* Первые `n` символов строки — эталон обреза, посчитанный своими глазами:
 * обрез по байтам разрезал бы кириллицу пополам, и сравнение с этим эталоном
 * поймало бы разницу, а сравнение `size()` — нет. */
std::string first_chars(const std::string& s, size_t n) {
    size_t pos = 0, seen = 0;
    while (pos < s.size() && seen < n) {
        const unsigned char c = static_cast<unsigned char>(s[pos]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (pos + len > s.size()) len = 1;
        pos += len;
        ++seen;
    }
    return s.substr(0, pos);
}

/* Часть-вызов с заданным содержимым — для проверок границ показа. */
MessagePart part_with_result(const char* call_id, const std::string& tool,
                             const std::string& output,
                             const json::JsonValue& metadata,
                             const json::JsonValue& args) {
    MessagePart p = MessagePart::tool(call_id, tool, args);
    p.set_running();
    ToolOutput o;
    o.output = output;
    if (metadata.is_object()) o.metadata = metadata;
    p.set_result(o);
    return p;
}

Message assistant_message(const char* id, std::vector<MessagePart> parts) {
    Message m = Message::assistant("");
    m.id = id;
    for (MessagePart& p : parts) m.parts.push_back(std::move(p));
    return m;
}

} // namespace

/* ======================================================================
 * Перечень видов полон по живому реестру
 * ====================================================================== */

TEST(every_live_tool_has_a_display_kind) {
    register_all_tools();
    const std::vector<std::string> names = live_tools();
    ASSERT_TRUE(names.size() > 40);
    for (const std::string& name : names) {
        const tool_display::Kind k = tool_display::kind_of(name);
        if (k == tool_display::Kind::Command || k == tool_display::Kind::Preview ||
            k == tool_display::Kind::Diff || k == tool_display::Kind::List ||
            k == tool_display::Kind::Task || k == tool_display::Kind::Checklist ||
            k == tool_display::Kind::Text) {
            continue;
        }
        std::cerr << "  инструмент " << name << " получил неизвестный вид"
                  << std::endl;
        ASSERT_TRUE(false);
    }
}

TEST(a_live_tool_without_its_own_kind_is_named_in_the_fallback_list) {
    /* ГЛАВНОЕ свойство перечня: откат «текст вывода» не молчит. Инструмент,
     * которому не назначен вид, обязан быть ПЕРЕЧИСЛЕН, иначе человек увидел
     * бы одинаковые серые блоки и не отличил бы «автор не описал этот
     * инструмент» от «инструменту нечего показать». */
    register_all_tools();
    const std::vector<std::string> fallback = tool_display::text_kind_tools();
    for (const std::string& name : live_tools()) {
        if (tool_display::kind_of(name) != tool_display::Kind::Text) continue;
        if (std::find(fallback.begin(), fallback.end(), name) != fallback.end()) {
            continue;
        }
        std::cerr << "  инструмент " << name
                  << " показан откатом, но его нет в перечне" << std::endl;
        ASSERT_TRUE(false);
    }
}

TEST(the_fallback_list_has_no_tool_that_has_a_kind_of_its_own) {
    /* Обратная сторона того же равенства: перечень отката не должен содержать
     * инструмент со своим видом — иначе панель называла бы «текстом вывода»
     * то, что описано, и число «сколько не описано» в шапке разошлось бы с
     * тем, что показано на самом деле. */
    register_all_tools();
    for (const std::string& name : tool_display::text_kind_tools()) {
        const tool_display::Kind k = tool_display::kind_of(name);
        if (k == tool_display::Kind::Text) continue;
        std::cerr << "  в перечне отката " << name << ", а у него вид "
                  << tool_display::kind_name(k) << std::endl;
        ASSERT_TRUE(false);
    }
}

TEST(the_display_kinds_are_counted_exactly) {
    /* Числа, а не «≥»: молчаливое добавление инструмента мимо счётчика
     * означало бы, что проверка перестала быть проверкой (то же основание,
     * что у every_tool_has_permission_key). */
    register_all_tools();
    size_t per_kind[7] = {0, 0, 0, 0, 0, 0, 0};
    for (const std::string& name : live_tools()) {
        ++per_kind[static_cast<int>(tool_display::kind_of(name))];
    }
    ASSERT_EQ(per_kind[static_cast<int>(tool_display::Kind::Command)], (size_t)1);
    ASSERT_EQ(per_kind[static_cast<int>(tool_display::Kind::Preview)], (size_t)1);
    ASSERT_EQ(per_kind[static_cast<int>(tool_display::Kind::Diff)], (size_t)4);
    ASSERT_EQ(per_kind[static_cast<int>(tool_display::Kind::List)], (size_t)3);
    ASSERT_EQ(per_kind[static_cast<int>(tool_display::Kind::Task)], (size_t)1);
    ASSERT_EQ(per_kind[static_cast<int>(tool_display::Kind::Checklist)], (size_t)1);
    ASSERT_EQ(per_kind[static_cast<int>(tool_display::Kind::Text)], (size_t)46);
    /* 57 живых инструментов: 11 со своим видом и 46 откатом. */
    ASSERT_EQ(tool_display::classified_tool_count(), (size_t)11);
    ASSERT_EQ(tool_display::text_kind_tools().size(), (size_t)46);
    ASSERT_EQ(tool_display::classified_tool_count() +
                  tool_display::text_kind_tools().size(),
              (size_t)57);
}

TEST(an_instrument_nobody_classified_still_shows_its_output) {
    /* Инструмент из следующей итерации обязан показаться текстом, а не
     * исчезнуть из дерева: пустой виджет читался бы как «инструмент не
     * отработал». */
    register_all_tools();
    ASSERT_TRUE(tool_display::kind_of("wp_coder_инструмента_нет") ==
                tool_display::Kind::Text);
    const tool_display::CallView v = fixture_view(
        "wp_coder_инструмента_нет", json::JsonValue::object(),
        "строка вывода");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::Text);
    ASSERT_TRUE(has_line(w.lines, "строка вывода"));
}

TEST(the_kinds_line_reports_the_same_counts_as_the_table) {
    /* Числа в шапке панели и числа в перечне — один источник. Расхождение
     * означало бы, что перечень расползся, и человек читал бы «показано всё»
     * там, где половина инструментов не описана. */
    register_all_tools();
    const std::string line = tool_display::kinds_line();
    ASSERT_TRUE(line.find("свой вид у 11 инструментов") != std::string::npos);
    ASSERT_TRUE(line.find("текстом вывода — 46") != std::string::npos);
    const std::string head = tool_display::panel_head(3, 2);
    ASSERT_TRUE(head.find("раскрыто 3") != std::string::npos);
    ASSERT_TRUE(head.find("за кадром 2") != std::string::npos);
    /* Пока ничего не пропущено — числа «за кадром» нет вовсе: называть ноль
     * означало бы намекать, что что-то спрятано. */
    ASSERT_TRUE(tool_display::panel_head(3, 0).find("за кадром") ==
                std::string::npos);
}

/* ======================================================================
 * Снимок: что копируется и как обрезается
 * ====================================================================== */

TEST(the_snapshot_takes_the_command_and_the_metadata_but_nothing_else) {
    register_all_tools();
    const fs::path project = make_tmp_tree("snap");
    init_engine(project);
    const json::JsonValue args =
        args_of({{"cli", "printf 'привет\\n'"}, {"timeout", "5"}});
    const tool_display::CallView v = live_view("bash", args);
    ASSERT_EQ(v.tool, std::string("bash"));
    /* Из аргументов берётся РОВНО одно поле: команда. Таймаут виджет не
     * показывает, и копировать его под локом незачем. */
    ASSERT_EQ(v.args.get_string("cli", ""), std::string("printf 'привет\\n'"));
    ASSERT_TRUE(!v.args.has("timeout"));
    /* Код возврата приходит из метаданных инструмента. */
    ASSERT_EQ(v.metadata.get_int("exit_code", -1), (long long)0);
    ASSERT_TRUE(v.has_output);
}

TEST(a_tool_without_its_own_kind_is_snapshotted_without_metadata) {
    /* Откат «текст вывода» ничего не читает из метаданных: у 46 инструментов
     * их и так нет, и копировать их под локом ради ничего — значило бы
     * платить за невидимое. */
    register_all_tools();
    const fs::path project = make_tmp_tree("snaptext");
    put_file(project / "a.txt", "alpha\n");
    init_engine(project);
    const tool_display::CallView v = live_view("repo_map", json::JsonValue::object());
    ASSERT_EQ(v.tool, std::string("repo_map"));
    ASSERT_FALSE(v.metadata.is_object());
    ASSERT_TRUE(v.has_output);
}

TEST(the_snapshot_cut_is_by_characters_and_the_text_says_it) {
    /* Кириллица — два байта на букку. Обрез по байтам разрезал бы русский
     * вывод пополам, и человек увидел бы битый байт (отклонение 155). */
    std::string body;
    for (int i = 0; i < 40; ++i) {
        body += "строка вывода номер " + std::to_string(i) + "\n";
    }
    const size_t cap = 100;
    MessagePart p = part_with_result("prt_00000000003", "bash", body,
                                     json::JsonValue::object(), json::JsonValue());
    const tool_display::CallView v = tool_display::snapshot(p, cap);
    ASSERT_TRUE(v.clipped);
    /* Эталон обреза посчитан своими глазами: обрез по символам даёт ровно
     * первые cap символов, обрез по байтам дал бы другое. */
    ASSERT_EQ(v.output, first_chars(body, cap));
    ASSERT_EQ(chars_of(v.output), (size_t)cap);
    ASSERT_TRUE(v.output_chars > v.clipped_chars);
    /* Виджет называет обрез ЧИСЛАМИ: молчаливое усечение выглядело бы как
     * «вывод был такой». */
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.note.find("частично") != std::string::npos);
    ASSERT_TRUE(w.note.find(std::to_string(cap)) != std::string::npos);
    ASSERT_TRUE(w.note.find(std::to_string(v.output_chars)) != std::string::npos);
}

TEST(a_broken_byte_in_the_output_does_not_hide_the_cut) {
    /* Признак «обрезано» берётся ИЗ САМОГО ОБРЕЗА. Сравнение длин на битом
     * тексте сказало бы «обрезания не было»: починенный байт занимает ТРИ
     * байта вместо одного, то есть результат обреза оказывается ДЛИННЕ
     * исходника. Ниже это ещё и проверяется числами: без утверждения
     * `output.size() > body.size()` тест проходил бы и на коде, который
     * обрезает правильно. */
    std::string body = "0123456789ab";
    body.push_back(static_cast<char>(0x80));   /* битый байт ровно на краю */
    body += "X";                              /* и хвост после него */
    const size_t cap = 13;                     /* обрез приходится на него */
    MessagePart p = part_with_result("prt_00000000004", "bash", body,
                                     json::JsonValue::object(), json::JsonValue());
    const tool_display::CallView v = tool_display::snapshot(p, cap);
    ASSERT_TRUE(v.clipped);
    ASSERT_EQ(chars_of(v.output), (size_t)cap);
    /* Вот тот случай, где длины бесполезны: починенный байт (три байта)
     * весит больше, чем отброшенный хвост, и результат ДЛИННЕ исходника. */
    ASSERT_TRUE(v.output.size() > body.size());
}

TEST(an_output_shorter_than_the_cap_is_not_reported_as_cut) {
    register_all_tools();
    const fs::path project = make_tmp_tree("snapok");
    put_file(project / "a.txt", "alpha\n");
    init_engine(project);
    const tool_display::CallView v = live_view("repo_map", json::JsonValue::object());
    ASSERT_FALSE(v.clipped);
    ASSERT_EQ(v.clipped_chars, v.output_chars);
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.note.find("частично") == std::string::npos);
}

/* ======================================================================
 * bash: команда, вывод, код возврата
 * ====================================================================== */

TEST(bash_shows_the_command_the_output_and_the_exit_code) {
    register_all_tools();
    const fs::path project = make_tmp_tree("bash1");
    init_engine(project);
    const tool_display::CallView v =
        live_view("bash", args_of({{"cli", "printf 'привет\\n'"}}));
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::Command);
    ASSERT_TRUE(w.head.find("exit 0") != std::string::npos);
    ASSERT_TRUE(has_line(w.lines, "$ printf 'привет\\n'"));
    ASSERT_TRUE(has_line(w.lines, "привет"));
}

TEST(the_exit_code_comes_from_the_metadata_and_not_from_the_text) {
    /* Различающий случай: текст ответа говорит одно, метаданные — другое.
     * Панель обязана показать число ИЗ МЕТАДАННЫХ: текст приходит от
     * инструмента строкой и может быть любым, а код возврата — факт о
     * процессе, и он лежит в поле. */
    json::JsonValue md = json::JsonValue::object();
    md.set("exit_code", static_cast<long long>(7));
    const tool_display::CallView v =
        fixture_view("bash", md, "exit=0\nвсё хорошо");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.head.find("exit 7") != std::string::npos);
    ASSERT_TRUE(w.head.find("exit 0") == std::string::npos);
}

TEST(a_failing_command_shows_its_real_code_not_zero) {
    register_all_tools();
    const fs::path project = make_tmp_tree("bash2");
    init_engine(project);
    /* Живой неуспешный вызов: `ls` несуществующего файла ВНУТРИ проекта
     * возвращает не ноль. Путь внутри проекта выбран не случайно: команда с
     * путём снаружиProject упирается в гейт внешних каталогов и возвращает
     * отказ без метаданных — первая версия проверки падала именно на этом и
     * проверяла бы «поле есть», а не значение. */
    const tool_display::CallView v = live_view(
        "bash", args_of({{"cli", "ls wp_coder_нет_такого_файла"}}));
    ASSERT_TRUE(v.metadata.is_object());
    ASSERT_EQ(v.metadata.get_int("exit_code", -1), (long long)2);
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.head.find("exit 0") == std::string::npos);
    ASSERT_TRUE(w.head.find("exit 2") != std::string::npos);
}

TEST(a_bash_refused_by_policy_has_no_exit_code_to_show) {
    /* Отказ политикой команд возвращает текст без метаданных: кода возврата
     * нет, и панель говорит «неизвестен», а не «0» — ноль означал бы
     * «отработала успешно». */
    register_all_tools();
    const fs::path project = make_tmp_tree("bash3");
    init_engine(project);
    const tool_display::CallView v = live_view("bash", args_of({{"cli", "rm -rf /"}}));
    ASSERT_FALSE(v.metadata.is_object());
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.head.find("exit неизвестен") != std::string::npos);
    ASSERT_TRUE(w.head.find("exit 0") == std::string::npos);
    ASSERT_TRUE(has_line(w.lines, "запрещено"));
}

TEST(a_bash_that_was_stopped_says_nothing_about_a_code_it_does_not_have) {
    /* Признак отмены человеком (`cancelled`) лежит только в ТЕКСТЕ ответа
     * bash, в метаданные инструмент его не кладёт. Проверяется именно
     * ОТСУТСТВИЕ: панель не имеет права ни выдумывать ключ, ни разбирать
     * чужой текст на «отмена». */
    json::JsonValue md = json::JsonValue::object();
    md.set("exit_code", static_cast<long long>(143));
    const tool_display::CallView v = fixture_view(
        "bash", md, "<shell_metadata> exit=143 cancelled=true\n</shell_metadata>");
    ASSERT_TRUE(!v.metadata.has("cancelled"));
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.head.find("отмен") == std::string::npos);
    ASSERT_TRUE(w.head.find("exit 143") != std::string::npos);
}

TEST(a_command_stopped_by_timeout_says_so) {
    /* Ключ `timed_out` пишет сам инструмент, и панель читает его. До этой
     * проверки ветка «прервано по таймауту» не была покрыта ничем, и снятие
     * её целиком было зелёным — то есть целая строка заголовка могла не
     * появиться, а прогон об этом не узнал бы. Найдено прогоном мутаций.
     *
     * ФИКТУРА, а не живой вызов, и это не недосмотр: чтобы инструмент
     * успел написать `timed_out`, команда обязана ПЕРЕЖИТЬ свой таймаут, то
     * еть нужно что-то, что долго работает. Единственное, что приходит в
     * голову, — `sleep`, и он отклонён закрытым списком разрешённых программ
     * (И3): allow_all из `approve_all_permissions()` открывает слой
     * разрешений, но не список бинарников. Проверено живьём — вызов вернул
     * «sleep нет в списке разрешённых программ» и пришёл БЕЗ метаданных,
     * то есть проверка зеленела бы на отказе политики, а не на таймауте.
     * Что `timed_out` пишет инструмент, — факт о `core/base_tools.cpp`, а
     * не предмет 11.4; предмет 11.4 — что панель его читает. */
    json::JsonValue md = json::JsonValue::object();
    md.set("exit_code", static_cast<long long>(124));
    md.set("timed_out", true);
    const tool_display::CallView v =
        fixture_view("bash", md, "команда прервана по таймауту 1 с");
    ASSERT_TRUE(v.metadata.get_bool("timed_out", false));
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.head.find("таймаут") != std::string::npos);
}

TEST(the_widget_does_not_read_a_cancel_key_the_tool_never_writes) {
    /* Отмена человеком — единственный факт о bash, который лежит ТОЛЬКО в
     * тексте ответа (`<shell_metadata> … cancelled=true`), и в метаданные
     * не копируется. Проверка на ОТСУТСТВИЕ ключа доказывает, что панель не
     * разбирает чужой текст, но не доказывает, что она не читает
     * несуществующее поле: поле с таким именем в метаданных не кладёт ни
     * один инструмент, и виджет, читающий его, показал бы человеку то,
     * чего вызов не сообщал. Именно это и проверяется. */
    json::JsonValue md = json::JsonValue::object();
    md.set("exit_code", static_cast<long long>(0));
    md.set("cancelled", true);
    const tool_display::CallView v =
        fixture_view("bash", md, "вывод без единого слова об отмене");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.head.find("отмен") == std::string::npos);
    ASSERT_TRUE(w.head.find("exit 0") != std::string::npos);
}

TEST(a_long_command_output_is_cut_by_lines_and_the_rest_is_named) {
    /* Предел строк считает ТЕЛО виджета, а не снимок, и пропущенное
     * называется числом. */
    std::string body;
    for (int i = 0; i < 60; ++i) body += "строка " + std::to_string(i) + "\n";
    const tool_display::CallView v = fixture_view(
        "bash", json::JsonValue::object(), body);
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_EQ(w.lines.size(), limits::kMaxToolDisplayLines);
    ASSERT_EQ(w.omitted_lines, (size_t)(60 - limits::kMaxToolDisplayLines));
    ASSERT_TRUE(w.note.find("строк за пределом") != std::string::npos);
}

/* ======================================================================
 * read_file: превью
 * ====================================================================== */

TEST(read_file_previews_the_content_and_names_the_file) {
    register_all_tools();
    const fs::path project = make_tmp_tree("read1");
    put_file(project / "note.txt", "первая строка\nвторая строка\n");
    init_engine(project);
    const tool_display::CallView v = live_view("read_file", args_of({{"path", "note.txt"}}));
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::Preview);
    ASSERT_TRUE(w.head.find("note.txt") != std::string::npos);
    ASSERT_TRUE(w.head.find("строк") != std::string::npos);
    ASSERT_TRUE(has_line(w.lines, "первая строка"));
    ASSERT_TRUE(has_line(w.lines, "вторая строка"));
}

TEST(read_file_says_when_more_of_the_file_is_left) {
    /* Признак `more` лежит в метаданных инструмента, и без него превью
     * читалось бы как «файл закончился» — то есть человек решил бы, что
     * прочитал файл целиком. */
    register_all_tools();
    const fs::path project = make_tmp_tree("read2");
    std::string body;
    for (int i = 0; i < 40; ++i) body += "строка " + std::to_string(i) + "\n";
    put_file(project / "big.txt", body);
    init_engine(project);
    json::JsonValue args = json::JsonValue::object();
    args.set("path", std::string("big.txt"));
    args.set("limit", std::string("5"));
    const tool_display::CallView v = live_view("read_file", args);
    ASSERT_TRUE(v.metadata.get_bool("more", false));
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.head.find("есть ещё") != std::string::npos);
}

/* ======================================================================
 * Пишущие инструменты: виджет diff
 * ====================================================================== */

TEST(a_written_file_is_shown_as_a_diff_with_its_counters) {
    register_all_tools();
    const fs::path project = make_tmp_tree("diff1");
    put_file(project / "f.txt", "one\ntwo\nthree\n");
    init_engine(project);
    const tool_display::CallView v =
        live_view("write_file", args_of({{"path", "f.txt"},
                                         {"content", "one\nTWO\nthree\nfour\n"}}));
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::Diff);
    ASSERT_EQ(w.files.size(), (size_t)1);
    ASSERT_EQ(w.files[0].file, std::string("f.txt"));
    /* Счётчики пересчитаны по патчу, который и рисуется, а не взяты из
     * метаданных вслепую (11.2, отклонение 149). */
    ASSERT_EQ(w.files[0].additions, (size_t)2);
    ASSERT_EQ(w.files[0].deletions, (size_t)1);
    ASSERT_TRUE(w.head.find("+2") != std::string::npos);
    ASSERT_TRUE(w.head.find("−1") != std::string::npos);
    ASSERT_TRUE(w.note.empty());
}

TEST(a_refused_write_says_in_words_that_there_is_no_diff) {
    /* Правки не несёт отказ: «ничего не изменилось» и «прав не было» —
     * разные вещи, и обе должны быть видны словами, а не пустым виджетом. */
    register_all_tools();
    const fs::path project = make_tmp_tree("diff2");
    init_engine(project);
    const tool_display::CallView v = live_view(
        "write_file", args_of({{"path", "../вне_проекта.txt"},
                               {"content", "x\n"}}));
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::Diff);
    ASSERT_EQ(w.files.size(), (size_t)0);
    ASSERT_TRUE(w.note.find("diff пришёл пустым") != std::string::npos);
    ASSERT_TRUE(has_line(w.lines, "запрещено"));
}

TEST(an_unreadable_filediff_is_named_and_not_hidden) {
    /* Форму читает ОДИН вызов `filediff_from_metadata` (отклонение 149), и
     * его оговорка обязана дойти до человека: иначе один битый патч выглядел
     * бы как «правок не было». */
    json::JsonValue bad = json::JsonValue::object();
    bad.set("filediff", std::string("это не список файлов"));
    const tool_display::CallView v =
        fixture_view("write_file", bad, "записано f.txt");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_EQ(w.files.size(), (size_t)0);
    /* Именно ЭТА оговорка, а не «оговорка вообще». Первая версия проверяла
     * `note` на непустоту, и этого хватало: рядом всегда ставится «diff
     * пришёл пустым», то есть снятие оговорки разбора оставляло проверку
     * зелёной — прогон мутаций и поймал. Правка проверки, а не кода. */
    ASSERT_TRUE(w.note.find("metadata.filediff") != std::string::npos);
    ASSERT_TRUE(has_line(w.lines, "записано f.txt"));
}

TEST(a_write_in_plan_mode_is_shown_as_a_proposal_and_not_as_a_diff) {
    /* Режим плана: правка предложена, а не применена. Предложение показывается
     * (человеку надо видеть, что агент собрался изменить), и diff у него нет
     * — показывать пустую правку было бы враньём. */
    register_all_tools();
    const fs::path project = make_tmp_tree("diff3");
    put_file(project / "p.txt", "старое\n");
    init_engine(project);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().plan_mode = true;
    }
    const tool_display::CallView v = live_view(
        "write_file", args_of({{"path", "p.txt"}, {"content", "новое\n"}}));
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().plan_mode = false;
    }
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_EQ(w.files.size(), (size_t)0);
    ASSERT_TRUE(has_line(w.lines, "предложено"));
}

/* ======================================================================
 * Поиск и обход: список
 * ====================================================================== */

TEST(glob_lists_paths_and_takes_its_count_from_the_metadata) {
    register_all_tools();
    const fs::path project = make_tmp_tree("list1");
    put_file(project / "one.txt", "a\n");
    put_file(project / "sub" / "two.txt", "b\n");
    init_engine(project);
    const tool_display::CallView v =
        live_view("glob", args_of({{"pattern", "**/*.txt"}}));
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::List);
    ASSERT_EQ(v.metadata.get_int("count", -1), (long long)2);
    ASSERT_TRUE(w.head.find("записей: 2") != std::string::npos);
    ASSERT_TRUE(has_line(w.lines, "one.txt"));
    ASSERT_TRUE(has_line(w.lines, "two.txt"));
}

TEST(grep_search_has_no_counters_and_the_panel_says_so) {
    /* Инвентарь, а не украшение: `grep_search` не пишет в метаданные НИЧЕГО,
     * поэтому числа у него нет — и разбирать его текст на число панель не
     * станет (второй источник того же факта, Д2). Проверяется утверждение на
     * ОТСУТСТВИЕ метаданных, а не «вроде показалось». */
    register_all_tools();
    const fs::path project = make_tmp_tree("list2");
    put_file(project / "code.txt", "alpha\nbeta\n");
    init_engine(project);
    const tool_display::CallView v =
        live_view("grep_search", args_of({{"pattern", "beta"}}));
    ASSERT_FALSE(v.metadata.is_object());
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.head.find("счётчиков инструмент не отдаёт") != std::string::npos);
    /* Числа в шапке нет вовсе — утверждением на ОТСУТСТВИЕ. Проверка только
     * на слова о счётчиках держалась бы на коде, который печатает рядом ещё
     * и выдуманное «записей: 0»: оговорка осталась бы, ложь появилась бы
     * (найдено прогоном мутаций). */
    ASSERT_TRUE(w.head.find("записей") == std::string::npos);
    ASSERT_TRUE(has_line(w.lines, "code.txt"));
}

TEST(a_capped_search_result_says_it_was_capped) {
    /* Признак усечения лежит в метаданных инструмента; молчаливый список
     * выглядел бы как «это все совпадения». */
    json::JsonValue md = json::JsonValue::object();
    md.set("count", static_cast<long long>(100));
    md.set("matched", static_cast<long long>(400));
    md.set("truncated", true);
    const tool_display::CallView v = fixture_view("glob", md, "a.txt\nb.txt\n");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.head.find("записей: 100 из 400") != std::string::npos);
    ASSERT_TRUE(w.head.find("список усечён") != std::string::npos);
}

/* ======================================================================
 * План задачи: чек-лист
 * ====================================================================== */

TEST(todowrite_is_shown_as_a_checklist_built_from_its_arguments) {
    register_all_tools();
    const fs::path project = make_tmp_tree("todo1");
    init_engine(project);
    json::JsonValue todos = json::JsonValue::array();
    const char* contents[3] = {"прочитать файл", "починить тест", "записать"};
    const char* statuses[3] = {"completed", "in_progress", "pending"};
    for (int i = 0; i < 3; ++i) {
        json::JsonValue item = json::JsonValue::object();
        item.set("content", std::string(contents[i]));
        item.set("status", std::string(statuses[i]));
        todos.push_back(std::move(item));
    }
    json::JsonValue args = json::JsonValue::object();
    args.set("todos", todos);
    const tool_display::CallView v = live_view("todowrite", args);
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::Checklist);
    ASSERT_TRUE(w.head.find("пунктов 3") != std::string::npos);
    ASSERT_TRUE(w.head.find("выполнено 1") != std::string::npos);
    ASSERT_EQ(line_starting(w.lines, "[x]"), std::string("[x] прочитать файл"));
    ASSERT_EQ(line_starting(w.lines, "[~]"), std::string("[~] починить тест"));
    ASSERT_EQ(line_starting(w.lines, "[ ]"), std::string("[ ] записать"));
    ASSERT_TRUE(w.note.empty());
}

TEST(an_unknown_todo_status_is_shown_as_it_is_and_named) {
    /* Статус приходит от модели текстом. Нераспознанный НЕ превращается в
     * «pending» молча: инструмент его нормализует и говорит об этом в
     * `metadata.normalized`, и панель обязана сказать то же. */
    register_all_tools();
    const fs::path project = make_tmp_tree("todo2");
    init_engine(project);
    json::JsonValue todos = json::JsonValue::array();
    json::JsonValue item = json::JsonValue::object();
    item.set("content", std::string("переименовать пункт"));
    item.set("status", std::string("перепроверить"));
    todos.push_back(std::move(item));
    json::JsonValue args = json::JsonValue::object();
    args.set("todos", todos);
    const tool_display::CallView v = live_view("todowrite", args);
    ASSERT_TRUE(v.metadata.get_int("normalized", 0) > 0);
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(has_line(w.lines, "[?]"));
    ASSERT_TRUE(has_line(w.lines, "переименовать пункт"));
    ASSERT_TRUE(w.note.find("перепроверить") != std::string::npos);
    ASSERT_TRUE(w.note.find("нормализовал") != std::string::npos);
    /* Нераспознанный статус не в счёт «выполнено»: он не completed. */
    ASSERT_TRUE(w.head.find("выполнено 0") != std::string::npos);
}

TEST(a_todowrite_without_todos_in_its_arguments_says_so) {
    /* Чек-лист строится по аргументам, и их может не быть: чужой
     * производитель, старая сессия, сломанный блок вызова. Молчаливая пустая
     * панель читалась бы как «план пуст». */
    const tool_display::CallView v = fixture_view(
        "todowrite", json::JsonValue::object(), "план записан");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::Checklist);
    ASSERT_TRUE(w.head.find("пункты не пришли") != std::string::npos);
    ASSERT_TRUE(has_line(w.lines, "план записан"));
}

TEST(todoread_is_shown_as_plain_text_and_not_as_a_checklist) {
    /* Решение задачи, а не забывка: у `todoread` нет ни аргументов, ни
     * метаданных с пунктами. Чек-лист пришлось бы собрать, разобрав
     * ОТРЕНДЕРЕННЫЙ текст, — это второй разбор одного формата (Д2). */
    register_all_tools();
    ASSERT_TRUE(tool_display::kind_of("todoread") == tool_display::Kind::Text);
    const tool_display::CallView v = live_view("todoread", json::JsonValue::object());
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::Text);
    /* Снимок отката не копирует аргументы: у него их и читать нечем. */
    ASSERT_FALSE(v.args.is_object());
}

/* ======================================================================
 * Субагент: вложенное дерево
 * ====================================================================== */

TEST(a_subagent_call_names_the_agent_the_task_and_the_session) {
    register_all_tools();
    json::JsonValue md = json::JsonValue::object();
    md.set("agent", std::string("wp_explore"));
    md.set("depth", static_cast<long long>(1));
    md.set("steps", static_cast<long long>(3));
    md.set("task", std::string("найти, где падает тест"));
    md.set("session_id", std::string("ses_000000000042"));
    md.set("session_saved", true);
    const tool_display::CallView v = fixture_view("task", md, "ответ субагента");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::Task);
    ASSERT_TRUE(w.head.find("wp_explore") != std::string::npos);
    ASSERT_TRUE(w.head.find("шагов 3") != std::string::npos);
    ASSERT_TRUE(has_line(w.lines, "задача: найти, где падает тест"));
    ASSERT_TRUE(has_line(w.lines, "сессия субагента: ses_000000000042"));
    /* Идентификатор идёт ОТДЕЛЬНЫМ полем, чтобы окно не знало ключей
     * метаданных (Д2) — и по нему строит вложенное дерево. */
    ASSERT_EQ(w.child_session, std::string("ses_000000000042"));
    ASSERT_TRUE(w.child_saved);
}

TEST(a_background_task_shows_no_step_count_instead_of_zero) {
    /* Фоновая постановка возвращается сразу: работы ещё не было, и «шагов 0»
     * читалось бы как «субагент отработал мгновенно». */
    json::JsonValue md = json::JsonValue::object();
    md.set("agent", std::string("wp_explore"));
    md.set("depth", static_cast<long long>(1));
    md.set("task", std::string("долгая работа"));
    md.set("session_id", std::string("ses_000000000043"));
    md.set("background", true);
    const tool_display::CallView v = fixture_view("task", md, "принято в фоне");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.head.find("в фон") != std::string::npos);
    ASSERT_TRUE(w.head.find("шагов") == std::string::npos);
    /* Три состояния, а не два: субагент ещё работает — это не «не записано».
     * Читать `session_saved` из отсутствия поля означало бы сказать человеку
     * «дерева не будет» там, где оно ещё не выросло. */
    ASSERT_TRUE(w.child_pending);
    ASSERT_FALSE(w.child_saved);
    ASSERT_TRUE(w.note.find("не записана") == std::string::npos);
}

TEST(a_subagent_session_that_was_not_saved_says_so) {
    /* Файла нет — читать нечего, и молчаливая пустота читалась бы как «у
     * субагента не было ходов». */
    json::JsonValue md = json::JsonValue::object();
    md.set("agent", std::string("wp_explore"));
    md.set("steps", static_cast<long long>(1));
    md.set("session_id", std::string("ses_000000000044"));
    md.set("session_saved", false);
    const tool_display::CallView v = fixture_view("task", md, "ответ субагента");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_FALSE(w.child_saved);
    ASSERT_TRUE(w.note.find("не записана") != std::string::npos);
}

TEST(a_subagent_call_without_a_session_id_says_the_tree_cannot_be_built) {
    register_all_tools();
    json::JsonValue md = json::JsonValue::object();
    md.set("agent", std::string("wp_explore"));
    md.set("steps", static_cast<long long>(1));
    const tool_display::CallView v = fixture_view("task", md, "ответ субагента");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.child_session.empty());
    ASSERT_TRUE(w.note.find("вложенное дерево построить нечем") !=
                std::string::npos);
}

/* ======================================================================
 * Откат «текст вывода» и четыре состояния отсутствующего вывода
 * ====================================================================== */

TEST(a_tool_without_its_own_kind_shows_the_title_and_the_output) {
    register_all_tools();
    const fs::path project = make_tmp_tree("text1");
    put_file(project / "a.txt", "alpha\n");
    init_engine(project);
    const tool_display::CallView v = live_view("repo_map", json::JsonValue::object());
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(w.kind == tool_display::Kind::Text);
    /* Заголовок ставит САМ инструмент: он знает, что сделал, и подставить
     * вместо него имя инструмента значило бы выбросить единственное, что в
     * заголовке есть.
     *
     * Сверять приходится с титулом САМОГО ЖИВОГО вызова, а не с литералом:
     * у `repo_map` титул случайно совпадает с именем инструмента, и
     * сравнение с литералом «repo_map» оставалось бы зелёным на коде, который
     * подставляет имя инструмента вместо титула, — то есть проверяло бы
     * совпадение двух строк, а не источник заголовка. Найдено прогоном
     * мутаций; ниже отдельная проверка на титуле, отличном от имени. */
    ASSERT_EQ(v.title, tool_display::build(v).head);
    ASSERT_TRUE(!w.lines.empty());
}

TEST(a_title_that_differs_from_the_tool_name_is_the_one_shown) {
    /* Различающий случай того же свойства: заголовок НЕ равен имени
     * инструмента, и подставить имя вместо него уже нельзя. */
    const tool_display::CallView v = fixture_view(
        "repo_map", json::JsonValue::object(), "перечислено 12 файлов");
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_EQ(w.head, std::string("заголовок repo_map"));
    ASSERT_TRUE(w.head != std::string("repo_map"));
}

TEST(a_tool_without_a_title_says_so_instead_of_pretending) {
    const tool_display::CallView v = fixture_view(
        "web_fetch", json::JsonValue::object(), "тело ответа");
    tool_display::CallView no_title = v;
    no_title.title.clear();
    const tool_display::Widget w = tool_display::build(no_title);
    ASSERT_EQ(w.head, std::string("web_fetch"));
    ASSERT_TRUE(w.note.find("не оставил заголовка") != std::string::npos);
}

TEST(a_call_that_has_not_finished_says_it_has_not_finished) {
    const tool_display::CallView v = fixture_view(
        "bash", json::JsonValue::object(), std::string(), json::JsonValue(),
        ToolState::Running);
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(has_line(w.lines, "вывод ещё не пришёл"));
    ASSERT_TRUE(w.head.find("exit неизвестен") != std::string::npos);
}

TEST(a_refused_call_shows_the_refusal_and_not_an_empty_output) {
    const tool_display::CallView v = fixture_view(
        "write_file", json::JsonValue::object(), std::string(),
        json::JsonValue(), ToolState::Error);
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(has_line(w.lines, "отказ: отказ по правам"));
}

TEST(an_output_cleared_by_the_pruner_is_still_shown_and_says_so) {
    /* Прореживание (И7.9) вычеркивает вывод из КОНТЕКСТА модели, но в части
     * он остаётся: стирание текста означало бы, что перезагруженная сессия
     * потеряла данные, которых не было никогда. Виджет показывает вывод и
     * ГОВОРИТ, что он вычеркнут из контекста: без этих слов «вывод урезан»
     * читался бы как ошибка инструмента. */
    MessagePart p = part_with_result("prt_00000000005", "repo_map",
                                     "было содержимое", json::JsonValue::object(),
                                     json::JsonValue());
    p.clear_output();
    const tool_display::CallView v = tool_display::snapshot(
        p, limits::kMaxToolDisplayChars);
    ASSERT_TRUE(v.output_cleared);
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(has_line(w.lines, "было содержимое"));
    ASSERT_TRUE(w.note.find("вычеркнут из контекста") != std::string::npos);
}

TEST(a_cleared_call_with_no_output_says_it_was_cleared) {
    /* Второе состояние очистки: вывода не было и очищать было нечего, а метка
     * прореживания стоит. Молчаливое «инструмент не вернул вывода» смешало
     * бы два разных факта. */
    MessagePart p = part_with_result("prt_00000000006", "repo_map", "",
                                     json::JsonValue::object(), json::JsonValue());
    p.clear_output();
    const tool_display::CallView v = tool_display::snapshot(
        p, limits::kMaxToolDisplayChars);
    ASSERT_TRUE(v.output_cleared);
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(has_line(w.lines, "очищен прореживанием"));
    ASSERT_TRUE(!has_line(w.lines, "не вернул вывода"));
}

TEST(a_call_with_an_empty_output_says_it_returned_nothing) {
    const tool_display::CallView v = fixture_view(
        "git_status", json::JsonValue::object(), std::string());
    const tool_display::Widget w = tool_display::build(v);
    ASSERT_TRUE(has_line(w.lines, "не вернул вывода"));
}

/* ======================================================================
 * Сбор снимков: предел и порядок
 * ====================================================================== */

TEST(only_open_calls_are_snapshotted_and_in_the_order_they_happened) {
    std::vector<Message> history;
    history.push_back(assistant_message("msg_000000000001", {
        part_with_result("prt_000000000011", "bash", "первый",
                         json::JsonValue::object(), json::JsonValue()),
        part_with_result("prt_000000000012", "glob", "второй",
                         json::JsonValue::object(), json::JsonValue()),
        part_with_result("prt_000000000013", "list", "третий",
                         json::JsonValue::object(), json::JsonValue()),
    }));
    /* Раскрыты второй и третий: снимка у первого быть не должно, а порядок
     * снимков — порядок частей, то есть порядок, в котором вызовы шли. */
    const std::vector<std::string> open = {"prt_000000000013",
                                           "prt_000000000012"};
    const tool_display::Snapshots shots = tool_display::snapshot_open_calls(
        history, open, limits::kMaxToolDisplayOpen, limits::kMaxToolDisplayChars);
    ASSERT_EQ(shots.views.size(), (size_t)2);
    ASSERT_EQ(shots.omitted, (size_t)0);
    ASSERT_EQ(shots.views[0].call_id, std::string("prt_000000000012"));
    ASSERT_EQ(shots.views[1].call_id, std::string("prt_000000000013"));
}

TEST(the_cap_on_open_calls_is_applied_here_and_the_rest_is_counted) {
    /* Предел применяет СБОР, и пропущенные считает он же: второе место, где
     * применяется предел, дало бы рядом второе число, и разошлись бы они
     * тихо. Молчаливое усечение выглядело бы как «показано всё». */
    std::vector<MessagePart> parts;
    std::vector<std::string> open;
    for (int i = 0; i < 5; ++i) {
        const std::string id = "prt_0000000000" + std::to_string(20 + i);
        parts.push_back(part_with_result(id.c_str(), "bash", "вывод",
                                         json::JsonValue::object(),
                                         json::JsonValue()));
        open.push_back(id);
    }
    std::vector<Message> history;
    history.push_back(assistant_message("msg_000000000002", parts));
    const tool_display::Snapshots shots = tool_display::snapshot_open_calls(
        history, open, 2, limits::kMaxToolDisplayChars);
    ASSERT_EQ(shots.views.size(), (size_t)2);
    ASSERT_EQ(shots.omitted, (size_t)3);
    /* Всё, что показано, — ИЗ открытых: предел не может показать закрытое. */
    ASSERT_EQ(shots.views[0].call_id, std::string("prt_000000000020"));
    ASSERT_EQ(shots.views[1].call_id, std::string("prt_000000000021"));
}

TEST(nothing_open_means_nothing_snapshotted) {
    /* Пока человек ничего не раскрыл, снимков быть не должно: под локом не
     * читается то, чего никто не смотрит. */
    std::vector<Message> history;
    history.push_back(assistant_message("msg_000000000003", {
        part_with_result("prt_000000000031", "bash", "вывод",
                         json::JsonValue::object(), json::JsonValue()),
    }));
    const tool_display::Snapshots shots = tool_display::snapshot_open_calls(
        history, {}, limits::kMaxToolDisplayOpen, limits::kMaxToolDisplayChars);
    ASSERT_EQ(shots.views.size(), (size_t)0);
    ASSERT_EQ(shots.omitted, (size_t)0);
}
TEST(one_snapshot_per_call_id_however_often_the_history_repeats_it) {
    /* И11.5: повторный идентификатор не съедает слот предела и не попадает
     * в пропущенные. Окно рисует ПЕРВОЕ совпадение по идентификатору, то
     * есть второй снимок не показался бы никогда; считать его «пропущенным»
     * тоже нельзя — пропущено то, что ДОЛЖНО было показаться и не влезло.
     * Идентификаторы в истории повторяются по-настоящему: вызов закрывается
     * по нему, и пара частей с одним `call_0` встречается в тестах дерева
     * (tests/test_timeline.cpp). */
    std::vector<MessagePart> parts;
    std::vector<std::string> open;
    for (int i = 0; i < 9; ++i) {
        const std::string id = "prt_0000000001" + std::to_string(i);
        parts.push_back(part_with_result(id.c_str(), "bash", "вывод",
                                         json::JsonValue::object(),
                                         json::JsonValue()));
        open.push_back(id);
    }
    /* Три части с уже занятыми идентификаторами: если бы снимок брался на
     * каждую часть, предел 8 съели бы повторы и два нужных тела не доехали
     * бы, а пропущенных оказалось бы 4 вместо 1. */
    parts.push_back(part_with_result("prt_00000000010", "bash", "вывод",
                                     json::JsonValue::object(), json::JsonValue()));
    parts.push_back(part_with_result("prt_00000000011", "bash", "вывод",
                                     json::JsonValue::object(), json::JsonValue()));
    parts.push_back(part_with_result("prt_00000000012", "bash", "вывод",
                                     json::JsonValue::object(), json::JsonValue()));
    std::vector<Message> history;
    history.push_back(assistant_message("msg_000000000002", parts));
    const tool_display::Snapshots shots = tool_display::snapshot_open_calls(
        history, open, 8, limits::kMaxToolDisplayChars);
    ASSERT_EQ(shots.views.size(), (size_t)8);
    /* Девять различных вызовов при пределе 8 — ровно один не влез. */
    ASSERT_EQ(shots.omitted, (size_t)1);
    /* Без предела: девять тел, и ни одного лишнего. */
    const tool_display::Snapshots all = tool_display::snapshot_open_calls(
        history, open, 64, limits::kMaxToolDisplayChars);
    ASSERT_EQ(all.views.size(), (size_t)9);
    ASSERT_EQ(all.omitted, (size_t)0);
}

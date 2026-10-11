/*
 * test_cancel_panel.cpp — отмена работающего инструмента и индикатор
 * отменяемости (И11.10).
 *
 * Проверяются три вещи, и все три — про правду:
 *
 *  1. ОТМЕНА ДОХОДИТ ДО РАБОТАЮЩЕГО ИНСТРУМЕНТА. Не до «ожидания LLM»
 *     (это И11.14) и не до `bash` (это И6.7), а до обхода дерева и до
 *     команды, запущенной инструментом модуля: с отменённым токеном вызов
 *     возвращается, а не проходит до конца.
 *
 *  2. ПРЕРОВАННЫЙ РЕЗУЛЬТАТ ПОМЕЧЕН. Список, который выглядит полным, а
 *     неполон, хуже отказа: модель приняла бы «найдено 40 файлов» за весь
 *     проект. Проверяется и текст («оборван»), и признак truncated, и
 *     «не кэшируется» для repo_map.
 *
 *  3. ИНДИКАТОР ГОВОРИТ ПРАВДУ. Панель обещает мгновенную отмену ровно
 *     тем инструментам, которые её проверяют, и признаёт «дождётся конца
 *     вызова» там, где внутри прерываться нельзя.
 *
 * Отдельно — сверка перечня с живым реестром в обе стороны: имя из
 * перечня обязано существовать, а обработчик, который спрашивает токен,
 * обязан в перечне быть. Без второй половины перечень тихо устарел бы,
 * и панель обещала бы мгновенную отмену инструменту, который её не
 * проверяет.
 */

#include "core/cancel.h"

#include "core/base_tools.h"
#include "core/engine.h"
#include "core/git_tools.h"
#include "core/glob.h"
#include "core/shell.h"
#include "core/tool.h"
#include "core/tools_registry.h"
#include "modules/devops/devops_tools.h"
#include "modules/python/python_tools.h"
#include "modules/wordpress/wp_tools.h"
#include "test_framework.h"
#include "test_support.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

namespace fs = std::filesystem;
using namespace coder;

namespace {

fs::path make_tmp_tree(const std::string& tag) {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_cancel_" + tag + "_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    register_tmp_tree(tmp.string());
    return tmp;
}

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

/* Дерево, на котором обход обязан остановиться: много файлов в ОДНОМ
 * каталоге. Плоская структура выбрана не случайно: проверка отмены «раз
 * на каталог» на плоском дереве не сработала бы никогда — ровно тот
 * случай, на котором обход и должен останавливаться. */
void make_wide_tree(const fs::path& root, int files) {
    for (int i = 0; i < files; ++i) {
        write_file(root / ("f" + std::to_string(i) + ".txt"),
                   "строка " + std::to_string(i) + "\n");
    }
}

void init_tools(const fs::path& project) {
    /* Полный набор, как в test_tool_display.cpp: перечень отменяемых
     * сверяется с живым реестром, и половина инструментов (модули) была бы
     * в нём отсутствовать. */
    static bool registered = false;
    if (!registered) {
        registered = true;
        test_support::approve_all_permissions();
        register_base_tools();
        register_rag_tools();
        register_git_tools();
        wp::register_wp_tools();
        python::register_python_tools();
        devops::register_devops_tools();
    }
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
    }
}

json::JsonValue args_of(const std::vector<std::pair<std::string, json::JsonValue>>& kv) {
    json::JsonValue a = json::JsonValue::object();
    for (const auto& p : kv) a.set(p.first, p.second);
    return a;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

long long ms_since(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

/* Снимок панели для типового случая. */
cancel::Snapshot snap_of(const std::string& tool, bool cancelled_requested) {
    cancel::Snapshot s;
    s.phase = cancel::Phase::Working;
    s.state_name = "Выполнение";
    s.tool_name = tool;
    s.tool_elapsed_ms = 12000;
    s.cancel_requested = cancelled_requested;
    return s;
}

/* Исходники core/ и modules/ — для сверки перечня с обработчиками. */
fs::path plugin_root() {
    return fs::path(__FILE__).parent_path().parent_path();
}

std::string read_source(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

/* Код без комментариев: иначе упоминание `cancelled()` в шапке файла
 * сочлось бы за проверку отмены в инструменте. */
std::string code_only(const std::string& src) {
    std::string out;
    bool in_block = false;
    bool in_line = false;
    bool in_str = false;
    char quote = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char n = i + 1 < src.size() ? src[i + 1] : '\0';
        if (in_line) {
            if (c == '\n') { in_line = false; out += c; }
            continue;
        }
        if (in_block) {
            if (c == '*' && n == '/') { in_block = false; ++i; }
            continue;
        }
        if (in_str) {
            out += c;
            if (c == '\\') {
                if (n) { out += n; ++i; }
                continue;
            }
            if (c == quote) in_str = false;
            continue;
        }
        if (c == '/' && n == '/') { in_line = true; ++i; continue; }
        if (c == '/' && n == '*') { in_block = true; ++i; continue; }
        if (c == '"' || c == '\'') { in_str = true; quote = c; }
        out += c;
    }
    return out;
}

}  // namespace

/* --- 1. Механика: токен хода доходит до инструмента --- */

/* ToolContext::cancelled() — единственная форма проверки. Отсутствие
 * токена НЕ отмена: ход без токена (тест, служебный вызов) отменять
 * нечем, и инструмент обязан отработать до конца. */
TEST(cancelled_is_false_without_a_token_and_true_after_abort) {
    EngineState state;
    HostCallbacks cb;
    ToolContext no_token(state, cb);
    ASSERT_FALSE(no_token.cancelled());

    state.turn_abort = std::make_shared<AbortToken>();
    ToolContext with_token(state, cb);
    ASSERT_FALSE(with_token.cancelled());
    state.turn_abort->abort();
    ASSERT_TRUE(with_token.cancelled());
}

/* fileglob::find останавливается по признаку и ОБОЗНАЧАЕТ это: молчаливо
 * оборванный список неотличим от полного. */
TEST(glob_walk_stops_on_cancel_and_says_so) {
    const fs::path t = make_tmp_tree("globwalk");
    make_wide_tree(t, 400);

    fileglob::Options opt;
    int probes = 0;
    opt.should_stop = [&probes]() { return ++probes > 5; };

    fileglob::Result r = fileglob::find(t.string(), "**", opt);
    ASSERT_TRUE(r.cancelled);
    ASSERT_FALSE(r.walk_capped);
    /* Обход прервался, а не завершился: проверено мало записей. */
    ASSERT_TRUE(r.visited <= 10);
    /* Найденное до отмены остаётся результатом: модель должна увидеть, что
     * что-то нашлось, а не пустоту. */
    ASSERT_FALSE(r.entries.empty());
}

/* Без признака обход не прерывается — иначе перестал бы работать. */
TEST(glob_walk_without_probe_runs_to_the_end) {
    const fs::path t = make_tmp_tree("glownoprobe");
    make_wide_tree(t, 30);

    fileglob::Result r = fileglob::find(t.string(), "**", fileglob::Options());
    ASSERT_FALSE(r.cancelled);
    ASSERT_EQ(static_cast<long long>(r.entries.size()), 30LL);
}

/* Инструмент glob с ОТМЕНЁННЫМ токеном возвращается сразу и говорит, что
 * список неполон, и помечает вывод усечённым (truncated): иначе модель
 * примет 40 файлов за весь проект. */
TEST(glob_tool_stops_on_cancelled_turn) {
    const fs::path t = make_tmp_tree("globtool");
    make_wide_tree(t, 500);
    init_tools(t);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().turn_abort = std::make_shared<AbortToken>();
        engine_state().turn_abort->abort();
    }

    json::JsonValue a = args_of({{"pattern", "**/*.txt"}});
    const auto t0 = std::chrono::steady_clock::now();
    ToolOutput o = ToolsRegistry::instance().run_output("glob", a);
    const long long ms = ms_since(t0);

    ASSERT_TRUE(ms < 5000);
    ASSERT_TRUE(has(o.output, "прерван"));
    ASSERT_TRUE(has(o.output, "обрезан"));
    /* Признак «вывод неполон» обязателен: без него общий предел вывода
     * срежет хвост, а модель не узнает, что список неполон. */
    ASSERT_TRUE(o.truncated);
}

/* Тот же инструмент без отмены доходит до конца и НЕ говорит, что
 * обрезан: иначе отмена стала бы постоянным «обрезано». */
TEST(glob_tool_without_cancel_is_complete) {
    const fs::path t = make_tmp_tree("globfull");
    make_wide_tree(t, 20);
    init_tools(t);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().turn_abort = std::make_shared<AbortToken>();
    }

    json::JsonValue a = args_of({{"pattern", "**/*.txt"}});
    ToolOutput o = ToolsRegistry::instance().run_output("glob", a);
    ASSERT_TRUE(!has(o.output, "прерван"));
    ASSERT_FALSE(o.truncated);
}

/* list — второй потребитель fileglob: та же честность. */
TEST(list_tool_marks_a_cancelled_walk) {
    const fs::path t = make_tmp_tree("listtool");
    make_wide_tree(t, 400);
    init_tools(t);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().turn_abort = std::make_shared<AbortToken>();
        engine_state().turn_abort->abort();
    }

    json::JsonValue a = args_of({{"depth", json::JsonValue(1)}});
    ToolOutput o = ToolsRegistry::instance().run_output("list", a);
    ASSERT_TRUE(has(o.output, "прерван"));
    ASSERT_TRUE(o.truncated);
}

/* grep_search идёт по file_utils::walk_files: другой обход, тот же
 * признак. */
TEST(grep_tool_stops_on_cancelled_turn) {
    const fs::path t = make_tmp_tree("greptool");
    make_wide_tree(t, 300);
    init_tools(t);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().turn_abort = std::make_shared<AbortToken>();
        engine_state().turn_abort->abort();
    }

    json::JsonValue a = args_of({{"pattern", "строка"}});
    const auto t0 = std::chrono::steady_clock::now();
    ToolOutput o = ToolsRegistry::instance().run_output("grep_search", a);
    const long long ms = ms_since(t0);

    ASSERT_TRUE(ms < 5000);
    ASSERT_TRUE(has(o.output, "прерван"));
}

/* repo_map кэширует результат на задачу. Оборванный обход кэшировать
 * НЕЛЬЗЯ: следующий вызов получил бы «[из кэша задачи]» — то есть
 * полный список там, где на самом деле половина. */
TEST(cancelled_repo_map_is_not_cached) {
    const fs::path t = make_tmp_tree("repomap");
    make_wide_tree(t, 400);
    init_tools(t);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().repo_map_cache_root.clear();
        engine_state().repo_map_cache_result.clear();
        engine_state().turn_abort = std::make_shared<AbortToken>();
        engine_state().turn_abort->abort();
    }
    ToolOutput first = ToolsRegistry::instance().run_output("repo_map",
        args_of({{"root", json::JsonValue(t.string())}}));
    ASSERT_TRUE(has(first.output, "прерван"));

    /* Второй вызов обязан пересчитать, а не отдать кэш. */
    ToolOutput second = ToolsRegistry::instance().run_output("repo_map",
        args_of({{"root", json::JsonValue(t.string())}}));
    ASSERT_TRUE(!has(second.output, "из кэша задачи"));
}

/* Команда инструмента останавливается тем же токеном — путь тот же, что у
 * bash, иначе «стоп» на `docker build` ждал бы полной сборки. */
TEST(run_capture_cancel_stops_a_running_command) {
    shell::RunStats stats;
    AbortToken token;
    std::thread canceller([&token] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        token.abort();
    });

    const auto t0 = std::chrono::steady_clock::now();
    shell::run_capture_cancel("sleep 30", 60,
                              [&token]() { return token.aborted(); }, stats);
    const long long ms = ms_since(t0);
    canceller.join();

    ASSERT_TRUE(stats.cancelled);
    ASSERT_TRUE(ms < 5000);
}

/* Без признака отмены команда живёт до конца: «отменяемый запуск» не
 * значит «не запускается вовсе». */
TEST(run_capture_cancel_without_probe_runs_to_completion) {
    shell::RunStats stats;
    const std::string out =
        shell::run_capture_cancel("echo отмена-не-запрошена", 30, {}, stats);
    ASSERT_FALSE(stats.cancelled);
    ASSERT_TRUE(has(out, "отмена-не-запрошена"));
}

/* --- 2. Индикатор отменяемости --- */

/* Мгновенная отмена обещается ровно тем инструментам, которые проверяют
 * токен. */
TEST(panel_promises_immediate_cancel_for_interruptible_tools) {
    for (const std::string& name : cancel::interruptible_tools()) {
        ASSERT_TRUE(cancel::interrupt_of(name) == cancel::Interrupt::Now);
        const cancel::Panel p = cancel::build(snap_of(name, false));
        ASSERT_TRUE(p.visible);
        ASSERT_TRUE(p.stop_now);
        ASSERT_TRUE(p.stopping == false);
        ASSERT_TRUE(has(p.stop_line, "прямо сейчас"));
        ASSERT_TRUE(has(p.head_line, name));
    }
}

/* Инструмент, который внутри не прерывается, обещает конец вызова, а не
 * мгновенную отмену: обещание «остановит сейчас» там было бы ложью,
 * потому что файл дописывается до конца. */
TEST(panel_says_a_plain_tool_has_to_finish) {
    const cancel::Panel p = cancel::build(snap_of("write_file", false));
    ASSERT_TRUE(p.visible);
    ASSERT_FALSE(p.stop_now);
    ASSERT_TRUE(has(p.stop_line, "дождётся конца вызова"));
    ASSERT_TRUE(has(p.stop_line, "write_file"));
}

/* Инструмента нет — ждём модель: отмена рвёт запрос. */
TEST(panel_describes_a_wait_for_the_model) {
    cancel::Snapshot s;
    s.phase = cancel::Phase::Working;
    s.state_name = "Выполнение";
    const cancel::Panel p = cancel::build(s);
    ASSERT_TRUE(p.visible);
    ASSERT_TRUE(p.stop_now);
    ASSERT_TRUE(has(p.stop_line, "оборвёт ожидание ответа модели"));
    ASSERT_TRUE(has(p.head_line, "Думает"));
}

/* Вопрос пользователя: отмена доходит до него отдельным механизмом
 * (cancel_all), и панель говорит об этом, а не молчит. */
TEST(panel_describes_a_pending_permission) {
    cancel::Snapshot s;
    s.phase = cancel::Phase::Waiting;
    s.waiting_permission = true;
    s.tool_name = "read_file";
    const cancel::Panel p = cancel::build(s);
    ASSERT_TRUE(p.visible);
    ASSERT_TRUE(has(p.stop_line, "отклонит ожидающий вопрос"));
    ASSERT_TRUE(has(p.head_line, "Ждёт вашего решения"));
}

/* Уже запрошенная отмена важнее описания текущего вызова: кнопку к этому
 * моменту уже нажали, и обещание сбило бы с толку. */
TEST(panel_reports_a_cancel_already_requested) {
    const cancel::Panel p = cancel::build(snap_of("bash", true));
    ASSERT_TRUE(p.visible);
    ASSERT_TRUE(p.stopping);
    ASSERT_FALSE(p.stop_now);
    ASSERT_TRUE(has(p.stop_line, "уже нажат"));
}

/* Простоя нет, когда отменять нечего: пустой блок с кнопкой выглядел бы
 * как поломка индикатора. */
TEST(panel_is_hidden_when_the_agent_is_idle) {
    cancel::Snapshot s;
    s.phase = cancel::Phase::Idle;
    s.tool_name = "bash";
    const cancel::Panel p = cancel::build(s);
    ASSERT_FALSE(p.visible);
}

/* Неизвестное имя — не отменяемо. Ответ по умолчанию обязан быть
 * «дождётся», а не «остановится сейчас». */
TEST(unknown_tool_is_not_promised_an_immediate_cancel) {
    ASSERT_TRUE(cancel::interrupt_of("такого_инструмента_нет")
                == cancel::Interrupt::AfterCall);
    const cancel::Panel p = cancel::build(snap_of("такого_инструмента_нет", false));
    ASSERT_FALSE(p.stop_now);
}

/* Законченный ход — это не Idle, но отменять в нём нечего. Без отдельной
 * фазы кнопка «Стоп» оставалась бы на экране после каждой задачи и
 * обещала бы оборвать ожидание модели, которого нет: ход уже закрыт. */
TEST(a_finished_turn_has_nothing_to_cancel) {
    ASSERT_TRUE(cancel::phase_of(AgentState::Done) == cancel::Phase::Finished);
    ASSERT_TRUE(cancel::phase_of(AgentState::Error) == cancel::Phase::Finished);
    ASSERT_TRUE(cancel::phase_of(AgentState::Aborted) == cancel::Phase::Finished);
    ASSERT_TRUE(cancel::phase_of(AgentState::Planning) == cancel::Phase::Working);
    ASSERT_TRUE(cancel::phase_of(AgentState::Executing) == cancel::Phase::Working);
    ASSERT_TRUE(cancel::phase_of(AgentState::WaitingPermission)
                == cancel::Phase::Waiting);
    ASSERT_TRUE(cancel::phase_of(AgentState::Idle) == cancel::Phase::Idle);

    cancel::Snapshot s;
    s.phase = cancel::phase_of(AgentState::Done);
    s.state_name = "Готово";
    const cancel::Panel p = cancel::build(s);
    ASSERT_FALSE(p.visible);
}

/* Фазы покрывают ВСЕ состояния FSM: новое состояние обязано попасть в
 * один из четырёх ответов, иначе оно молча стало бы «Idle» — то есть
 * кнопка исчезла бы там, где отменять есть что. */
TEST(every_agent_state_maps_to_a_phase) {
    static const AgentState kAll[] = {
        AgentState::Idle, AgentState::Planning, AgentState::Executing,
        AgentState::WaitingPermission, AgentState::Done, AgentState::Aborted,
        AgentState::Error};
    for (AgentState s : kAll) {
        const cancel::Phase p = cancel::phase_of(s);
        ASSERT_TRUE(p == cancel::Phase::Idle || p == cancel::Phase::Working ||
                    p == cancel::Phase::Waiting || p == cancel::Phase::Finished);
    }
    /* Считаем: у FSM ровно 7 состояний — если появится восьмое, таблица
     * проверки должна быть дополнена, а не молча разойтись. */
    ASSERT_EQ(static_cast<int>(sizeof(kAll) / sizeof(kAll[0])), 7);
}

/* --- 3. Перечень отменяемых: обе стороны --- */

/* Каждое имя перечня есть в живом реестре: иначе панель обещала бы
 * отмену инструменту, которого не существует, а настоящий инструмент
 * остался бы необъяснённым. */
TEST(interruptible_list_matches_the_live_registry) {
    /* Реестр поднимается целиком: сверка с половиной инструментов была бы
     * сверкой с тем, чего нет. */
    init_tools(fs::temp_directory_path());
    for (const std::string& name : cancel::interruptible_tools()) {
        if (!ToolsRegistry::instance().has(name)) {
            std::cerr << "  в перечне отменяемых есть «" << name
                      << "», а такого инструмента нет" << std::endl;
            ASSERT_TRUE(false);
        }
    }
    ASSERT_TRUE(cancel::interruptible_tools().size() >= 10);
}

/* Обратная сторона: обработчик, который спрашивает токен хода, обязан быть
 * в перечне — иначе панель скажет «дождётся конца вызова» про инструмент,
 * который на самом деле останавливается. Сканирование исходников, а не
 * флаг в ToolDef: флаг стоял бы рядом с проверкой, и забытый был бы
 * незаметен. */
TEST(every_tool_that_checks_cancel_is_in_the_list) {
    const fs::path root = plugin_root();
    size_t checked = 0;
    for (const char* dir : {"core", "modules"}) {
        const fs::path d = root / dir;
        if (!fs::exists(d)) continue;
        for (const auto& e : fs::recursive_directory_iterator(d)) {
            if (!e.is_regular_file() || e.path().extension() != ".cpp") continue;
            const std::string src = read_source(e.path());
            if (src.empty()) return;   /* плагин собран отдельно от репозитория */
            /* Проверка отмены ищется по КОДУ без комментариев: иначе
             * упоминание `cancelled()` в шапке сочлось бы за проверку
             * отмены в инструменте. */
            const std::string code = code_only(src);
            const std::string marker = "def.name = \"";
            size_t at = 0;
            while ((at = code.find(marker, at)) != std::string::npos) {
                const size_t name_begin = at + marker.size();
                const size_t name_end = code.find('"', name_begin);
                if (name_end == std::string::npos) break;
                const std::string tool = code.substr(name_begin, name_end - name_begin);
                /* Сегмент инструмента: от его имени до следующего `def.name`
                 * в этом же файле. */
                const size_t seg_end = code.find(marker, name_end);
                const std::string segment =
                    code.substr(name_begin, seg_end - name_begin);
                at = name_end + 1;
                ++checked;
                if (segment.find("cancelled()") == std::string::npos) continue;
                const std::vector<std::string>& list = cancel::interruptible_tools();
                const bool listed =
                    std::find(list.begin(), list.end(), tool) != list.end();
                if (!listed) {
                    std::cerr << "  инструмент " << tool
                              << " спрашивает токен отмены, но его нет в"
                                 " перечне отменяемых (core/cancel.h)" << std::endl;
                    ASSERT_TRUE(false);
                }
            }
        }
    }
    ASSERT_TRUE(checked > 30);
}
/*
 * test_tool_repair.cpp — И12.3: починка вызова инструмента.
 *
 * ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ И ПОЧЕМУ ИМЕННО ТАК
 * ------------------------------------------
 * Починка состоит из двух шагов, и проверять только первый — значит
 * оставить незамеченным ровно то, ради чего её затевали: модель, назвав
 * инструмент с опечаткой, должна получать РЕЗУЛЬТАТ ВЫЗОВА, а не отказ.
 *
 * ПОЧЕМУ ПРОВЕРЯЕТСЯ НЕ ТОЛЬКО ЧИСТАЯ ФУНКЦИЯ. repair_tool_call —
 * чистая функция, и проверка на ней была бы зелёной при коде, который
 * никто не зовёт, — ровно класс мёртвого executeShortcut (журнал §4,
 * И11.11). Поэтому каждое утверждение о маршрутизации продублировано
 * вызовом ToolRunner::run: он и есть единственная точка исполнения.
 *
 * ПОЧЕМУ ОТДЕЛЬНО ПРОВЕРЯЕТСЯ, ЧТО ПОЧИНКА НЕ СТАЛА ОБХОДОМ. Режим и
 * разрешения enforcement'ятся в ToolRunner::run (правило 7). Починка
 * стоит ДО этого места, и ошибка в её порядке дала бы инструменту
 * возможность выполниться в обход режима — то есть дыру, а не
 * улучшение. На это два встречных теста: несуществующее имя проходит
 * режим (инструмента нет, спрашивать не о чем), а ПОЧИНЕННОЕ имя — нет.
 *
 * ПОЧЕМУ ОТДЕЛЬНО ПРОВЕРЯЕТСЯ СКРЫТОСТЬ `invalid`. Скрытый инструмент,
 * который однажды попадёт в реестр, станет виден модели: либо в каталоге
 * промпта, либо в нативном вызове (И12.2), либо в перечислении доступных
 * внутри собственного сообщения об ошибке. Все три читают ОДИН реестр,
 * поэтому проверка «его нет в реестре» покрывает их все разом — и это
 * дешевле, чем три проверки, каждая из которых забывается при следующем
 * добавлении инструмента.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/agent_components.h"
#include "../core/base_tools.h"
#include "../core/engine.h"
#include "../core/json.h"
#include "../core/tool.h"
#include "../core/tool_repair.h"
#include "../core/tools_registry.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

using namespace coder;

namespace fs = std::filesystem;

namespace {

void register_all_tools() {
    test_support::approve_all_permissions();
    register_base_tools();
}

/* Engine — синглтон, состояние протекает между тестами (правило 5). */
struct ModeGuard {
    int prev_mode;
    bool prev_plan;
    explicit ModeGuard(int mode, bool plan) {
        prev_mode = engine_state().mode;
        prev_plan = engine_state().plan_mode;
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().mode = mode;
        engine_state().plan_mode = plan;
    }
    ~ModeGuard() {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().mode = prev_mode;
        engine_state().plan_mode = prev_plan;
    }
};

/* Событие вместе с видом: проверять только текст нельзя — починка
 * обязана слать AgentEvent::Tool, и ошибка «Error с похожим текстом»
 * от проверки текста неотличима. */
struct Seen {
    AgentEvent::Kind kind;
    std::string text;
};

struct Sink {
    std::vector<Seen> events;
    ToolRunner runner() {
        return ToolRunner(engine_state(), Engine::instance().callbacks(),
                          [this](AgentEvent::Kind k, const std::string& text) {
                              events.push_back({k, text});
                          });
    }
    size_t count(AgentEvent::Kind k) const {
        size_t n = 0;
        for (const Seen& e : events) n += (e.kind == k) ? 1 : 0;
        return n;
    }
};

json::JsonValue args_with(const char* key, const std::string& value) {
    json::JsonValue args = json::JsonValue::object();
    args.set("tool", "read_file");
    args.set(key, value);
    return args;
}

} // anonymous namespace

/* ======================================================================
 * Шаг 1 — имя в нижнем регистре
 * ====================================================================== */

TEST(repair_lowercases_a_name_the_registry_still_has) {
    register_all_tools();
    const ToolRepair r = repair_tool_call("READ_FILE");
    ASSERT_EQ(r.tool, std::string("read_file"));
    ASSERT_TRUE(r.repaired_case);
    ASSERT_FALSE(r.routed_to_invalid);
    ASSERT_EQ(r.requested, std::string("READ_FILE"));
    /* Описание пришло из того же реестра, что и починка: иначе починка
     * знала бы об инструментах больше, чем об этом знает исполнение. */
    ASSERT_TRUE(r.def != nullptr);
    ASSERT_EQ(r.def->name, std::string("read_file"));
}

TEST(exact_match_is_not_a_case_repair) {
    register_all_tools();
    /* Имя, уже написанное правильно, починкой НЕ считается: иначе
     * «исправленное имя» означало бы «любое имя», и проверка выше
     * проходила бы в том числе для кода, который ломает нормальные
     * вызовы. */
    const ToolRepair r = repair_tool_call("read_file");
    ASSERT_EQ(r.tool, std::string("read_file"));
    ASSERT_FALSE(r.repaired_case);
    ASSERT_FALSE(r.routed_to_invalid);
    ASSERT_TRUE(r.def != nullptr);
}

TEST(case_repair_needs_the_lowercased_name_to_exist) {
    register_all_tools();
    /* Регистр чинится ТОЛЬКО если такое имя есть. Приведение «любого»
     * имени к нижнему регистру без проверки дало бы для «read_files»
     * несуществующий инструмент вместо честного ответа модели. */
    const ToolRepair r = repair_tool_call("READ_FILES");
    ASSERT_TRUE(r.routed_to_invalid);
    ASSERT_FALSE(r.repaired_case);
    ASSERT_EQ(r.tool, std::string(kInvalidTool));
    ASSERT_TRUE(r.def == nullptr);
}

/* ======================================================================
 * Шаг 2 — маршрутизация в скрытый `invalid`
 * ====================================================================== */

TEST(unknown_name_routes_to_the_hidden_invalid_tool) {
    register_all_tools();
    const ToolRepair r = repair_tool_call("frobnicate");
    ASSERT_TRUE(r.routed_to_invalid);
    ASSERT_EQ(r.tool, std::string("invalid"));
    ASSERT_TRUE(r.def == nullptr);
    /* Аргументы `invalid` — {tool, error}, и именно то имя, которое
     * прислала модель: «Read» должно читаться как опечатка, а не как
     * имя уже выполненного инструмента. */
    ASSERT_EQ(r.invalid_args.get_string("tool", ""), std::string("frobnicate"));
    ASSERT_TRUE(!r.invalid_args.get_string("error", "").empty());
}

TEST(invalid_tool_is_not_in_the_registry) {
    register_all_tools();
    ToolsRegistry& reg = ToolsRegistry::instance();
    /* Скрыт = его нет в единственном источнике истины. Отдельный флаг
     * «скрытый» оставил бы второе место, где про него можно забыть. */
    ASSERT_TRUE(reg.find(kInvalidTool) == nullptr);
    ASSERT_FALSE(reg.has(kInvalidTool));
    /* Три потребителя реестра, и все три читают его же: каталог
     * промпта, нативный вызов (И12.2) и перечисление доступных внутри
     * сообщения об ошибке. Проверка «нет в реестре» покрывает их все;
     * здесь — по одному на каждый, чтобы падение называло потребителя. */
    ASSERT_TRUE(reg.build_tool_catalogue().find("invalid") == std::string::npos);
    ASSERT_TRUE(reg.build_tool_specs_json().find("\"invalid\"")
                == std::string::npos);
    ASSERT_TRUE(reg.join_tools().find(kInvalidTool) == std::string::npos);
}

TEST(invalid_arguments_and_its_result_carry_one_and_the_same_text) {
    register_all_tools();
    /* Одно сообщение в двух местах (D12): иначе модель в аргументах
     * читала бы одно, а в результате вызова — другое. */
    const ToolRepair r = repair_tool_call("frobnicate");
    ASSERT_EQ(r.invalid_args.get_string("error", ""),
              invalid_tool_output(r).output);
}

/* ======================================================================
 * Точка исполнения: ToolRunner::run
 * ====================================================================== */

TEST(tool_runner_returns_a_result_not_a_refusal_for_an_unknown_name) {
    register_all_tools();
    Sink sink;
    ToolRunner runner = sink.runner();
    const ToolOutcome outcome = runner.run("frobnicate", json::JsonValue::object());
    /* ГЛАВНОЕ УТВЕРЖДЕНИЕ ЗАДАЧИ. ok=true — вызов получил результат.
     * Прежде было ToolError, то есть протокольная ошибка чужого слоя:
     * у вызова не было результата, и модель вместо правки имени
     * начинала рассуждать заново. */
    ASSERT_TRUE(outcome.ok);
    ASSERT_TRUE(outcome.error.empty());
    /* Результат называет инструмент, которого нет, и говорит, какие
     * есть: иначе модель знала бы, что вызов не состоялся, но не
     * знала бы, что написать вместо него. */
    ASSERT_TRUE(outcome.output.output.find("frobnicate")
                != std::string::npos);
    ASSERT_TRUE(outcome.output.output.find("не существует")
                != std::string::npos);
    ASSERT_TRUE(outcome.output.output.find("read_file")
                != std::string::npos);
    /* Список доступных берётся из реестра, поэтому починка не может
     * предложить вызвать то, чего в этой сборке нет. */
    ASSERT_TRUE(outcome.output.output.find(
                    ToolsRegistry::instance().join_tools())
                != std::string::npos);
}

TEST(unknown_name_is_reported_as_a_tool_call_not_as_an_error) {
    register_all_tools();
    Sink sink;
    ToolRunner runner = sink.runner();
    runner.run("frobnicate", json::JsonValue::object());
    /* В ленте починка — это Tool, а не Error: ошибки не было, отработала
     * маршрутизация. Иначе UI показывал бы агенту красную ошибку там,
     * где он успешно получил подсказку. */
    ASSERT_EQ(sink.count(AgentEvent::Tool), (size_t)1);
    ASSERT_EQ(sink.count(AgentEvent::Error), (size_t)0);
    ASSERT_TRUE(sink.events.front().text.find("frobnicate")
                != std::string::npos);
}

TEST(tool_runner_runs_the_case_repaired_tool) {
    register_all_tools();
    ModeGuard mode(0, false);
    const fs::path dir = fs::temp_directory_path() /
                         ("wp_coder_repair_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    /* Регистрация каталога — основной механизм уборки (test_framework.h):
     * при падении проверки на середине тело до remove_all не дойдёт, а
     * каталог останется. Самоочистка в теле теста сработала бы только
     * на зелёном прогоне. */
    register_tmp_tree(dir.string());
    {
        std::ofstream f(dir / "a.txt");
        f << "живая проверка починки";
    }
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = dir.string();
        engine_state().recent_calls.clear();
    }
    Sink sink;
    ToolRunner runner = sink.runner();
    /* Имя с опечаткой в регистре исполняется НАСТОЯЩИМ инструментом:
     * проверка на содержимом файла, а не на «не отказал» — отказ и
     * пустой вывод отличаются, а «выполнилось» без результата — это
     * третий случай, который виден только по содержимому. */
    const ToolOutcome outcome = runner.run("READ_FILE", args_with("path", "a.txt"));
    ASSERT_TRUE(outcome.ok);
    ASSERT_TRUE(outcome.output.output.find("живая проверка починки")
                != std::string::npos);
    fs::remove_all(dir, ec);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir.clear();
        engine_state().recent_calls.clear();
    }
}

/* ======================================================================
 * Починка не стала обходом enforcement
 * ====================================================================== */

TEST(unknown_name_is_repaired_in_research_mode_too) {
    register_all_tools();
    /* В research-режиме инструмент без классификации запрещён
     * fail-closed. Если бы `invalid` шёл общим путём, опечатка в этом
     * режиме давала бы отказ ПО РЕЖИМУ вместо подсказки — починка
     * работала бы не везде, и худший случай совпал бы с худшим. */
    ModeGuard mode(1, false);
    Sink sink;
    ToolRunner runner = sink.runner();
    const ToolOutcome outcome = runner.run("frobnicate", json::JsonValue::object());
    ASSERT_TRUE(outcome.ok);
    ASSERT_TRUE(outcome.output.output.find("не существует")
                != std::string::npos);
}

TEST(case_repaired_name_still_goes_through_enforcement) {
    register_all_tools();
    /* Обратная сторона того же места: починенное имя — это имя
     * НАСТОЯЩЕГО инструмента, поэтому режим и разрешения применяются к
     * нему как к любому. Если бы починка обходила enforcement, запись
     * в research-режиме прошла бы — дыра вместо улучшения. */
    ModeGuard mode(1, false);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().recent_calls.clear();
    }
    Sink sink;
    ToolRunner runner = sink.runner();
    json::JsonValue args = json::JsonValue::object();
    args.set("path", "x.txt");
    args.set("content", "не должно быть записано");
    const ToolOutcome outcome = runner.run("WRITE_FILE", args);
    ASSERT_FALSE(outcome.ok);
    ASSERT_TRUE(outcome.error.find("запрещено режимом") != std::string::npos);
    /* Отпечаток зацикливания не попал в историю: проверка режима идёт
     * до него, иначе три одинаковых отказа сочлись бы зацикливанием
     * (то же, что у refusal по разрешению). */
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    ASSERT_TRUE(engine_state().recent_calls.empty());
}

/* ======================================================================
 * Края
 * ====================================================================== */

TEST(empty_name_routes_to_invalid_and_does_not_crash) {
    register_all_tools();
    /* Пустое имя — это тоже «инструмента нет», и починка обязана дать
     * модели тот же ответ, а не уйти в разыменование пустого описания. */
    const ToolRepair r = repair_tool_call("");
    ASSERT_TRUE(r.routed_to_invalid);
    ASSERT_TRUE(r.def == nullptr);
    Sink sink;
    ToolRunner runner = sink.runner();
    ASSERT_TRUE(runner.run("", json::JsonValue::object()).ok);
}

TEST(non_ascii_name_is_repaired_only_if_such_a_tool_exists) {
    register_all_tools();
    /* Приведение регистра здесь — ТОЛЬКО ASCII. Нижний регистр кириллицы
     * зависит от локали, а результат починки обязан быть одинаковым на
     * любой машине; кроме того, std::tolower на не-ASCII байтах — это
     * UB. Здесь имя не существует, поэтому уходит в `invalid`, но
     * главное — не падает и не «чинит» его во что-то другое. */
    const ToolRepair r = repair_tool_call("ЧТЕНИЕ_ФАЙЛА");
    ASSERT_TRUE(r.routed_to_invalid);
    ASSERT_EQ(r.requested, std::string("ЧТЕНИЕ_ФАЙЛА"));
    ASSERT_TRUE(r.def == nullptr);
}
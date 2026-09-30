// subagent.cpp — инструмент `task` (И8.7, порт tool/task.ts).
//
// Обоснование решений (кто выбирает агента, как его промпт попадает в
// системный, по каким правилам живёт ребёнок, что остаётся за 8.8–8.14) —
// в заголовке core/subagent.h. Здесь только код.

#include "subagent.h"

#include "agent_components.h"
#include "agent_registry.h"
#include "engine.h"
#include "limits.h"
#include "tool.h"
#include "tools_registry.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace coder {

namespace {

/* Отказ с объяснением, что делать дальше. Модель получает этот текст в
 * результате вызова, и «неверный агент» без подсказки стоило бы ей
 * следующего шага на ту же ошибку: имена агентов не перечислить в
 * статическом описании инструмента (список живой — 8.13), а здесь они
 * перечисляются в том месте, где модель уже ошибается. */
std::string refuse(const std::string& reason) {
    return "[ошибка] task: " + reason +
           "\nНЕ ПОВТОРЯЙ вызов с теми же аргументами.";
}

std::string quoted(const std::string& s) { return "«" + s + "»"; }

} // namespace

std::string subagent_names_line() {
    const std::vector<std::string> names = AgentRegistry::instance().subagent_names();
    if (names.empty()) {
        return "(ни одного: агенты не загружены — проверь каталог"
               " .wpcode/agent проекта)";
    }
    std::string out;
    for (const std::string& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

void register_task_tool() {
    ToolDef def;
    def.name = "task";
    def.description =
        "Передать отдельную задачу субагенту и получить его итог текстом."
        " Зови, когда задача самостоятельна и нужен её РЕЗУЛЬТАТ, а не"
        " ход работы: обзор проекта, разбор вопроса, правка одного файла."
        " Субагент работает со своим отдельным контекстом, в твой контекст"
        " попадёт только его итоговый текст — поэтому пиши в prompt всё,"
        " что нужно знать: разговор с пользователем ему не пересказывают."
        " description — короткое имя задачи в 2-3 слова (по нему человек"
        " видит, что запущено), prompt — полное задание для субагента,"
        " включая то, в каком виде вернуть ответ, subagent_type — имя"
        " агента из списка субагентов. Уточнить задание у субагента нельзя:"
        " он не разговаривает с пользователем, и переспросить он не сможет.";
    /* И8.7: флаги. TF_SLOW — единственный: вызов делает несколько
     * запросов к модели и может идти долго. TF_READ_ONLY здесь НЕ
     * ставится, и это не осторожность ради осторожности: субагент
     * получает инструменты, и что именно он ими сделает, статически не
     * известно. В режиме Research, где обещано «ничего не меняется»,
     * делегирование поэтому запрещено — а «ничего не меняет» у
     * конкретного субагента обеспечивается его правилами (wp_explore,
     * И8.6) и теми же флагами на его инструментах, которые
     * проверяются в ToolRunner::run.
     *
     * TF_WRITES_FILES тоже нет: файлы субагента пишут ЕГО инструменты,
     * и каждое такое попадание проверяется отдельно. Пометить здесь
     * WRITES_FILES значило бы запретить инструмент в план-режиме, то
     * есть запретить делегирование там, где делегирование безопасно
     * (правки ребёнка всё равно предложат пользователю). */
    def.flags = TF_SLOW;
    /* Свой ключ, а не `bash`/`write`: «всегда разрешить делегирование» и
     * «всегда разрешить запуск команд» — разные решения пользователя, а
     * один ключ означал бы, что кнопка «всегда» на вопросе о субагенте
     * тихо разрешает и запуск кода. */
    def.permission_key = "task";

    SchemaBuilder b;
    b.str("description", "короткое имя задачи в 2-3 слова: что именно "
                         "поручено субагенту (его увидит пользователь)");
    b.str("prompt", "полное задание для субагента: что сделать, где "
                    "смотреть, в каком виде вернуть ответ. Субагент не "
                    "видит разговора, поэтому всё нужное здесь");
    b.str("subagent_type", "имя субагента (как у субагента из списка "
                           "доступных)");
    b.str("task_id", "НЕ ПОДДЕРЖИВАЕТСЯ (задача 8.9): продолжить ранее "
                     "запущенную задачу нельзя, поле оставь пустым");
    b.boolean("background", "НЕ ПОДДЕРЖИВАЕТСЯ (задача 8.14): субагент "
                            "выполняется до конца, ответ придёт в этом же "
                            "вызове; поле оставь false");
    b.required("description").required("prompt").required("subagent_type");
    def.parameters = b.build();

    def.handler = [](const json::JsonValue& args, ToolContext& ctx) -> ToolOutput {
        Engine& engine = Engine::instance();
        EngineState& state = ctx.state();

        ToolOutput out;
        out.title = "task";
        out.metadata = json::JsonValue::object();

        /* --- 1. Обязательные поля --- */
        const std::string description = args.get_string("description");
        const std::string prompt = args.get_string("prompt");
        const std::string raw_type = args.get_string("subagent_type");
        if (description.empty()) {
            out.title = "task (без описания)";
            out.output = refuse(
                "поле «description» пустое. Это короткое имя задачи (2-3"
                " слова), по нему пользователь видит, что запущено.");
            return out;
        }
        if (prompt.empty()) {
            out.output = refuse(
                "поле «prompt» пустое. Субагент не видит разговора, поэтому"
                " без него он не знает, что делать.");
            return out;
        }
        if (raw_type.empty()) {
            out.output = refuse(
                "поле «subagent_type» пустое — нужно имя субагента."
                " Доступные субагенты: " + subagent_names_line() + ".");
            return out;
        }

        /* --- 2. Поля, работа которых ещё не написана --- */
        /* Объявлены в схеме (подпись порта), но не работают. Отказ здесь
         * явный, потому что молчаливый игнор выглядел бы как успех: модель
         * решила бы, что продолжила задачу, и ждала бы результата, которого
         * не будет. */
        if (!args.get_string("task_id").empty()) {
            out.output = refuse(
                "поле «task_id» не поддерживается: продолжить ранее"
                " запущенную задачу пока нельзя. Поставь новую задачу без"
                " task_id.");
            return out;
        }
        if (args.get_bool("background")) {
            out.output = refuse(
                "поле «background» не поддерживается: субагент выполняется"
                " до конца, и его итог придёт в этом же вызове.");
            return out;
        }

        /* --- 3. Глубина вложенности (порог — 8.8) --- */
        int depth = 0;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            depth = state.scope.depth + 1;
        }
        if (depth > limits::kSubagentDepthLimit) {
            out.output = refuse(
                "Subagent depth limit reached (" +
                std::to_string(limits::kSubagentDepthLimit) +
                "). Глубже вкладывать субагентов нельзя: это кончилось бы"
                " рекурсивными запросами к модели. Сделай работу сам или"
                " вернись к вызывающему агенту.");
            return out;
        }

        /* --- 4. Агент по имени --- */
        std::string name;
        if (!normalize_agent_name(raw_type, &name)) {
            out.output = refuse("имя агента «" + raw_type +
                                "» пустое после нормализации. Доступные"
                                " субагенты: " + subagent_names_line() + ".");
            return out;
        }
        /* Список берётся ПЕРВЫМ, а определение — вторым, и оба под локом
         * реестра: «найден, но звать нельзя» и «не найден» — разные
         * ошибки, и модель, назвавшая существующее имя, должна знать это,
         * а не искать опечатку. Определение — shared_ptr, поэтому даже
         * если конфиг перечитают между вызовами, указатель остаётся
         * рабочим. */
        const std::vector<std::string> allowed =
            AgentRegistry::instance().subagent_names();
        const std::shared_ptr<const AgentDef> def =
            AgentRegistry::instance().find(name);
        const bool callable =
            std::find(allowed.begin(), allowed.end(), name) != allowed.end();
        if (!def || !callable) {
            out.output = refuse(
                def ? ("агент " + quoted(name) + " не субагент: его mode — " +
                       std::string(agent_mode_name(def->mode)) +
                       (def->hidden ? ", и он скрыт" : "") +
                       ". Звать его можно только как основного агента."
                       " Доступные субагенты: " + subagent_names_line() + ".")
                    : ("агент " + quoted(name) + " не найден."
                       " Доступные субагенты: " + subagent_names_line() + "."));
            return out;
        }

        /* --- 5. Правила и область выполнения --- */
        std::vector<AgentLoadDiag> diags;
        /* База — копия правил сессии под локом движка разрешений: Info
         * живёт дольше вызова, а правила меняются из UI-потока. */
        const std::shared_ptr<agent::Info> info =
            agent::Info::from_def(*def, engine.permissions().rules_snapshot(),
                                  &diags);
        for (const AgentLoadDiag& d : diags) {
            engine.push_event(AgentEvent::Status,
                              "task/" + name + ": " + d.message);
        }

        int max_steps = 0;
        RunScope scope;
        scope.agent = name;
        scope.info = info;
        scope.depth = depth;
        scope.loop_calls = std::make_shared<std::deque<std::string>>();
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            max_steps = state.max_steps;
        }
        if (info->steps() > 0) max_steps = info->steps();

        /* ScopedAgentScope живёт до конца обработчика, а не только на
         * время сборки промпта: область обязана быть подменена на ВЕСЬ
         * ход субагента, иначе enforcement его инструментов отвечал бы по
         * правилам сессии, а сам ход шёл бы с чужим агентом. В
         * деструкторе область возвращается и кэш промпта сбрасывается —
         * даже при раннем выходе, потому что Engine и ToolsRegistry —
         * синглтоны и оставшийся после ребёнка чужой промпт ушёл бы в
         * СЛЕДУЮЩИЙ ход родителя. */
        ScopedAgentScope guard(engine, std::move(scope));
        const std::string sys = engine.build_system_prompt();

        engine.push_event(AgentEvent::Status,
                          "task: субагент " + name + " — " + description);

        /* --- 6. Ход субагента --- */
        const SubagentResult r = run_subagent_turn(
            state, ctx.callbacks(),
            [&engine](AgentEvent::Kind k, const std::string& text) {
                engine.push_event(k, text);
            },
            sys, prompt, max_steps);

        out.title = "task " + description + " (" + name + ")";
        out.metadata.set("agent", name);
        out.metadata.set("depth", static_cast<long long>(depth));
        out.metadata.set("steps", static_cast<long long>(r.steps));
        out.metadata.set("task", description);

        if (!r.ok) {
            out.output = "[ошибка] task: субагент " + name + " не выполнил"
                         " задачу.\n" + r.error;
            return out;
        }
        out.output = r.text;
        return out;
    };

    ToolsRegistry::instance().register_def(std::move(def));
}

} // namespace coder

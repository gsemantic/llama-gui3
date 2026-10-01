// subagent.cpp — инструмент `task` (И8.7, порт tool/task.ts).
//
// Обоснование решений (кто выбирает агента, как его промпт попадает в
// системный, по каким правилам живёт ребёнок, что остаётся за 8.8–8.14) —
// в заголовке core/subagent.h. Здесь только код.

#include "subagent.h"

#include "agent_components.h"
#include "agent_registry.h"
#include "engine.h"
#include "id_prefix.h"
#include "limits.h"
#include "session_store.h"
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

/* Обёртка результата задачи (И8.11, порт task.ts:69).
 *
 * Идентификатор задачи едет ВМЕСТЕ с результатом, и это ровно то, чего
 * не хватало механизму продолжения (И8.9): без него модель не знала, что
 * задачу можно продолжить, потому что узнать идентификатор было негде.
 * Формат назван в kBaseSystemPrompt — по общему правилу файла (D2):
 * строка формата, о которой модель не знает, ею и не пользуется.
 *
 * `state` — «completed» или «error»: отдельный признак, а не слово в
 * тексте, потому что по нему решает вызывающий, а читать текст он не
 * обязан. ОТКАЗЫ (несуществующий агент, лимит глубины, негодный task_id)
 * обёрткой НЕ идут: там не было задачи, а был отказ до неё, и обёртка
 * с идентификатором выглядела бы как «задача выполнена». */
std::string task_wrapper(const std::string& session_id, bool ok,
                         const std::string& body) {
    return "<task id=\"" + session_id + "\" state=\"" +
           (ok ? "completed" : "error") + "\">\n<task_result>\n" + body +
           "\n</task_result>\n</task>";
}

/* --- И8.14: фоновая задача ---
 *
 * Текст ПРИЁМА — ответ вызова `task` с background: true. Он обязан
 * сказать три вещи, иначе модель сделает что-нибудь лишнее: что задача
 * принята (а не потеряна), что итог придёт сам и отдельным сообщением, и
 * что опрашивать прогресс нельзя. Третье — не пожелание, а устройство
 * механизма: продолжение фоновой задачи до её конца отклоняется (см.
 * is_queued_background), и без явного запрета модель тратила бы шаг на
 * заведомо отклонённый вызов. */
std::string background_accepted(const std::string& session_id,
                                 const std::string& agent,
                                 const std::string& description) {
    return "Задача принята в фоне: " + session_id + " (" + agent + ", «" +
           description + "»).\n"
           "Она начнёт выполняться после того, как закончится текущий ход, и её "
           "итог придёт ОТДЕЛЬНЫМ сообщением в этот же диалог — в том же виде, "
           "что и итог обычного вызова task.\n"
           "НЕ ОПРАШИВАЙ ПРОГРЕСС: пока итог не пришёл, не зови task с этим "
           "task_id, не ставь такую же задачу в фон повторно и не жди её "
           "результата в этом вызове — делай другую работу. Итог придёт сам.";
}

/* Текст ДОСТАВКИ — синтетическое сообщение, которым результаты ложатся в
 * сессию (И8.14). Заголовок назван словами, а не оставлен пустым: пустое
 * сообщение с обёртками читалось бы как обычная реплика пользователя, и
 * модель не поняла бы, откуда оно взялось. */
std::string background_results_header(size_t count) {
    return "[фоновые задачи завершены] Выполнено задач: " +
           std::to_string(count) +
           ". Это итоги фоновых задач, поставленных тобой раньше: работа "
           "сделана, её не нужно ждать и не нужно перезапускать. Разберись с "
           "ними и продолжи задачу.\n";
}

/* Жива ли ещё фоновую задачу — по ОЧЕРЕДИ, а не по файлу.
 *
 * Дочерняя сессия пишется в конце хода ребёнка, то есть файла у
 * поставленной задачи ещё нет, и проверка «файл есть» объявила бы такую
 * задачу несуществующей: модель получила бы «задача не найдена» на
 * заведомо существующей работе и решила бы, что постановка не сработала. */
bool is_queued_background(const EngineState& state, const std::string& task_id) {
    std::lock_guard<std::mutex> lk(state.mtx);
    for (const SubagentJob& job : state.background_tasks) {
        if (job.session_id == task_id) return true;
    }
    return false;
}

} // namespace

/* Итог выполнения задачи ребёнком: его собственный результат и то, что
 * знать вызывающему, — сошлась ли запись дочерней сессии. */
struct ChildTurn {
    SubagentResult result;
    bool session_saved = false;
};

/* Один ход ребёнка — ОБЩИЙ для обычного вызова и для фоновой задачи
 * (И8.14). Два места, выполняющие «сделай задачу ребёнком», разошлись бы
 * первым же изменением: область, промпт, предел шагов, запись дочерней
 * сессии и обёртка результата — здесь ровно по одному разу каждое.
 *
 * ScopedAgentScope живёт до конца функции, а не только на время сборки
 * промпта: область обязана быть подменена на ВЕСЬ ход ребёнка, иначе
 * enforcement его инструментов отвечал бы по правилам сессии, а сам ход
 * шёл бы с чужим агентом. В деструкторе область возвращается и кэш
 * промпта сбрасывается — даже при раннем выходе, потому что Engine и
 * ToolsRegistry — синглтоны и оставшийся после ребёнка чужой промпт ушёл
 * бы в СЛЕДУЮЩИЙ ход родителя.
 *
 * Дочерняя сессия пишется ЛЮБЫМ исходом хода, включая отказ и
 * прерывание: частично сделанная работа тоже стоит продолжения, а терять
 * её молча значило бы, что `task_id` бесполезен ровно тогда, когда он
 * нужен.
 *
 * Ошибка записи — не ошибка задачи: ход-то состоялся, поэтому в ответе
 * модели она выглядела бы ошибкой работы. Пользователь узнаёт о ней из
 * события, а продолжить задачу будет нечем. */
ChildTurn run_child_turn(Engine& engine, EngineState& state,
                         const SubagentJob& job) {
    ChildTurn out;

    RunScope scope;
    scope.agent = job.agent;
    scope.info = job.info;
    scope.depth = job.depth;
    scope.loop_calls = std::make_shared<std::deque<std::string>>();
    ScopedAgentScope guard(engine, std::move(scope));
    const std::string sys = engine.build_system_prompt();

    /* Продолжение получает историю из дочерней сессии (И8.9): prompt
     * становится следующей репликой того же разговора, а не началом
     * нового. */
    out.result = run_subagent_turn(
        state, engine.callbacks(),
        [&engine](AgentEvent::Kind k, const std::string& text) {
            engine.push_event(k, text);
        },
        sys, job.prompt, job.max_steps, job.seed);

    SessionFile child;
    child.session_id = job.session_id;
    child.parent_id = job.parent_id;
    child.title = job.title;
    child.messages = out.result.history;
    const std::string dir = engine.callbacks().path_data_dir
                                ? engine.callbacks().path_data_dir()
                                : std::string();
    const std::string path =
        SessionArchive::subagent_file_path(dir, job.session_id);
    std::string error;
    if (path.empty() || !SessionArchive::save(path, child, &error)) {
        out.session_saved = false;
        engine.push_event(
            AgentEvent::Status,
            "task/" + job.agent + ": не удалось сохранить задачу " +
                job.session_id + " — " +
                (error.empty() ? std::string("нет каталога данных")
                               : error));
    } else {
        out.session_saved = true;
        engine.push_event(AgentEvent::Status,
                          "task/" + job.agent + ": задача " + job.session_id +
                              (job.resumed ? " продолжена" : " создана"));
    }
    return out;
}

/* Текст ответа вызова по итогу хода ребёнка. Одинаков для обычного вызова
 * и для фоновой задачи (И8.11): состояние говорит признак, а внутри лежит
 * сама причина, и модель читает одно место вместо двух. */
std::string child_answer(const std::string& agent, const std::string& session_id,
                         const SubagentResult& r) {
    return task_wrapper(session_id, r.ok,
                        r.ok ? r.text
                             : ("Субагент " + agent + " не выполнил задачу.\n" +
                                r.error));
}

bool run_background_tasks(Engine& engine) {
    EngineState& state = engine.state();

    /* Очередь забирается целиком под локом и выполняется уже без него:
     * запрос к модели идёт без лока движка (D1 — висящий GUI). */
    std::vector<SubagentJob> jobs;
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        while (!state.background_tasks.empty()) {
            jobs.push_back(std::move(state.background_tasks.front()));
            state.background_tasks.pop_front();
        }
    }
    if (jobs.empty()) return false;

    /* Пока идёт фоновая работа, движок не «готов»: ход родителя уже
     * закончился, и «готово» при работающем субагене было бы враньём того
     * же класса, что И6.8 (исход → состояние). Состояние ПРИХРАНЯЕТСЯ и
     * возвращается на выходе: разбор очереди не ход (свой состояние
     * выставит run_task, если он начнётся), а оставленное «исполняется»
     * навсегда — враньё второго порядка, и на пределе цепочки, где хода не
     * будет, оно осталось бы висеть до следующего запроса. */
    AgentState prev_state = AgentState::Idle;
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        prev_state = state.state;
        state.state = AgentState::Executing;
    }
    engine.push_event(AgentEvent::Status,
                      "Фоновых задач: " + std::to_string(jobs.size()) +
                          " — выполняю между ходами.");

    std::string results;
    size_t done = 0;
    for (const SubagentJob& job : jobs) {
        /* «Стоп» между задачами: остальные не начинаются вовсе, а начатая
         * останавливается сама (флаг проверяется в цикле ребёнка, И8.12). */
        if (state.abort_requested.load()) break;
        engine.push_event(AgentEvent::Status,
                          "task (фон): субагент " + job.agent + " — " +
                              job.description);
        const ChildTurn t = run_child_turn(engine, state, job);
        results += child_answer(job.agent, job.session_id, t.result);
        results += "\n";
        ++done;
    }

    /* Отмена — результаты в диалог НЕ кладутся: цикл на отмене чистит
     * историю (см. AgentLoop::run), и доставка в пустую сессию воскресила
     * бы диалог, от которого человек отказался. Работа при этом не
     * потеряна: дочерние сессии записаны, и продолжить её можно по
     * идентификатору — поэтому в событии он и называется.
     *
     * Названо и СКОЛЬКО успело выполниться: «остановлены» без счёта
     * читалось бы как «ничего не началось» при обратном, и человек
     * счёл бы работу потерянной впустую (или наоборот — ждал бы её). */
    if (state.abort_requested.load()) {
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.state = AgentState::Aborted;
        }
        engine.push_event(
            AgentEvent::Status,
            "Фоновые задачи остановлены пользователем: выполнено " +
                std::to_string(done) + " из " + std::to_string(jobs.size()) +
                ". Их результаты в диалог НЕ доставлены — сделайте новый "
                "запрос, и они попадут в контекст." +
                (done > 0 ? " Выполненное сохранено, продолжить можно по "
                            "ses_… из списка задач."
                          : ""));
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.state = prev_state;
        state.session.push_back(
            Message::user(background_results_header(done) + results));
    }
    return true;
}

/* Описание субагента одной строкой: имя и его описание. Имя само по
 * себе модели ничего не говорит («wp_explore» — и что делать?), а
 * описание из конфига может содержать переводы строк, которые сломали бы
 * формат списка, поэтому они сворачиваются в пробелы. */
std::string subagent_line(const std::string& name) {
    const std::shared_ptr<const AgentDef> def =
        AgentRegistry::instance().find(name);
    std::string text = def ? def->description : std::string();
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    return text.empty() ? ("- " + name) : ("- " + name + " — " + text);
}

/* Список субагентов, доступных ВЫЗЫВАЮЩЕМУ (И8.13).
 *
 * Отбор по правилам ВЫЗЫВАЮЩЕГО, а не по наличию агента: «агент есть» и
 * «его можно позвать» — разные вещи, и модель не должна видеть в списке
 * того, кого звать запрещено (запрет пришёл бы как отказ на
 * заведомо отклонённый вызов — то есть шаг впустую).
 *
 * Правило вида «task: wp_explore → запретить» работает потому, что
 * паттерн у `task` — имя субагента (core/tool.cpp, И8.13), а не «*».
 *
 * Кто именно спрашивает: субагент (RunScope) либо сессия — тем же
 * правилом, каким решается enforcement в ToolRunner::run. */
std::string subagent_catalogue_for_caller() {
    const std::shared_ptr<agent::Info> rules =
        Engine::instance().scope_snapshot().info;
    std::string out;
    for (const std::string& name :
         AgentRegistry::instance().subagent_names()) {
        const PermissionAction action =
            rules ? rules->evaluate("task", name)
                  : Engine::instance().permissions().evaluate("task", name);
        if (action == PermissionAction::Deny) continue;
        out += subagent_line(name);
        out += "\n";
    }
    return out;
}

/* Описание инструмента целиком. Список субагентов — часть описания, а не
 * отдельная строка каталога: описание `task` читают как текст, и список
 * внутри него остаётся в том же месте при любом размере. */
std::string task_description() {
    std::string text =
        "Передать отдельную задачу субагенту и получить его итог текстом."
        " Зови, когда задача самостоятельна и нужен её РЕЗУЛЬТАТ, а не"
        " ход работы: обзор проекта, разбор вопроса, правка одного файла."
        " Субагент работает со своим отдельным контекстом, в твой контекст"
        " попадёт только его итоговый текст — поэтому пиши в prompt всё,"
        " что нужно знать: разговор с пользователем ему не пересказывают."
        " description — короткое имя задачи в 2-3 слова (по нему человек"
        " видит, что запущено), prompt — полное задание для субагента,"
        " включая то, в каком виде вернуть ответ, subagent_type — имя"
        " агента из списка ниже. Уточнить задание у субагента нельзя:"
        " он не разговаривает с пользователем, и переспросить он не сможет."
        " background: true ставит задачу в ФОН: вызов вернётся сразу, работа"
        " пойдёт после того, как закончится твой ход, а итог придёт отдельным"
        " сообщением. Опрашивать прогресс нельзя и не нужно: повторный task с"
        " тем же task_id будет отклонён, пока задача не закончилась."
        "\n  Доступные субагенты:\n";
    const std::string list = subagent_catalogue_for_caller();
    /* Пустой список — не «доступны все», а «доступных нет»: иначе модель
     * выбрала бы имя наугад и потратила шаг на отказ. */
    text += list.empty() ? std::string("  (нет: делегирование запрещено "
                                       "правилами разрешений)\n")
                         : list;
    return text;
}

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
    /* Описание собирается при КАЖДОЙ сборке каталога, то есть попадает в
     * каждый запрос (И8.13): список субагентов живой, и статическая
     * строка разошлась бы с ним после первого же загруженного агента.
     * `description` остаётся заполненной — по ней инструмент
     * показывается в панели и в отладке, где вызова каталога нет. */
    def.description = task_description();
    def.describe_dynamic = []() { return task_description(); };
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
    b.str("task_id", "идентификатор РАНЕЕ ЗАПУЩЕННОЙ задачи этого же "
                     "субагента (вида ses_000000000123), чтобы продолжить "
                     "её с того места, где она остановилась; поле пустое — "
                     "это новая задача");
    b.boolean("background", "поставить задачу в ФОН: вызов вернётся сразу, "
                            "работа пойдёт после того, как закончится текущий "
                            "ход, а итог придёт отдельным сообщением в диалог. "
                            "Не опрашивай прогресс и не перезапускай такую же "
                            "задачу; поле пустое — работа ждёт результата "
                            "здесь же, в этом вызове");
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

        /* --- 2. task_id: продолжение дочерней сессии (И8.9) ---
         *
         * Отказ 8.7 на это поле снят: продолжение написано. Проверка
         * порядка не формальность — `task_id` приходит ОТ МОДЕЛИ, а из
         * него склеивается путь к файлу, поэтому всё, что не является
         * идентификатором сессии, отсекается ДО обращения к диску.
         *
         * Список задач субагента — это файлы в подкаталоге
         * `sessions/sub` (kSubagentSessionDir). Отдельный каталог, а не
         * фильтр по parent_id в общем: правило resume («самый свежий файл»)
         * обходит каталог нерекурсивно, и файл ребёнка рядом с сессией
         * человека стал бы «текущей сессией» — resume открыл бы диалог
         * субагента вместо диалога пользователя. Поэтому проверка «это
         * задача субагента» получилась бесплатно: идентификатор сессии
         * человека в подкаталоге не лежит, и ответ — «не найдена». */
        std::string task_id = args.get_string("task_id");
        SessionFile resumed;
        /* Идентификатор родителя — до проверки task_id: он нужен и для
         * сверки «чья это задача», и для записи дочерней сессии, и
         * выдаётся один раз (ensure_session_id, И8.9). */
        const std::string parent_id = engine.ensure_session_id();
        if (!task_id.empty()) {
            if (!SessionArchive::is_session_id(task_id)) {
                out.output = refuse(
                    "поле «task_id» — не идентификатор задачи: ожидается"
                    " ses_ и 12 цифр. Поставь новую задачу без task_id.");
                return out;
            }
            /* И8.14: задача, которая ещё в очереди, — это ОПРОС, а не
             * продолжение. Проверка идёт ДО чтения файла: дочерней сессии
             * у поставленной задачи ещё нет, и проверка «файл есть»
             * объявила бы живую работу несуществующей — модель получила бы
             * «задача не найдена» и решила бы, что постановка не
             * сработала. */
            if (is_queued_background(state, task_id)) {
                out.output = refuse(
                    "задача " + task_id + " ещё выполняется в фоне. Опрашивать"
                    " её нельзя: сделай другую работу, а её итог придёт"
                    " отдельным сообщением.");
                return out;
            }
            const std::string dir = ctx.callbacks().path_data_dir
                                        ? ctx.callbacks().path_data_dir()
                                        : std::string();
            std::string error;
            const std::string path = SessionArchive::subagent_file_path(dir, task_id);
            std::vector<std::string> warnings;
            if (path.empty() ||
                !SessionArchive::load(path, resumed, &error, &warnings)) {
                out.output = refuse(
                    "задача " + task_id + " не найдена: " +
                    (error.empty() ? std::string("файла нет") : error) +
                    ". Поставь новую задачу без task_id.");
                return out;
            }
            for (const std::string& w : warnings) {
                engine.push_event(AgentEvent::Status, "task/" + task_id + ": " + w);
            }
            /* Чужая задача — из другого диалога. Проверяется только когда
             * у обоих есть идентификатор: у родителя он появляется при
             * первом сохранении, и до него любой `task_id` выглядел бы
             * «чужим». */
            if (!resumed.parent_id.empty() && resumed.parent_id != parent_id) {
                out.output = refuse(
                    "задача " + task_id + " принадлежит другому диалогу"
                    " (родитель " + resumed.parent_id + ", текущий " +
                    parent_id + "). Продолжить её здесь нельзя: субагент"
                    " получил бы чужую переписку. Поставь новую задачу без"
                    " task_id.");
                return out;
            }
        }

        /* --- 2a. Фон (И8.14) --- */
        /* Принимается, а не отклоняется: работа за полем написана. Само
         * поле разбирается ниже, где уже собраны правила ребёнка и предел
         * шагов, — постановка в фон это тот же вызов, только без хода
         * сейчас. */
        const bool background = args.get_bool("background");

        /* --- 3. Глубина вложенности (предел настраивается, И8.8) ---
         *
         * Глубина и предел читаются под ОДНИМ локом: решение «вложить ли
         * ещё» не должно собираться из двух снимков. Предел — из
         * настройки `wp_coder.subagent_depth` (EngineState, читается в
         * load_settings), а не из константы: константа осталась
         * дефолтом, и «два места, которые думают о пределе» здесь бы и
         * разошлись. */
        int depth = 0;
        int depth_limit = limits::kSubagentDepthLimit;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            depth = state.scope.depth + 1;
            depth_limit = state.subagent_depth;
        }
        if (depth > depth_limit) {
            /* Сообщение порта — «Subagent depth limit reached (N).» — и в
             * N теперь эффективное значение, а не константа: модель, которая
             * попросит человека поднять предел, должна увидеть, какой он
             * сейчас. Название настройки в тексте нужно по той же
             * причине, по которой отказ называет недоступных субагентов:
             * отказ без подсказки стоит модели следующего шага на ту же
             * ошибку. */
            out.output = refuse(
                "Subagent depth limit reached (" +
                std::to_string(depth_limit) +
                "). Глубже вкладывать субагентов нельзя: это кончилось бы"
                " рекурсивными запросами к модели. Предел — настройка"
                " wp_coder.subagent_depth, и поднимает её пользователь."
                " Сделай работу сам.");
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

        /* --- 5. Правила, предел шагов и ЗАДАНИЕ --- */
        std::vector<AgentLoadDiag> diags;
        /* for_subagent, а не from_def (И8.10): ребёнок наследует от
         * сессии только запреты и `external_directory`, а сверху получает
         * свои правила и авто-запрет на `task`/`todo`. Снимок правил
         * сессии берётся под локом движка разрешений: Info живёт дольше
         * вызова, а правила меняются из UI-потока. */
        const std::shared_ptr<agent::Info> info = agent::Info::for_subagent(
            *def, engine.permissions().rules_snapshot(), &diags);
        for (const AgentLoadDiag& d : diags) {
            engine.push_event(AgentEvent::Status,
                              "task/" + name + ": " + d.message);
        }

        int max_steps = 0;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            max_steps = state.max_steps;
        }
        if (info->steps() > 0) max_steps = info->steps();

        /* Идентификатор задачи и её название — ДО работы, а не после: ими
         * подписывается событие «субагент запущен» и обёртка результата
         * (И8.11), а у фоновой задачи по нему же модель узнаёт, что
         * поставлено, ещё до всякой работы. Выдавать его после означало бы,
         * что в событии запуска его нет.
         *
         * Название при продолжении остаётся прежним: описание нового
         * вызова — подпись конкретного продолжения, а не имя задачи, и
         * переименование по нему сделало бы «продолжить задачу»
         * неотличимым от «начать новую». */
        SubagentJob job;
        job.agent = name;
        job.description = description;
        job.prompt = prompt;
        job.session_id = task_id.empty() ? ids().next_session() : task_id;
        job.title = (task_id.empty() || resumed.title.empty())
                        ? (description + " (@" + name + " subagent)")
                        : resumed.title;
        job.parent_id = parent_id;
        job.info = info;
        job.depth = depth;
        job.max_steps = max_steps;
        job.seed = resumed.messages;
        job.resumed = !task_id.empty();

        /* --- 6. Фон или ход сейчас ---
         *
         * Оба пути собирают одно и то же задание (job) и уходят в общую
         * run_child_turn; различаются они только тем, КОГДА она
         * выполняется. Именно поэтому правила и глубина берутся здесь, до
         * ветвления: у фоновой задачи они снимаются в момент постановки
         * (см. SubagentJob), иначе к моменту выполнения область
         * принадлежала бы сессии. */
        if (background) {
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                state.background_tasks.push_back(job);
            }
            out.title = "task " + description + " (" + name + ", в фоне)";
            out.metadata.set("agent", name);
            out.metadata.set("depth", static_cast<long long>(depth));
            out.metadata.set("task", description);
            out.metadata.set("session_id", job.session_id);
            out.metadata.set("background", true);
            engine.push_event(AgentEvent::Status,
                              "task: субагент " + name + " — " + description +
                                  " поставлен в фон (" + job.session_id + ")");
            out.output = background_accepted(job.session_id, name, description);
            return out;
        }

        engine.push_event(AgentEvent::Status,
                          "task: субагент " + name + " — " + description);

        /* --- 7. Ход субагента --- */
        const ChildTurn t = run_child_turn(engine, state, job);

        out.title = "task " + description + " (" + name + ")";
        out.metadata.set("agent", name);
        out.metadata.set("depth", static_cast<long long>(depth));
        out.metadata.set("steps", static_cast<long long>(t.result.steps));
        out.metadata.set("task", description);
        out.metadata.set("session_id", job.session_id);
        out.metadata.set("session_saved", t.session_saved);

        /* И8.11: результат — обёртка с идентификатором задачи, и она
         * одинакова для успеха и отказа. Отдельным текстом «субагент не
         * выполнил задачу» больше нет: состояние говорит признак, а
         * внутри лежит сама причина, и модель читает одно место вместо
         * двух. */
        out.output = child_answer(name, job.session_id, t.result);
        return out;
    };

    ToolsRegistry::instance().register_def(std::move(def));
}

} // namespace coder

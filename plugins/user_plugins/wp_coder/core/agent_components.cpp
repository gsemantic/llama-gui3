// agent_components.cpp (Фаза D1) — реализация компонентов без goto.
// И5.7: цикл работает с событиями, история — со сообщениями с частями.

#include "agent_components.h"
#include "prompts.h"
#include "shell.h"
#include "engine.h"
#include "limits.h"
#include "json_utils.h"
#include "llm_source.h"
#include "snapshot.h"

#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace coder {

/* Лимиты ReAct-цикла — единый источник: core/limits.h (Фаза 4.5).
 * kMaxSteps/kSessionBudget переопределяются настройками агента (5.3):
 * state_.max_steps / state_.session_budget. */
using limits::kResultBudget;

/* Порог «застревания»: если N шагов подряд модель даёт короткий
 * ответ (< 200 символов), считаем, что она не может прогрессировать
 * и запрашиваем итоговый ответ. 200 — потому что ответ 100-190 символов
 * тоже слишком короткий для продуктивного шага агента. */
constexpr int kStuckThreshold = 3;
constexpr size_t kShortResponseLen = 200;
/* Порог «итога» живёт в limits.h (limits::kMinFinalAnswerLen): условие
 * завершения считает его в turn_verdict (core/message.h, И5.8), и второе
 * место с этим числом разъехалось бы с первым. */

/* ======================================================================
 * PermissionGate
 * ====================================================================== */

std::string PermissionGate::check(const std::string& abs_path) {
    std::string proj;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        proj = state_.project_dir;
    }
    if (!is_path_outside(abs_path, proj)) return "";
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (is_path_allowed(abs_path, proj, state_.allowed_external_paths)) return "";
    }

    /* И2.4/2.7: поверх списка «разрешённых навсегда» путей — правила
     * разрешений с ключом external_directory. Именно они дают whitelist
     * (data_dir, /tmp, навыки): без этого агент спрашивал бы
     * разрешение на каждый заход в /tmp, и вопрос стал бы помехой.
     * Порядок именно такой — список пользователя проверяется первым и
     * работает даже с пустым набором правил.
     *
     * И8.10: решение принимают ПРАВИЛА ТЕКУЩЕГО АГЕНТА, а не сессии.
     * Иначе наследование `external_directory` было бы мёртвым: ребёнок
     * получил бы правила в свой набор, а спрашивал бы всё равно сессия —
     * и запрет САМОГО агента («мне нельзя ходить в /etc») не действовал
     * бы. Правила сессии — случай «агента нет» (ход родителя).
     *
     * Указатель копируется под локом движка, а решение принимается ВНЕ
     * него: `Info::evaluate` берёт свой мьютекс, а брать два мьютекса в
     * разном порядке в одном месте — это ровно тот дедлок, ради которого
     * написан порядок блокировок в SESSION_START. */
    std::shared_ptr<agent::Info> agent_rules;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        agent_rules = state_.scope.info;
    }
    const PermissionAction action =
        agent_rules ? agent_rules->evaluate("external_directory", abs_path)
                    : engine().permissions().evaluate("external_directory",
                                                       abs_path);
    if (action == PermissionAction::Deny) {
        return "[запрещено] Доступ к пути вне проекта запрещён правилом: "
               + abs_path
               + "\nНЕ ПОВТОРЯЙ вызов и не ищи обход.";
    }
    if (action == PermissionAction::Allow) return "";

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.pending_permission_path = abs_path;
        state_.state = AgentState::WaitingPermission;
    }
    this->push_event_(AgentEvent::Tool, "[access] Требуется разрешение: " + abs_path);
    return "[ВАЖНО] Доступ запрещён. Файл вне проекта: " + abs_path
           + "\nНЕ ПОВТОРЯЙ вызов. Скажи пользователю что нужно нажать «Разрешить»."
           + "\nЖди подтверждения. Не пытайся снова до подтверждения.";
}

void PermissionGate::allow_once(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.allowed_external_paths.push_back(path);
        state_.once_path = path;
        state_.pending_permission_path.clear();
        state_.state = AgentState::Executing;
    }
    state_.permission_cv.notify_all();
}

void PermissionGate::allow_always(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.allowed_external_paths.push_back(path);
        state_.pending_permission_path.clear();
        state_.state = AgentState::Executing;
    }
    state_.permission_cv.notify_all();
    /* И2.7: то же решение попадает в правила разрешений, чтобы ключ
     * external_directory был единственным источником истины, а список
     * путей — его сохраняемым отражением. */
    engine().permissions().approve_path(path);
    std::string json = "[";
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        for (size_t i = 0; i < state_.allowed_external_paths.size(); ++i) {
            if (i > 0) json += ",";
            json += "\"" + state_.allowed_external_paths[i] + "\"";
        }
    }
    if (cb_.settings_set) cb_.settings_set("wp_coder.allowed_external_paths", json);
}

void PermissionGate::reject() {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.pending_permission_path.clear();
    state_.state = AgentState::Executing;
    state_.permission_cv.notify_all();
}

bool PermissionGate::is_waiting() const {
    std::lock_guard<std::mutex> lk(state_.mtx);
    return state_.state == AgentState::WaitingPermission;
}

void PermissionGate::wait(std::unique_lock<std::mutex>& lk) {
    state_.permission_cv.wait(lk, [this] {
        return state_.state != AgentState::WaitingPermission
            || state_.shutting_down || state_.abort_requested.load();
    });
}

PermissionOutcome permission_outcome(EngineState& state, const std::string& path) {
    /* Ответ пользователя — это список разрешённых путей, а не флаг:
     * allow_once/allow_always кладут путь туда, reject() — нет. Спрашивать
     * «какое было решение» отдельным флагом значило бы завести второе
     * место, где живёт один и тот же факт, и они разъедутся.
     *
     * Лок внутри: вызывающий цикла лока НЕ держит (state_.mtx
     * нерекурсивный). Именно эта функция заменила в цикле безусловное
     * добавление пути в allowed_external_paths: после отказа путь
     * попадал в список и повторный вызов проходил уже без вопроса —
     * то есть отказ не означал ничего (дефект, найденный при И5.7). */
    std::string project;
    bool allowed = false;
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        project = state.project_dir;
        allowed = is_path_allowed(path, project, state.allowed_external_paths);
    }
    return allowed ? PermissionOutcome::Granted : PermissionOutcome::Rejected;
}

/* ======================================================================
 * ToolRunner
 * ====================================================================== */

namespace {

/* И8.7: правила того, кто выполняет ход. nullptr — ход сессии, решения
 * принимает PermissionEngine. Снимок (shared_ptr) берётся под локом и
 * используется без него: agent::Info не копируется, а его правила
 * заморожены, и блокировка state_.mtx на всё время решения брала бы
 * общий мьютекс движка на пустой операции. */
std::shared_ptr<agent::Info> scope_agent(EngineState& state) {
    std::lock_guard<std::mutex> lk(state.mtx);
    return state.scope.info;
}

/* Тот же снимок для СЧЁТЧИКА зацикливания: у вложенного хода он свой
 * (RunScope::loop_calls), иначе вызовы субагента выглядели бы
 * вызовами родителя. */
std::shared_ptr<std::deque<std::string>> scope_calls_holder(EngineState& state) {
    std::lock_guard<std::mutex> lk(state.mtx);
    return state.scope.loop_calls;
}

/* Куда пишется отпечаток вызова: свой счётчик вложенного хода либо
 * родительский state_.recent_calls. Ссылка действительна, пока
 * ScopedAgentScope жив (держит holder), и всё обращение с ней — под
 * state_.mtx. */
std::deque<std::string>& scope_loop_calls(EngineState& state) {
    const std::shared_ptr<std::deque<std::string>> own = scope_calls_holder(state);
    return own ? *own : state.recent_calls;
}

} // namespace

ToolOutcome ToolRunner::run(const std::string& tool_name,
                            const json::JsonValue& args) {
    const ToolDef* def = ToolsRegistry::instance().find(tool_name);
    if (!def) {
        ToolOutcome outcome;
        outcome.error = "[ошибка] неизвестный инструмент: " + tool_name
            + "\nДоступные инструменты: " + ToolsRegistry::instance().join_tools();
        this->push_event_(AgentEvent::Error, (tool_name + " — неизвестный инструмент").c_str());
        return outcome;
    }

    /* И1.7: enforcement режимов — ДО fingerprint и ДО вызова.
     * Это единственное место, где решается, допустим ли инструмент.
     * Лок отпускаем до push_event_ (state_.mtx нерекурсивный). */
    std::string refusal;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        refusal = check_tool_mode_policy(tool_name, *def, state_.mode, state_.plan_mode);
    }
    if (!refusal.empty()) {
        this->push_event_(AgentEvent::Error, refusal);
        ToolOutcome outcome;
        outcome.error = refusal;
        return outcome;
    }

    /* И2.5: разрешение по ключу инструмента. Идёт ПОСЛЕ проверки
     * режима (запрещённый режимом инструмент не спрашивает разрешения:
     * вопрос пользователю про то, что всё равно нельзя, только путает)
     * и ДО отпечатка зацикливания (отказ — тоже результат вызова, и он
     * должен попасть в историю иначе три одинаковых отказа сочтутся
     * дословно тем же, что три одинаковых успешных вызова).
     *
     * И8.7: решает ТЕКУЩИЙ агент, а не сессия. У субагента в
     * agent::Info уже сложены правила сессии и его собственные (И8.4),
     * поэтому тот же вопрос задаётся тому же объекту — а правила сессии
     * отдельно спрашивать было бы вторым ответом на один вопрос.
     * Снимок области берётся под локом, а решения — уже без него. */
    const std::string perm_key = permission_key_of(*def);
    const std::string perm_pattern = permission_pattern(*def, args);
    PermissionEngine& perms = engine().permissions();
    const std::shared_ptr<agent::Info> agent_rules = scope_agent(state_);
    const auto evaluate = [&perms, &agent_rules](const std::string& key,
                                                  const std::string& pattern) {
        return agent_rules ? agent_rules->evaluate(key, pattern)
                           : perms.evaluate(key, pattern);
    };
    const PermissionAction perm_action = evaluate(perm_key, perm_pattern);
    if (perm_action == PermissionAction::Deny) {
        /* И8.13: при не-«звёздочном» паттерне назван и ЗНАЧЕНИЕ, иначе
         * отказ по «task: wp_explore → запретить» выглядел бы как «делегировать
         * нельзя» — и модель позвала бы другого агента, не понимая, что
         * запрещён именно этот. Для остальных инструментов это тоже полезно:
         * отказ по write/read называет путь. */
        const std::string what =
            perm_pattern == "*" ? std::string()
                                : (" (значение " + perm_pattern + ")");
        std::string denial =
            "[запрещено] Инструмент " + tool_name + " запрещён правилом"
            " разрешений (ключ «" + perm_key + "»" + what +
            "). Не ищи обход: другой"
            " инструмент с тем же эффектом тоже запрещён."
            "\nНЕ ПОВТОРЯЙ вызов. Скажи пользователю, что действие"
            " запрещено настройкой.";
        this->push_event_(AgentEvent::Error, denial);
        ToolOutcome outcome;
        outcome.error = denial;
        return outcome;
    }
    if (perm_action == PermissionAction::Ask) {
        std::string metadata = tool_name;
        if (perm_pattern != "*") metadata += " → " + perm_pattern;
        const std::string suggested = permission_suggested_pattern(*def, perm_pattern);
        /* ask() блокирует worker-поток до решения пользователя и сам
         * возвращает состояние движка, которое до него перевёл. */
        bool always = false;
        if (!perms.ask(perm_key, {perm_pattern}, suggested, metadata, &always)) {
            std::string refusal =
                "[отказ] Пользователь не разрешил: " + tool_name
                + (perm_pattern == "*" ? "" : " (" + perm_pattern + ")")
                + "\nНЕ ПОВТОРЯЙ вызов и не ищи обход. Спроси пользователя,"
                  " что делать дальше, или перейди к другим задачам.";
            this->push_event_(AgentEvent::Error, refusal);
            ToolOutcome outcome;
            outcome.error = refusal;
            return outcome;
        }
        /* И8.7: ответ «всегда» пишется в правила ДВИЖКА, а правила
         * агента заморожены при его сборке. Без этой строки субагент
         * спросил бы то же самое при каждом следующем вызове — то есть
         * кнопка «всегда» работала бы в сессии и не работала у
         * субагента. Паттерн — ТОТ ЖЕ, что записан у сессии
         * (suggested), иначе у ребёнка и у родителя окажутся разные
         * правила об одном решении пользователя. Пустой suggested
         * (у doom_loop он пуст намеренно) ничего не пишет — так же,
         * как у сессии. */
        if (always && !suggested.empty() && agent_rules) {
            agent_rules->approve(perm_key, suggested);
        }
    }

    /* Детектор зацикливания (И4.11).
     *
     * Смотрим ТОЛЬКО на последние три вызова, а не на окно из восьми:
     * в окне из восьми одинаковые отпечатки «плавающего» вызова (например
     * git_status с меняющимся состоянием рабочей копии) накапливались и
     * через несколько шагов выглядели как зацикливание, хотя вызовы шли
     * с разными результатами.
     *
     * Раньше детектор сам отменял вызов. Теперь он СПРАШИВАЕТ: три
     * одинаковых вызова — это ещё не всегда зацикливание (агент вправе
     * повторить команду после её провала), и решение об этом не наше, а
     * пользователя. Отказ после ответа — как у любого разрешения. */
    std::string fp = tool_name + "\n" + args.dump();
    bool loop_detected = false;
    {
        /* И8.7: счётчик — того, кто выполняет ход. У субагента он свой
         * (RunScope::loop_calls), и общий счётчик видел бы три
         * одинаковых чтения ребёнка как зацикливание РОДИТЕЛЯ: вопрос
         * «зацикливание?» ушёл бы человеку про вызовы, которых он не
         * видел, а отказ обрывал бы работу субагента. */
        std::deque<std::string>& calls = scope_loop_calls(state_);
        std::lock_guard<std::mutex> lk(state_.mtx);
        const size_t n = calls.size();
        loop_detected = n >= 2 && calls[n - 1] == fp && calls[n - 2] == fp;
        calls.push_back(fp);
        while (calls.size() > 2) calls.pop_front();
    }  /* mtx отпущен — ask() берёт свой mtx_ */
    if (loop_detected) {
        bool allowed = true;
        if (evaluate("doom_loop", tool_name) == PermissionAction::Ask) {
            /* always_pattern пустой: повторять один и тот же вызов можно
             * сколько угодно, записывать это в постоянные правила нечего. */
            allowed = perms.ask("doom_loop", {tool_name}, "",
                                "Зацикливание: " + tool_name);
        }
        if (!allowed) {
            {
                std::deque<std::string>& calls = scope_loop_calls(state_);
                std::lock_guard<std::mutex> lk(state_.mtx);
                calls.clear();
            }
            this->push_event_(AgentEvent::Error,
                "Инструмент " + tool_name + " вызван 3 раза подряд с"
                " одинаковыми аргументами — пользователь прервал зацикливание.");
            ToolOutcome outcome;
            outcome.error =
                "[отказ] зацикливание вызова " + tool_name +
                ": пользователь не разрешил повтор. НЕ ПОВТОРЯЙ тот же"
                " вызов — смени подход или спроси пользователя.";
            return outcome;
        }
        /* Разрешено: сбрасываем счётчик, иначе следующий такой же вызов
         * снова спросил бы (а после «всегда» это просто шум). */
        {
            std::deque<std::string>& calls = scope_loop_calls(state_);
            std::lock_guard<std::mutex> lk(state_.mtx);
            calls.clear();
        }
        this->push_event_(AgentEvent::Status,
            "Зацикливание: " + tool_name + " вызван 3 раза подряд —"
            " продолжаю по решению пользователя.");
    }

    ToolOutput out = ToolsRegistry::instance().run_output(tool_name, args);
    std::string label = tool_name;
    if (!out.title.empty()) label += " (" + out.title + ")";
    this->push_event_(AgentEvent::Tool, label + " -> " + out.output);
    /* И5.7: наружу отдаётся ToolOutput целиком, а не строка вывода.
     * Часть сообщения хранит заголовок, метаданные и признак усечения
     * (core/message.h), и файл сессии их тоже сохраняет — раньше всё это
     * доходило только до UI, а в истории оставалась голая строка. */
    ToolOutcome outcome;
    outcome.ok = true;
    outcome.output = std::move(out);
    return outcome;
}

bool ToolRunner::is_loop_guard(const std::string& fingerprint) const {
    std::lock_guard<std::mutex> lk(state_.mtx);
    if (state_.recent_calls.size() >= 2 &&
        state_.recent_calls[state_.recent_calls.size() - 1] == fingerprint &&
        state_.recent_calls[state_.recent_calls.size() - 2] == fingerprint) {
        return true;
    }
    return false;
}

void ToolRunner::record_call(const std::string& fingerprint) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.recent_calls.push_back(fingerprint);
    if (state_.recent_calls.size() > 8) {
        state_.recent_calls.pop_front();
    }
}

/* ======================================================================
 * Planner
 * ====================================================================== */

/* Признак плана в истории. Живёт здесь, а не в цикле и не в разборе
 * частей: план — это обычное текстовое сообщение ассистента с
 * характерным началом, и проверять его должна одна функция. Иначе
 * «есть ли план» и «как план выглядит» разъедутся (D12). */
namespace {

bool message_is_plan(const Message& m) {
    return m.is_assistant() && m.text().rfind("[ПЛАН]", 0) == 0;
}

const char* kPlanPrompt =
    "Составь краткий пошаговый план решения задачи (без вызова "
    "инструментов, без wp_action). Перечисли только шагам одним "
    "нумерованным списком. Потом ты выполнишь их инструментами.";

} // namespace

namespace {

/* Учёт токенов хода (И7.2).
 *
 * ОДНО место на три точки вызова — планировщик, цикл и хвостовой ход.
 * Списком по три строки в каждой точке они и раньше дублировались, и
 * добавление четвёртого счётчика (measured_input_tokens) в трёх местах
 * рано или поздно разъехалось бы: забыть — значит компакшн получит
 * устаревшее измерение и будет считать контекст по чужому запросу.
 *
 * Лок берётся внутри — вызывающие цикла лока НЕ держат (state_.mtx
 * нерекурсивный, общий с UI: та же причина, что у permission_outcome).
 *
 * Считается именно input, а не total: контекстом является то, что ушло
 * В модель, а ответ следующего шага будет другим. */
void record_turn_usage(EngineState& state, const Usage& usage) {
    std::lock_guard<std::mutex> lk(state.mtx);
    state.total_prompt_tokens += static_cast<int>(usage.input);
    state.total_completion_tokens += static_cast<int>(usage.output);
    if (usage.input > 0) state.measured_input_tokens = usage.input;
}

} // namespace

/* ======================================================================
 * Planner
 * ====================================================================== */

bool Planner::plan(const std::string& sys_prompt) {
    if (!state_.use_planning) return false;

    /* Если в сессии уже есть план — не генерируем новый.
     * Раньше каждый submit() очищал сессию и план терялся,
     * теперь сессия сохраняется между запросами. */
    std::vector<Message> history;
    bool plan_exists = false;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        history = state_.session;
        for (const Message& msg : history) {
            if (message_is_plan(msg)) {
                plan_exists = true;
                break;
            }
        }
    }  /* mtx отпущен ДО push_event_ — иначе deadlock: push_event_
        * внутри тоже захватывает state_.mtx, а std::mutex не рекурсивный. */
    if (plan_exists) {
        std::cerr << "[wp_coder] planner: plan already exists, skipping" << std::endl;
        this->push_event_(AgentEvent::Status,
            "План уже существует — продолжаю по нему.");
        return true;
    }
    std::cerr << "[wp_coder] planner: no existing plan, generating..." << std::endl;

    std::vector<ModelMessage> plan_msgs = to_model_messages(history);
    plan_msgs.push_back({std::string(kRoleUser), kPlanPrompt});

    this->push_event_(AgentEvent::Status, "Составляю план...");
    std::vector<LlmEvent> events;
    const bool plan_ok =
        llm_source::fetch(cb_, sys_prompt, plan_msgs, events);
    const LlmResponse plan = llm_source::fold(events);
    if (plan_ok && !plan.text().empty() && !state_.abort_requested.load()) {
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            /* План — обычное сообщение ассистента с одной текстовой
             * частью. Отдельного вида части у него нет: он не часть
             * хода, а целый ход, и в транскрипт уходит как текст. */
            Message plan_msg = Message::assistant(
                last_user_message(state_.session) ? last_user_message(state_.session)->id
                                                  : std::string());
            plan_msg.parts.push_back(MessagePart::text("[ПЛАН]\n" + plan.text()));
            state_.session.push_back(std::move(plan_msg));
        }
        /* Вне блока выше: record_turn_usage берёт лок сам, а state_.mtx
         * нерекурсивный (правило 1 в SESSION_START.md). */
        record_turn_usage(state_, plan.usage());
        this->push_event_(AgentEvent::Assistant,
            "План: " + plan.text());
        return true;
    } else if (!plan_ok && !state_.abort_requested.load()) {
        this->push_event_(AgentEvent::Error, "Не удалось составить план — работаю без него.");
    }
    return false;
}

/* ======================================================================
 * Запрос к модели для сводки (И7.10)
 * ======================================================================
 *
 * Сводка — фоновая работа, а не ход агента: она не достаётся ни
 * инструментов, ни реплики в UI, и её ответ не попадает в историю
 * (кроме самой сводки, которую положит сжатие). Поэтому здесь
 * блокирующий запрос и ноль событий наружу.
 *
 * Функция СНАРУЖИ по той же причине, что и у сжатия: движок не умеет
 * спрашивать модель в тесте, а проверять надо решение. */
namespace {

compaction::SummaryTurn summarizer_turn(const HostCallbacks& cb) {
    return [&cb](const std::string& sys_prompt,
                 const std::vector<ModelMessage>& history,
                 std::string& text, std::string& error) {
        std::vector<LlmEvent> events;
        if (!llm_source::fetch(cb, sys_prompt, history, events)) {
            error = "провайдер не ответил на запрос сводки";
            return false;
        }
        const LlmResponse answer = llm_source::fold(events);
        if (answer.text().empty()) {
            error = "пустой ответ на запрос сводки";
            return false;
        }
        text = answer.text();
        return true;
    };
}

} // namespace

/* ======================================================================
 * AgentLoop
 * ====================================================================== */

namespace {

/* Обрезка результата инструмента до kResultBudget — главный способ
 * экономии токенов: полный результат может быть 5–10 КБ, и он уходит на
 * КАЖДОМ следующем шаге. Модели достаточно начала для решения; нужно
 * больше — вызовет повторно.
 *
 * ToolOutput.truncated здесь НЕ ставится: этот флаг означает «инструмент
 * усек вывод, остаток в файле» (И4.10), а у нас остатка нигде нет. О том,
 * что вывод обрезан, говорит сам текст. */
ToolOutput cap_result(ToolOutput out) {
    if (out.output.size() <= kResultBudget) return out;
    const size_t full = out.output.size();
    out.output = text::utf8_prefix(out.output, kResultBudget);
    out.output += "\n[...обрезано, всего " + std::to_string(full) +
                  " символов. Вызови инструмент повторно, если нужно больше.]";
    return out;
}

/* Хвостовое напоминание (И5.9, порт prompt.ts:1281).
 *
 * Текст один на два места, где кончается бюджет шагов: напоминание на
 * последнем шаге и форсированный итог после него. Два разных текста
 * разъехались бы, и модель получала бы «не вызывай инструменты» и
 * «инструменты нельзя» — фразы, отличающиеся одним словом и означающие
 * одно и то же. Детектор застревания сюда НЕ относится: у него своя
 * причина («ты не прогрессируешь»), и путать её с исчерпанием шагов
 * модель не должна. */
const char* kLastStepReminder =
    "Это последний шаг. Инструменты больше вызывать НЕЛЬЗЯ — бюджет шагов "
    "исчерпан, и следующий твой ответ станет ответом пользователю. Дай "
    "текстовый итог по результатам проделанной работы: что сделано, что "
    "не сделано и что нужно от пользователя.";

} // namespace

/* И10.1: снимок рабочего каталога в начале шага.
 *
 * Порядок здесь не косметический, и оба пункта — про блокировки:
 *
 *   1. Каталог проекта и предыдущая причина неудачи читаются под
 *      state.mtx и копируются — дальше лок не держится.
 *   2. Сам снимок берётся БЕЗ лока: внутри работает git (до
 *      limits::kSnapshotTimeoutSec) или копируется каталог, а UI ждёт
 *      тот же мьютекс. Лок на этом месте означал бы висящий GUI.
 *   3. Результат кладётся под локом — короткая запись. */
snapshot::Snapshot take_step_snapshot(EngineState& state,
                                      HostCallbacks& cb,
                                      AgentEventCallback push_event) {
    std::string project_dir;
    std::string prev_reason;
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        project_dir = state.project_dir;
        prev_reason = state.last_snapshot.reason;
    }

    const std::string data_dir = cb.path_data_dir ? cb.path_data_dir() : "";
    const snapshot::Snapshot snap =
        snapshot::take(project_dir, snapshot::store_dir(data_dir));

    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.last_snapshot = snap;
        if (snap.ok()) ++state.snapshots_taken;
    }

    /* Пустой каталог проекта — не отказ снимка, а отсутствие проекта:
     * об этом панель говорит отдельно, и второе сообщение на каждом
     * шаге было бы только шумом. */
    if (!snap.ok() && !project_dir.empty() && snap.reason != prev_reason &&
        push_event) {
        push_event(AgentEvent::Status,
                   "Снимок рабочего каталога не снят: " + snap.reason);
    }
    return snap;
}

bool AgentLoop::run(const std::string& sys_prompt, std::string& full_response) {
    bool final_given = false;
    int stuck_counter = 0;  // последовательных коротких ответов

    std::cerr << "[wp_coder] agent_loop: starting, max_steps=" << state_.max_steps << std::endl;

    for (int step = 0; step < state_.max_steps; ++step) {
        if (state_.abort_requested.load()) {
            this->push_event_(AgentEvent::Status, "[прервано пользователем]");
            full_response += "\n\n[прервано пользователем]";
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.session.clear();
            }
            return true;
        }

        /* И10.1: снимок рабочего каталога в начале шага. Раньше
         * откатывать правки агента было нечем: undo_edit держит ОДИН слот
         * .orig на файл, и вторая правка того же файла затирала первую.
         * Снимок снимается ДО запроса к модели — то есть до того, как
         * агент узнает, что ему делать, и состояние на снимке принадлежит
         * началу шага, а не его результату. */
        take_step_snapshot(state_, cb_, this->push_event_);

        /* И5.9: у цикла есть ПРЕДУПРЕЖДЕНИЕ о последнем шаге, а не только
         * жёсткий конец. max_steps остаётся предохранителем (о нём ниже), но
         * решение «шаги кончились» больше не принимается счётчиком: на
         * последнем шаге модель получает напоминание и обязана дать
         * текстовый итог вместо нового вызова инструмента (порт
         * prompt.ts:1281).
         *
         * Напоминание уходит в историю ДО сборки запроса, чтобы модель
         * увидела его в том же контексте, в котором будет отвечать. На
         * первом шаге (step == 0) оно не добавляется: там ещё нечего
         * заканчивать, и напоминание было бы враньём. */
        const bool is_last_step = (step + 1 >= state_.max_steps);
        if (is_last_step && step > 0) append_note(kLastStepReminder);

        /* История — снимок под локом, сборка реплик для модели — вне
         * лока: to_model_messages ходит по всем частям, а держать
         * state_.mtx на всё время сборки нельзя (UI ждёт тот же
         * мьютекс, и это тот висящий GUI, который закрыл D1). */
        std::vector<Message> history;
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            history = state_.session;
        }
        const std::vector<ModelMessage> model_msgs = to_model_messages(history);

        /* Родитель хода — последняя реплика пользователя. Именно на неё
         * отвечает ход, и именно её сравнивает условие завершения
         * (И5.8). Пусто — ход без родителя (тест, пустая сессия): такое
         * условие трактует отдельно, а не считает «есть родитель». */
        std::string parent_id;
        if (const Message* last_user = last_user_message(history)) {
            parent_id = last_user->id;
        }

        /* Диагностика: размер контекста, отправляемого в LLM. */
        {
            size_t total_chars = 0;
            for (const ModelMessage& m : model_msgs) total_chars += m.content.size();
            std::cout << "[wp_coder] step " << (step + 1) << ": msgs=" << model_msgs.size()
                      << " chars=" << total_chars << std::endl;
        }

        std::cerr << "[wp_coder] agent_loop: step " << (step+1) << " calling LLM..." << std::endl;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<LlmEvent> events;
        const bool ok = llm_source::fetch(cb_, sys_prompt, model_msgs, events);
        auto t1 = std::chrono::steady_clock::now();
        const double llm_s =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;

        /* Свёртка событий — единственное, что цикл делает с ответом.
         * Дальше он работает с текстом, вызовами, finish и метриками, а
         * не с блоками и префиксами: до И5.7 здесь был разбор строки
         * extract_action/parse_action и склейка «RESULT [x]:». */
        LlmResponse response = llm_source::fold(events);

        /* Метрики хода. Источник — usage из события Finish (И5.2): ровно то,
         * что прислал провайдер, посчитанное один раз и без перекрытия
         * счётчиков. Складывать prompt с кэшем «вручную» больше негде.
         *
         * Копия, а не ссылка: ниже в свёртку добавляются ToolResult и
         * ToolError, и ссылка на член, который сейчас будут трогать, —
         * ловушка на ровно ту ошибку, от которой здесь защищаются. */
        const Usage usage = response.usage();
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.steps = step + 1;
            state_.llm_total_time += llm_s;
        }
        /* Учёт токенов — вне блока, лок берёт record_turn_usage. */
        record_turn_usage(state_, usage);

        /* A4: usage токенов шага. */
        this->push_event_(AgentEvent::Status,
            "step " + std::to_string(step + 1) + ": "
            + std::to_string(usage.input) + "+" + std::to_string(usage.output)
            + " tok, " + std::to_string((int)llm_s) + "s");

        /* Ответа нет — это не пустой ход, а сбой: иначе модель молча
         * получила бы пустую историю и повторила бы тот же запрос. */
        if (!ok) {
            const std::string err =
                response.error().empty() ? "не ответил" : response.error();
            std::string diag;
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                diag = "session_msgs=" + std::to_string(state_.session.size())
                     + " step=" + std::to_string(step + 1);
            }
            this->push_event_(AgentEvent::Error,
                ("[ошибка] LLM: " + err + " (" + diag + ")").c_str());
            full_response = full_response.empty()
                ? "[ошибка] LLM: " + err : full_response;
            return false;
        }

        /* ОДИН ход — ОДНО сообщение (И5.7). Внутри: размышление, текст,
         * вызовы, в том порядке, в каком пришли события. Раньше тот же ход
         * разбрасывался по нескольким сообщениям, и текст после результата
         * инструмента модель читала как отдельную реплику. */
        Message turn = turn_to_message(response, parent_id);
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.session.push_back(turn);
        }
        /* Записать ход обратно после того, как закрылись части вызовов.
         * Лок внутри: вызывающий цикла лока не держит, а держать его на
         * всю работу инструмента нельзя (UI ждёт тот же мьютекс). */
        const auto commit_turn = [this, &turn](void) {
            std::lock_guard<std::mutex> lk(state_.mtx);
            if (Message* stored = find_message(state_.session, turn.id)) {
                *stored = turn;
            }
        };

        /* Детектор застревания: короткий текст БЕЗ вызова инструмента.
         * Проверка вызовов идёт по событиям, а не по «нашёлся ли блок в
         * строке»: раньше короткий вызов инструмента (< 200 символов)
         * ошибочно считался застреванием, и после трёх таких вызовов агент
         * падал с ошибкой «LLM: не ответил». */
        if (response.text().size() < kShortResponseLen &&
            response.tool_calls().empty()) {
            ++stuck_counter;
            if (stuck_counter >= kStuckThreshold) {
                this->push_event_(AgentEvent::Status,
                    "Модель застряла — запрашиваю итоговый ответ.");
                commit_turn();
                ask_for_summary(sys_prompt,
                    "Ты делаешь очень короткие ответы и не прогрессируешь. "
                    "Инструменты больше вызывать НЕЛЬЗЯ. "
                    "Дай итоговый ответ по результатам проделанной работы.",
                    full_response);
                return true;
            }
        } else {
            stuck_counter = 0;  // вызов инструмента или длинный ответ = прогресс
        }

        if (response.finish_reason() == "length") {
            this->push_event_(AgentEvent::Status,
                "Внимание: модель упёрлась в лимит токенов (finish=length). "
                "Разбей задачу на шаги.");
        }

        /* Текст хода — пользователю. Это response.text(), а не «строка без
         * блока»: блок вызова сюда не попадает в принципе, его разбор — забота
         * адаптера (core/llm_source.h). */
        if (!response.text().empty()) {
            if (!full_response.empty()) full_response += "\n\n";
            full_response += response.text();
            this->push_event_(AgentEvent::Assistant, response.text());
        }

        /* Вызовы инструментов. Аргументы приходят разобранными (И5.1/И5.3),
         * поэтому ни parse_action, ни проекция в 8 слотов здесь не нужны:
         * новый параметр инструмента доходит до обработчика сам. */
        const size_t call_count = response.tool_calls().size();
        for (size_t i = 0; i < call_count; ++i) {
            /* Копия, а не ссылка: reduce() меняет сам вектор вызовов, и
             * ссылка на его элемент повисла бы. */
            const LlmToolCall call = response.tool_calls()[i];

            /* Вызов, закрытый ошибкой ещё в адаптере (мусор в блоке,
             * аргументы без имени инструмента), выполнять нельзя: части уже
             * несёт его исход, и модель увидит его в транскрипте. */
            if (!call.runnable()) continue;

            if (MessagePart* part = find_tool_part(turn, call.call_id)) {
                part->set_running();
            }

            ToolRunner tool_runner(state_, cb_, this->push_event_);
            const ToolOutcome outcome =
                tool_runner.run(call.name, call.arguments);
            if (outcome.ok) {
                LlmResponse::reduce(response, LlmEvent::tool_result(
                    call.call_id, cap_result(std::move(outcome.output))));
            } else {
                LlmResponse::reduce(response, LlmEvent::tool_error(
                    call.call_id, outcome.error));
            }
            /* Состояние вызова переносится в часть по call_id. Обе копии
             * (свёртка ответа и история) обязаны говорить одно и то же: иначе
             * файл сессии сохранит «не начат» для отработавшего вызова, а
             * условие завершения (И5.8) не наступит. */
            sync_tool_parts(turn, response);

            if (state_.abort_requested.load()) {
                full_response += "\n\n[прервано пользователем]";
                {
                    std::lock_guard<std::mutex> lk(state_.mtx);
                    state_.session.clear();
                }
                return true;
            }

            /* Внешний путь: PermissionGate перевёл движок в ожидание. */
            std::string perm_path;
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                if (state_.state == AgentState::WaitingPermission) {
                    perm_path = state_.pending_permission_path;
                }
            }
            if (perm_path.empty()) continue;

            /* Ход записывается ДО ожидания: следующий запрос к модели уже
             * должен видеть отказ по этому вызову, иначе модель повторит
             * его вслепую. */
            commit_turn();
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.waiting_in_sync = true;
            }
            this->push_event_(AgentEvent::Status,
                "Ожидание разрешения: " + perm_path);
            if (cb_.chat_event) {
                cb_.chat_event("[ai-coder] ⏸ ожидание разрешения: " + perm_path);
            }
            {
                std::unique_lock<std::mutex> lk(state_.mtx);
                state_.permission_cv.wait(lk, [this] {
                    return state_.state != AgentState::WaitingPermission
                        || state_.shutting_down || state_.abort_requested.load();
                });
                state_.waiting_in_sync = false;
            }
            if (state_.shutting_down) {
                full_response += "\n\n[агент остановлен]";
                return false;
            }
            if (state_.abort_requested.load()) {
                full_response += "\n\n[прервано пользователем]";
                {
                    std::lock_guard<std::mutex> lk(state_.mtx);
                    state_.session.clear();
                }
                return true;
            }

            /* Решение пользователя читается из списка разрешённых путей, а
             * не из флага: allow_once/allow_always кладут путь в список,
             * reject() — нет. Раньше здесь стояло безусловное добавление
             * пути в allowed_external_paths, и после ОТКАЗА повторный вызов
             * проходил уже без вопроса — то есть отказ не означал ничего. */
            const bool granted =
                permission_outcome(state_, perm_path) == PermissionOutcome::Granted;
            this->push_event_(AgentEvent::Status, granted
                ? "Доступ разрешён — продолжаю."
                : "Доступ запрещён пользователем — продолжаю без него.");
            append_note(granted
                ? ("(пользователь разрешил доступ к " + perm_path
                   + "; разрешение действует. Выполни инструмент ещё раз "
                     "с теми же аргументами.)")
                : ("(пользователь ОТКАЗАЛ в доступе к " + perm_path
                   + ". Не повторяй этот вызов: он запрещён. Скажи "
                     "пользователю, что нужно для работы, или предложи то же "
                     "самое внутри проекта.)"));
            break;  /* ход встал на ожидание: следующий вызов — в след. ходе */
        }

        /* Всё, что осталось незакрытым (ход встал на ожидание разрешения),
         * закрывается отказом. Незакрытая часть в истории означала бы, что
         * условие завершения (И5.8) не наступит никогда.
         *
         * Обход по индексу, а не по ссылке: reduce() добавляет вызов, если
         * такого call_id не было, и range-for с кэшированным end() после
         * этого прочитал бы за границей. */
        bool closed = false;
        for (size_t i = 0; i < response.tool_calls().size(); ++i) {
            if (response.tool_calls()[i].finished) continue;
            const std::string id = response.tool_calls()[i].call_id;
            LlmResponse::reduce(response, LlmEvent::tool_error(
                id, "вызов не выполнен: ход остановлен ожиданием разрешения"));
            closed = true;
        }
        if (closed) sync_tool_parts(turn, response);
        commit_turn();

        /* Условие завершения — ПОСЛЕ выполнения вызовов (И5.8). Проверять его
         * раньше нельзя: незакрытая часть-вызов означает, что результат ещё
         * не пришёл, и задача закрылась бы, потеряв его. */
        std::string current_user_id;
        {
            /* Последняя реплика пользователя читается ЗДЕСЬ, а не берётся из
             * снимка в начале хода: новое сообщение могло прийти посреди
             * работы инструмента, и тогда этот ход отвечает на прежнее
             * (условие 3 в turn_verdict). */
            std::lock_guard<std::mutex> lk(state_.mtx);
            const std::vector<Message> current = state_.session;
            if (const Message* last = last_user_message(current)) {
                current_user_id = last->id;
            }
        }
        const TurnVerdict verdict =
            turn_verdict(response, turn, current_user_id);
        if (verdict == TurnVerdict::Completed) {
            final_given = true;
            this->push_event_(AgentEvent::Status, "Готово (финальный ответ).");
            return true;
        }
        if (verdict == TurnVerdict::TooShort) {
            /* Преждевременный финал: текст без вызова инструмента, но и не
             * настоящий итог (короткий — «заголовок»/«начало работы»). Не
             * завершаем задачу — даём модели напоминание и продолжаем цикл.
             * Если модель реально застрянет, сработает stuck-детектор и
             * запросит принудительный итоговый ответ. */
            this->push_event_(AgentEvent::Status,
                "Модель не вызвала инструмент и не дала итог — продолжаю.");
            append_note(
                "[напоминание] Твой последний ответ не содержал вызова "
                "инструмента и слишком короток для итога. Продолжай задачу: "
                "вызови следующий инструмент (блок wp_action) либо, если "
                "задача завершена, дай ИТОГОВЫЙ ответ не короче 500 символов.");
        }

        /* Автосжатие по порогу окна (И7.10): пока переполнения нет —
         * ничего; переполнение есть — сводка отдельным агентом вместо
         * обрезки строк. Прежняя обрезка осталась внутри как аварийный
         * предохранитель и вызывается оттуда же.
         *
         * ВАЖНО: здесь нельзя брать state_.mtx перед вызовом: сжатие берёт
         * тот же нерекурсивный мьютекс, а запрос к модели идёт вообще без
         * него. Такая конструкция раньше приводила к дедлоку воркер-треда и
         * зависанию UI (D1). */
        engine().compact_history_if_needed(summarizer_turn(cb_));
    }

    /* Предохранитель (И5.9). Обычного пути сюда не приводит: на последнем
     * шаге модель получила напоминание и ответила текстом, и условие
     * завершения (И5.8) закрыло задачу. Сюда попадают два случая: модель на
     * последнем шаге всё-таки вызвала инструмент (напоминание она
     * проигнорировала) или цикл оборвался на отмене. В обоих нужен ответ
     * пользователю, а не молчание. */
    if (!final_given && !state_.abort_requested.load()) {
        this->push_event_(AgentEvent::Status,
            "Шаги исчерпаны, а текстового ответа так и нет — прошу итоговый.");
        ask_for_summary(sys_prompt, kLastStepReminder, full_response);
    }

    return final_given;
}

void AgentLoop::append_note(const std::string& text) {
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.session.push_back(Message::user(text));
}

bool AgentLoop::ask_for_summary(const std::string& sys_prompt,
                                const std::string& reminder,
                                std::string& full_response) {
    /* Напоминание — обычная реплика пользователя: так её видела модель и
     * раньше, и иначе пришлось бы заводить второе правило разбора ролей. */
    Message note;
    std::vector<ModelMessage> msgs;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        note = Message::user(reminder);
        state_.session.push_back(note);
        const std::vector<Message> history = state_.session;
        msgs = to_model_messages(history);
    }

    std::vector<LlmEvent> events;
    const bool ok = llm_source::fetch(cb_, sys_prompt, msgs, events);
    const LlmResponse answer = llm_source::fold(events);
    if (!ok || answer.text().empty()) {
        /* И6.8: хвостовой ход — тоже ход. Раньше он просто возвращал false,
         * и задача, у которой сломался именно он, выглядела выполненной по
         * результату основного цикла. */
        const std::string err = answer.error().empty() ? "не ответил"
                                                       : answer.error();
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            if (state_.outcome == TaskOutcome::None) {
                state_.outcome = TaskOutcome::Failed;
                state_.outcome_reason = "хвостовой ход: " + err;
            }
        }
        this->push_event_(AgentEvent::Error, ("[ошибка] " + err).c_str());
        return false;
    }

    /* Ответ уходит и в результат задачи, и в историю. Раньше он шёл
     * только в результат, и вопрос «а что ты сделал?» после задачи
     * получал пустоту: последняя в истории реплика — напоминание. */
    Message msg = Message::assistant(note.id);
    msg.parts.push_back(MessagePart::text(answer.text()));
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.session.push_back(std::move(msg));
        state_.steps += 1;
    }
    /* Вне блока: record_turn_usage берёт state_.mtx сам. */
    record_turn_usage(state_, answer.usage());
    if (!full_response.empty()) full_response += "\n\n";
    full_response += answer.text();
    this->push_event_(AgentEvent::Assistant, answer.text());
    return true;
}

/* ======================================================================
 * Вложенный ход субагента (И8.7)
 * ======================================================================
 *
 * ПОЧЕМУ ЭТО НЕ AgentLoop
 *
 * AgentLoop работает с СЕССИЕЙ: его история — state_.session, он зовёт
 * Planner, компактит историю, ждёт разрешения и двигает состояние
 * движка. У субагента своя история (своя, короткая, никуда не
 * сохраняется — дочерняя сессия это 8.9), и всё перечисленное выше
 * для него не просто лишнее, а ВРЕДНОЕ:
 *   - своя история нужна, чтобы ход ребёнка не попал в историю
 *     родителя (иначе родитель увидел бы чужой диалог в своей
 *     транскрипте и счёл его своим);
 *   - Planner звать нельзя: он пишет в state_.session, то есть
 *     подмешивал бы родителю план ребёнка;
 *   - compact_history_if_needed звать нельзя: сжатие сбрасывает
 *     state_.session и измеренные токены — то есть СЖАЛО БЫ ИСТОРИЮ
 *     РОДИТЕЛЯ посреди его хода, и ход, который к ней вернётся, был бы
 *     уже другим;
 *   - ожидание разрешения пользователя не нужно: ребёнок спрашивает
 *     тем же PermissionEngine (ToolRunner::run — единственная
 *     enforcement-точка), и этот вопрос пойдёт в общий диалог.
 *
 * Переиспользовать можно и нужно МЕХАНИКУ, и она здесь общая с
 * родителем: llm_source::fetch + fold, ToolRunner::run, cap_result,
 * sync_tool_parts, record_turn_usage. Политика (история, шаги, условие
 * завершения, сжатие) — своя, и она здесь написана целиком.
 *
 * Условие завершения — тоже своё, а не turn_verdict: родительский ждёт
 * ответа на последнюю реплику пользователя и считает короткий текст
 * «началом работы» (kMinFinalAnswerLen). Субагенту реплика пользователя не
 * адресована, а короткий ответ законен («класс AuthService, файл
 * auth.php»), и цикл, вошедший в вечную переписку «продолжай» из-за
 * длины текста, стоил бы дороже самой задачи.
 */

SubagentResult run_subagent_turn(EngineState& state, HostCallbacks& cb,
                                 AgentEventCallback push_event,
                                 const std::string& sys_prompt,
                                 const std::string& prompt, int max_steps,
                                 std::vector<Message> seed) {
    SubagentResult result;
    /* Ноль шагов сюда не доходит, и предохранителя на него нет: лимит
     * сессии задаёт load_settings и он не ниже единицы
     * (`steps > 0 ? steps : 12`), а `steps` агента проверяется на
     * положительность до вызова. Строка «на всякий случай» была бы
     * недостижимым кодом (тот же класс, что снятая проверка в И7.1). */

    /* История ребёнка — ЛОКАЛЬНАЯ. Родительский state_.session не
     * читается и не пишется: ход субагента не часть диалога (его
     * результатом станет одна строка в вызове `task` — 8.11). Всё, что
     * о ней узнаёт вызывающий, — в result.history (И8.9): дочерняя
     * сессия пишется из этого поля, и второй путь к истории ребёнка
     * означал бы, что сохраняется не та. */
    std::vector<Message> history = std::move(seed);
    history.push_back(Message::user(prompt));
    /* Корень — ПЕРВОЕ сообщение истории, а не только что добавленное:
     * при продолжении (И8.9) это корень исходной задачи, иначе в одном
     * файле сессии оказались бы два разговора. */
    const std::string root_id = history.front().id;

    for (int step = 0; step < max_steps; ++step) {
        if (state.abort_requested.load()) {
            result.error = "[прервано пользователем] субагент остановлен";
            break;
        }
        ++result.steps;

        /* И10.1: тот же снимок в начале шага, что и в основном цикле, и
         * той же функцией. Отдельная точка обязательна: ход субагента
         * ходит по СВОЕМУ циклу (функция, а не AgentLoop), и без этой
         * строки субагент, а значит и автор WP-правок (8.15), оставлял
         * бы файлы без снимка — то есть «откат работает» было бы верно
         * только для агента сессии. */
        take_step_snapshot(state, cb, push_event);

        std::vector<LlmEvent> events;
        const bool ok = llm_source::fetch(
            cb, sys_prompt, to_model_messages(history), events);
        LlmResponse response = llm_source::fold(events);
        /* Токены ребёнка — токены сессии: они оплачены тем же запросом и
         * занимают то же окно. Метрики берутся из usage того же события
         * Finish, что и у родителя (И7.2). */
        record_turn_usage(state, response.usage());

        /* И8.12: отмена, случившаяся ВО ВРЕМЯ запроса, — это отмена, а не
         * сбой провайдера. Проверка стоит ДО разбора ответа, потому что
         * после отмены провайдер почти всегда вернёт ошибку, и модель (и
         * человек в ленте событий) прочитали бы «провайдер не ответил» там,
         * где человек нажал «стоп». Тот же флаг, что и между шагами, и
         * ждать его не надо: ожидание блокирующего вызова отменяемо и у
         * ребёнка, и у родителя (предикат один — state_.abort_requested). */
        if (state.abort_requested.load()) {
            result.error = "[прервано пользователем] субагент остановлен";
            break;
        }
        if (!ok || response.empty()) {
            /* Два разных отказа и два разных текста: «провайдер не
             * ответил» (ok == false) и «ответил пустым» (ok == true, но
             * свернулось ни текста, ни вызовов, ни ошибки). Второй был бы
             * выглядеть как успешный субагент, который сказал «делать
             * нечего» (И6.8). */
            result.error = "[ошибка] субагент " + state.scope.agent + ": " +
                (!ok ? (response.error().empty() ? "провайдер не ответил"
                                                : response.error())
                     : std::string("провайдер ответил пустым ответом"));
            break;
        }

        Message turn = turn_to_message(response, root_id);
        history.push_back(turn);
        /* Ход кладётся в историю ССЫЛКОЙ-КОПИЕЙ, а состояние его частей
         * меняется на локальном `turn`. Поэтому после вызовов локальный
         * ход копируется НАЗАД — по идентификатору, как это делает цикл
         * родителя (commit_turn). Без этого запрос к модели ушёл бы с
         * частью-вызовом в состоянии «работает» и без результата: модель
         * получила бы вызов, на который не последовало ответа, и
         * повторяла бы его. */
        const auto commit_turn = [&history, &turn](void) {
            if (Message* stored = find_message(history, turn.id)) {
                *stored = turn;
            }
        };

        bool aborted = false;
        const size_t call_count = response.tool_calls().size();
        for (size_t i = 0; i < call_count; ++i) {
            const LlmToolCall call = response.tool_calls()[i];
            if (!call.runnable()) continue;
            if (MessagePart* part = find_tool_part(turn, call.call_id)) {
                part->set_running();
            }
            /* Тот же ToolRunner и та же точка enforcement: у ребёнка
             * решения принимают его правила (RunScope, И8.7), у
             * родителя — правила сессии. Отдельный путь для субагента
             * означал бы второе место, где решается «можно ли». */
            ToolRunner runner(state, cb, push_event);
            const ToolOutcome outcome = runner.run(call.name, call.arguments);
            if (outcome.ok) {
                LlmResponse::reduce(response, LlmEvent::tool_result(
                    call.call_id, cap_result(std::move(outcome.output))));
            } else {
                LlmResponse::reduce(response,
                    LlmEvent::tool_error(call.call_id, outcome.error));
            }
            sync_tool_parts(turn, response);
            if (state.abort_requested.load()) {
                aborted = true;
                break;
            }
        }

        /* Вызовы, до которых не дошли (обрыв на отмене), закрываются
         * отказом: незакрытая часть означала бы, что ход нельзя
         * закончить, и следующий запрос ушёл бы с незакрытым вызовом. */
        for (size_t i = 0; i < response.tool_calls().size(); ++i) {
            if (response.tool_calls()[i].finished) continue;
            LlmResponse::reduce(response, LlmEvent::tool_error(
                response.tool_calls()[i].call_id,
                "вызов не выполнен: субагент остановлен"));
        }
        sync_tool_parts(turn, response);
        commit_turn();

        if (aborted) {
            result.error = "[прервано пользователем] субагент остановлен";
            break;
        }

        /* Провайдер закончил не затем, чтобы звать инструменты, и всё
         * закрыто — ход субагента закончен.
         *
         * Отдельной проверки «текст непуст» здесь нет, и она была бы
         * мёртвой строкой: пустой текст при отсутствии вызовов и ошибки
         * означает `response.empty()`, а этот случай разобран выше. Там,
         * где свернулось только РАЗМЫШЛЕНИЕ, ответа действительно нет —
         * но такой ответ невозможен через HostCallbacks (в LlmReply нет
         * поля размышления, И6.1), и когда появится, его разберут здесь
         * же, а не в ToolRunner. */
        if (response.finish_reason() != "tool-calls" && !turn.has_open_tool_part()) {
            result.ok = true;
            /* И8.11: результат достаётся из ХОДА (Message::task_answer), а
             * не из ответа провайдера. Ответ — это то, что прислал
             * провайдер, а ход — то, что осталось после выполнения
             * вызовов; на финальном шаге, где модель только позвала
             * хелпер, это разные вещи, и «пусто у провайдера» не значит
             * «нечего сказать». */
            result.text = turn.task_answer();
            break;
        }
    }
    if (!result.ok && result.error.empty()) {
        result.error =
            "[ошибка] субагент " + state.scope.agent +
            " не закончил задачу за " + std::to_string(max_steps) +
            " шагов и не дал итога. Попроси его о конкретном "
            "результате или сделай работу сам.";
    }
    /* Единственный выход (И8.9): история ребёнка отдаётся ЛЮБЫМ исходом,
     * и обеспечено это здесь, а не в шести точках возврата. */
    result.history = std::move(history);
    return result;
}

/* ======================================================================
 * main
 * ====================================================================== */

} // namespace coder

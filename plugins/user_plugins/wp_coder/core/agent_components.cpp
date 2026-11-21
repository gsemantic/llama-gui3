// agent_components.cpp (Фаза D1) — реализация компонентов без goto.
// И5.7: цикл работает с событиями, история — со сообщениями с частями.

#include "agent_components.h"
#include "prompts.h"
#include "security.h"
#include "shell.h"
#include "engine.h"
#include "limits.h"
#include "json_utils.h"
#include "llm_source.h"
#include "llm_client.h"
#include "snapshot.h"
#include "tool_repair.h"

#include <sstream>
#include <vector>
#include <set>
#include <map>
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
/* Порог серии read-only ходов без изменений: после него цикл
 * напоминает модели перейти к действию (ses_496: 25 чтений подряд). */
constexpr int kExploreRemindAt = 8;
/* Сколько раз можно прочитать ОДИН файл, прежде чем цикл потребует
 * перейти к делу. В ses_000000000006 модель читала library.py шесть раз
 * с разными offset/limit, и каждый раз это был шаг, а не работа. */
constexpr int kMaxReadsPerFile = 4;

namespace {

/* Каноническая подпись аргументов: учитываются только параметры из схемы
 * инструмента, ключи сортируются. Это важно, потому что модель добавляет
 * служебные поля (`"tool":"read_file"`) и меняет порядок ключей: с сырым
 * dump() вызовы с `{"path":"f"}` и `{"path":"f","tool":"read_file"}`
 * считались разными, и защита от повторов их пропускала. */
std::string canonical_args(const json::JsonValue& args,
                           const json::JsonValue* schema) {
    std::vector<std::string> keys;
    if (schema && schema->is_object() && schema->has("properties")) {
        const json::JsonValue& props = schema->get("properties");
        if (props.is_object()) keys = props.keys();
    }
    if (keys.empty()) keys = args.keys();
    std::sort(keys.begin(), keys.end());
    std::string out = "{";
    bool first = true;
    for (const std::string& key : keys) {
        if (!args.has(key)) continue;
        if (!first) out += ",";
        first = false;
        out += json::JsonValue(key).dump();
        out += ":";
        out += args.get(key).dump();
    }
    out += "}";
    return out;
}

/* Путь из аргументов вызова — так, как его видит модель (относительный).
 * Отдельно от canonical_args: там подпись нужна ПОЛНАЯ (все параметры),
 * а здесь — только путь, чтобы считать чтения одного файла. Пустая
 * строка = вызова без path (например, list), и такой вызов в счёт
 * чтений не попадает. */
std::string canonical_args_path(const json::JsonValue& args) {
    if (!args.is_object()) return std::string();
    std::string path = args.get_string("path");
    if (path.empty()) path = args.get_string("file");
    return path;
}

} // namespace
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
    if (!security::is_path_outside(abs_path, proj)) return "";
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        if (security::is_path_allowed(abs_path, proj, state_.allowed_external_paths)) return "";
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
        allowed = security::is_path_allowed(path, project, state.allowed_external_paths);
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

ToolOutcome ToolRunner::run(const std::string& requested_name,
                            const json::JsonValue& args) {
    /* И12.3: починка имени ДО исполнения и до отпечатка зацикливания.
     *
     * Здесь, а не в адаптере ответа, потому что имя приходит двумя
     * путями — нативный вызов дельтами и текстовый блок через
     * extract_action — и починка обязана работать для обоих. Точка одна.
     */
    const ToolRepair repair = repair_tool_call(requested_name);
    if (repair.routed_to_invalid) {
        /* РЕЗУЛЬТАТ ВЫЗОВА, А НЕ ОТКАЗ. Раньше здесь был ToolError, то
         * есть протокольная ошибка чужого слоя: у вызова не было
         * результата, и модель вместо правки имени обычно начинала
         * рассуждать заново, платя за опечатку всем накопленным
         * контекстом. Теперь модель читает в своём обычном канале
         * результатов, какого инструмента нет, и переписывает вызов.
         *
         * ok=true — иначе AgentLoop отправил бы ToolError вместо
         * ToolResult, и весь смысл починки вернулся бы на кругу.
         *
         * Событие в ленте — AgentEvent::Tool, а не Error: починка
         * отработала, ошибки нет. */
        ToolOutcome outcome;
        outcome.ok = true;
        outcome.output = invalid_tool_output(repair);
        this->push_event_(AgentEvent::Tool,
            (repair.requested + " -> неизвестный инструмент: "
             + outcome.output.output).c_str());
        return outcome;
    }

    /* Дальше по всему пути — ПОЧИНЕННОЕ имя. Иначе ключ разрешения,
     * отпечаток зацикливания и подпись в ленте говорили бы про «Read»,
     * которого в реестре нет, а политика режимов искала бы инструмент
     * по имени, которого не существует. */
    const std::string& tool_name = repair.tool;
    const ToolDef& def = *repair.def;

    /* И1.7: enforcement режимов — ДО fingerprint и ДО вызова.
     * Это единственное место, где решается, допустим ли инструмент.
     * Лок отпускаем до push_event_ (state_.mtx нерекурсивный). */
    std::string refusal;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        refusal = check_tool_mode_policy(tool_name, def, state_.mode, state_.plan_mode);
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
    const std::string perm_key = permission_key_of(def);
    const std::string perm_pattern = permission_pattern(def, args);
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
        const std::string suggested = permission_suggested_pattern(def, perm_pattern);
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

/* Instructions for the planning turn are in ENGLISH for the same reason as
 * the execution contract: the model family used in the failing sessions
 * (particle-1 behind the kilo gateway, DeepSeek-style safety wrapper)
 * follows English imperatives more reliably than Russian ones. The plan
 * itself is shown to the user in whatever language the user wrote in; only
 * the instruction is English. */
const char* kPlanPrompt =
    "Write a short plan for the task. Rules: no tools, no wp_action, no "
    "code. At most 5 numbered steps, one line each, imperative. Do not "
    "mention files, frameworks or versions you have not verified. The plan "
    "will be executed with tools afterwards, so describe actions, not facts. "
    "The last step MUST be verification: run the project's tests (or create "
    "and run a minimal smoke test) and report the result.";

} // namespace

/* Учёт токенов хода (И7.2) — тело ниже, объявление в engine.h.
 *
 * ОДНО место на три точки вызова — планировщик, цикл и хвостовой ход.
 * Списком по три строки в каждой точке они и раньше дублировались, и
 * добавление четвёртого счётчика (measured_input_tokens) в трёх местах
 * рано или поздно разъехалось бы: забыть — значит компакшн получит
 * устаревшее измерение и будет считать контекст по чужому запросу.
 *
 * Лок берётся внутри — вызывающие цикла лока НЕ держат (state_.mtx
 * нерекурсивный, общий с UI: та же причина, что у permission_outcome).
 * Объявление функции — в engine.h: единственный способ проверить, что
 * счётчики копятся, — звать их на живом состоянии, а цикл требует LLM.
 *
 * Считается именно input, а не total: контекстом является то, что ушло
 * В модель, а ответ следующего шага будет другим. */
void record_turn_usage(EngineState& state, const Usage& usage) {
    std::lock_guard<std::mutex> lk(state.mtx);
    state.total_prompt_tokens += static_cast<int>(usage.input);
    state.total_completion_tokens += static_cast<int>(usage.output);
    /* И11.9: копятся и остальные три счётчика. До этого задачи они
     * терялись: провайдер их присылал, а состояние их не хранило, и
     * панель токенов показала бы input/output и молчала бы про кэш —
     * то есть человек не увидел бы, сколько запросов было дешёвыми.
     *
     * Порядок сложения неважен, порядок СБРОСА важен: сбрасываются все
     * пять в `Engine::reset_task_metrics` (engine.cpp), иначе после новой
     * задачи панель сложила бы старый кэш с новым входом. */
    state.total_reasoning_tokens += usage.reasoning;
    state.total_cache_read_tokens += usage.cache_read;
    state.total_cache_write_tokens += usage.cache_write;
    if (usage.input > 0) state.measured_input_tokens = usage.input;
}

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
    /* И11.14: planner идёт через LlmClient, а не напрямую (D23). Раньше
     * план был самым «долгим» запросом без отмены: он идёт до модели
     * один раз за задачу, и «стоп» на нём ждал бы полный таймаут.
     * Токен берётся под локом, потому что он СВОЙ у хода (И6.7) и
     * читать его без лока — гонка с новым ходом. */
    AbortToken* plan_abort = nullptr;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        plan_abort = state_.turn_abort.get();
    }
    const bool plan_ok =
        LlmClient::fetch(cb_, sys_prompt, plan_msgs, events,
                         nullptr, -1, plan_abort);
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
        /* План — это рассуждение, а не ответ пользователю. В чате он
         * уходит статусом (обрезанным), целиком остаётся в окне «Сессия».
         * Так итоговый ответ не смешивается с планом. */
        this->push_event_(AgentEvent::Status, "План: " + plan.text());
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
ToolOutput cap_result(ToolOutput out, size_t budget) {
    if (budget == 0) budget = limits::kResultBudget;
    if (out.output.size() <= budget) return out;
    const size_t full = out.output.size();
    out.output = text::utf8_prefix(out.output, budget);
    out.output += "\n[...обрезано, всего " + std::to_string(full) +
                  " символов. Вызови инструмент повторно, если нужно больше.]";
    return out;
}

/* Бюджет RESULT для текущего состояния — И14.3. Читает лимит окна модели
 * через `Engine::model_context_limit()` (короткий лок внутри) и масштабирует
 * `kResultBudget` от него. Если окно не задано (0) — прежнее значение. */
size_t result_budget_for() {
    return limits::result_budget_for(
        Engine::instance().model_context_limit());
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
    "This is the LAST step. You may NOT call tools any more: the step budget "
    "is exhausted and your next message goes directly to the user. Write the "
    "final report now: what was done, what was NOT done, and why. Do not "
    "write a plan.\n"
    "Do NOT hand the work back to the user. \"Please fix it manually\", "
    "\"check it yourself\", \"the rest is up to you\" are forbidden: the "
    "task is yours. If something is blocked, name the exact permission or "
    "rule you need; if something was not done, say plainly what and why.";

/* Пустой JSON-объект (`{}`), который модель иногда присылает вместо
 * вызова инструмента. Это не ответ и не пустой ход провайдера: нативные
 * инструменты отключать из-за него нельзя, а в историю его писать
 * бессмысленно — он не добавляет модели ничего. */
bool is_empty_json_object(const std::string& text) {
    const size_t b = text.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return false;
    const size_t e = text.find_last_not_of(" \t\r\n");
    const std::string t = text.substr(b, e - b + 1);
    return t == "{}" || t == "[]" || t == "null";
}

} // namespace

/* И10.1: снимок рабочего каталога в начале шага.
 *
 * Порядок здесь не косметический, и все три пункта — про блокировки:
 *
 *   1. Каталог проекта и предыдущая причина неудачи читаются под
 *      state.mtx и копируются — дальше лок не держится.
 *   2. Сам снимок берётся БЕЗ лока: внутри работает git (до
 *      limits::kSnapshotTimeoutSec) или копируется каталог, а UI ждёт
 *      тот же мьютекс. Лок на этом месте означал бы висящий GUI.
 *   3. Результат кладётся под локом — короткая запись.
 *
 * И10.4: тем же вызовом состояние становится УРОВНЕМ сессии. Это один
 * вызов, а не два, потому что уровень и есть снимок шага: второй вызов
 * снял бы то же состояние вторым уровнем, и «отменить» пришлось бы жать
 * вхолостую. Копии, вытесненные из стека, удаляются ПОСЛЕ лока: под ним
 * удалять каталоги нельзя (UI ждёт тот же мьютекс). */
StepSnapshot take_step_snapshot(EngineState& state, HostCallbacks& cb,
                                AgentEventCallback push_event) {
    StepSnapshot out;
    std::string project_dir;
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        project_dir = state.project_dir;
        out.before = state.last_snapshot;
    }

    const std::string data_dir = cb.path_data_dir ? cb.path_data_dir() : "";
    const std::string store = snapshot::store_dir(data_dir);
    out.after = snapshot::take(project_dir, store);

    std::vector<snapshot::Snapshot> trash;
    bool became_level = false;
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.last_snapshot = out.after;
        if (out.after.ok()) ++state.snapshots_taken;
        became_level = state.undo_stack.push(out.after);
        trash = state.undo_stack.take_trash();
    }
    /* Снимок, который НЕ стал уровнем, убирается здесь же: на проекте
     * без git это полная копия каталога, которую не читает ни undo, ни
     * redo и которая иначе осталась бы на диске навсегда. Шаг без единой
     * правки (агент ответил текстом) был обычным делом, то есть утечка
     * шла на каждом таком шаге всей сессии. */
    if (out.after.ok() && !became_level) out.discard.push_back(out.after);
    snapshot::discard_copies(trash);

    /* Пустой каталог проекта — не отказ снимка, а отсутствие проекта:
     * об этом панель говорит отдельно, и второе сообщение на каждом
     * шаге было бы только шумом. */
    if (!out.after.ok() && !project_dir.empty() &&
        out.after.reason != out.before.reason && push_event) {
        push_event(AgentEvent::Status,
                   "Снимок рабочего каталога не снят: " + out.after.reason);
    }
    return out;
}

Message* attach_patch_part(std::vector<Message>& history,
                           const snapshot::Snapshot& before,
                           const snapshot::FileChanges& changes) {
    if (!changes.ok || changes.files.empty()) return nullptr;
    /* Последнее сообщение ассистента, а не последнее сообщение вообще:
     * список изменений принадлежит тому ходу, чьи инструменты их
     * сделали, и приписанный к чужой реплике он означал бы «это
     * изменилось, пока я говорил». */
    Message* target = nullptr;
    for (Message& m : history) {
        if (m.is_assistant()) target = &m;
    }
    if (!target) return nullptr;
    json::JsonValue files = json::JsonValue::array();
    for (const std::string& f : changes.files) {
        files.push_back(json::JsonValue(f));
    }
    /* Хеш — тот, С КОГОГО считали, а не тот, который получился: вернуться
     * надо к состоянию до изменений этого хода. */
    target->parts.push_back(MessagePart::patch(before.hash, std::move(files)));
    return target;
}

namespace {

/* Первый сбалансированный {…} из начала текста, без хвоста (</rfl:parameter>). */
std::string first_json_object(const std::string& text) {
    const size_t b = text.find_first_not_of(" \t\r\n");
    if (b == std::string::npos || text[b] != '{') return std::string();
    size_t i = b + 1;
    int depth = 1;
    bool in_str = false;
    while (i < text.size() && depth > 0) {
        const char c = text[i];
        if (in_str) {
            if (c == '\\' && i + 1 < text.size()) { i += 2; continue; }
            if (c == '"') in_str = false;
        } else {
            if (c == '"') in_str = true;
            else if (c == '{') ++depth;
            else if (c == '}') --depth;
        }
        ++i;
    }
    if (depth != 0) return std::string();
    return text.substr(b, i - b);
}

/* Если текст — JSON-аргументы READ-ONLY инструмента (grep_search/read_file),
 * выполняем его за модель и кладём результат в out_note. Лечит зацикливание
 * «модель печатает вызов текстом» (ses_000000000542): вместо «позови
 * инструмент» модель получает готовый результат и идёт дальше. */
bool try_infer_readonly_call(const std::string& text, std::string& out_note) {
    const std::string body = first_json_object(text);
    if (body.empty()) return false;
    json::JsonValue j;
    std::string perr;
    if (!json::JsonValue::parse(body, j, &perr) || !j.is_object()) return false;

    const auto has = [&j](const char* k) { return j.has(k); };
    std::string tool;
    json::JsonValue args = json::JsonValue::object();
    if (has("pattern") && (has("root") || has("path")) && !has("content")) {
        tool = "grep_search";
        args.set("pattern", j.get("pattern"));
        args.set("root", has("root") ? j.get("root") : j.get("path"));
    } else if (has("path") && !has("content") && !has("query")) {
        tool = "read_file";
        args.set("path", j.get("path"));
        if (has("offset")) args.set("offset", j.get("offset"));
        if (has("limit")) args.set("limit", j.get("limit"));
    }
    if (tool.empty()) return false;

    const ToolOutput r = ToolsRegistry::instance().run_output(tool, args);
    out_note = "[выполнил за тебя " + tool + "] " +
               text::utf8_prefix(r.output, 800);
    return true;
}

} // namespace

bool AgentLoop::run(const std::string& sys_prompt, std::string& full_response) {
    bool final_given = false;
    int stuck_counter = 0;  // последовательных коротких ответов
    /* Пустые ответы провайдера — отдельный счётчик: выключение нативных
     * инструментов (первый пустой ход) и «сдаться» (второй подряд) —
     * разные решения, и общий счётчик коротких ответов их бы смешал. */
    int empty_streak = 0;
    int reasoning_only_streak = 0;
    int truncated_streak = 0;
    int planning_streak = 0;
    int tool_args_streak = 0;
    /* Подряд идущие read-only ходы без единой правки/выполнения. */
    int read_only_streak = 0;
    /* Гейт проверки: просили ли уже запустить тесты (один раз). */
    bool verification_reminded = false;
    /* Гейт передачи работы пользователю: просили ли уже сделать её
     * самому (один раз). Второй такой отчёт принимаем, иначе цикл
     * вымогал бы невозможное и жёг бюджет. */
    bool handoff_reminded = false;
    /* И15.4: были ли в этой задаче успешные ПИШУЩИЕ вызовы. Отличается от
     * `has_unfinished_todos()`: todowrite зовут не все модели, и «правки
     * есть, а итога нет» — это тоже повод продлить бюджет, а не закрывать
     * задачу. Флаг ставится при первом успешном write/edit/apply_patch. */
    bool made_write = false;
    /* Отпечаток последнего выполненного read-only вызова. Нужен, чтобы
     * НЕ выполнять повторное чтение того же файла с теми же аргументами:
     * в сессии 0.18.16 один и тот же read_file вызывался 6–9 раз подряд,
     * каждый повтор стоил шага, раздувал контекст, а на третьем срабатывал
     * вопрос doom_loop — человек ждал его десятки секунд. Любой пишущий
     * или исполняющий вызов сбрасывает отпечаток: после правки файл мог
     * измениться, и повторное чтение законно. */
    std::string last_read_sig;
    /* Подписи вызовов, которые инструмент признал no-op (`[no-op]` в
     * результате). Если модель повторит любую из них — не выполняем
     * второй раз: в сессии 0.18.18 так зацикливались search_replace и
     * edit_file с query == content, а в ses_000000000006 модель
     * ЧЕРЕДОВАЛА no-op вызовы (search_replace ↔ write_file), и одиночный
     * last_noop_sig каждый раз забывал предыдущий.
     *
     * Набор чистится при первой НАСТОЯЩЕЙ правке: после неё повтор
     * любого из старых вызовов может быть осмысленным. */
    std::set<std::string> nooped_calls;
    /* Сколько раз модель прочитала ОДИН файл. В ses_000000000006 она
     * читала library.py шесть раз с разными offset/limit, ища нужный
     * фрагмент, который уже был в истории: защита от повторов ловит
     * только СОВПАДАЮЩИЕ аргументы, а смена offset её обходит.
     *
     * Ключ — канонический путь, значение — число чтений. Сбрасывается
     * записью в этот же файл: после правки перечитать его законно. */
    std::map<std::string, int> reads_per_file;
    /* Системный промпт пересобирается на каждом шаге: note_tools_rejected
     * меняет протокол вызова, и старый снимок заставил бы модель писать
     * блок, которого приложение уже не ждёт. */
    std::string sys = sys_prompt;
    /* Runtime-порог итога: профиль может его менять (limit_min_final_answer_len).
     * Раньше здесь была константа, и поле профиля никто не читал. */
    size_t min_final_len = limits::kMinFinalAnswerLen;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        min_final_len =
            static_cast<size_t>(state_.limit_min_final_answer_len);
    }

    /* Мягкий бюджет: если к последнему шагу план задачи закрыт не
     * полностью (в todowrite есть pending/in_progress), цикл один раз
     * продлевается на тот же лимит. Причина — сессия 0.18.16: задача на
     * пять файлов упиралась в 20 шагов ровно на последней проверке, и
     * человеку приходилось вручную писать «продолжай», вставляя ошибку
     * запуска. Продление ровно одно, поэтому задача не может крутиться
     * вечно; жёсткий предохранитель (2 * max_steps) остаётся.
     *
     * Если плана нет (todos пуст), поведение прежнее: последний шаг
     * получает kLastStepReminder и обязан дать итог. */
    const auto has_unfinished_todos = [this]() {
        std::lock_guard<std::mutex> lk(state_.mtx);
        for (const TodoItem& t : state_.todos) {
            if (t.status == "pending" || t.status == "in_progress") return true;
        }
        return false;
    };
    int total_steps = state_.max_steps;
    /* Продлеваем бюджет не один раз, а два: задача на несколько файлов
     * часто упирается в лимит ровно на проверке, и один раз «продолжай»
     * не спасал — агент писал отчёт «сделайте сами». Жёсткий предел —
     * 3 * max_steps, дальше цикл обязан дать итог. */
    int budget_extensions = 0;
    /* Вырожденные ходы (`{}` вместо вызова) не должны съедать бюджет:
     * они не пишутся в историю и не приближают результат. Компенсируем
     * их отдельным счётчиком, чтобы цикл реально получил свои
     * total_steps продуктивных шагов. */
    int free_retries = 0;

    std::cerr << "[wp_coder] agent_loop: starting, max_steps=" << total_steps << std::endl;

    for (int step = 0; step < total_steps + free_retries; ++step) {
        const int effective_step = step - free_retries;
        if (state_.abort_requested.load()) {
            this->push_event_(AgentEvent::Status, "[прервано пользователем]");
            full_response += "\n\n[прервано пользователем]";
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.session.clear();
            }
            return true;
        }

        /* Протокол мог смениться на прошлом шаге (пустой ответ → без
         * нативных инструментов). Кэш делает пересборку дешёвой: без
         * изменений build_system_prompt вернёт тот же текст. */
        sys = engine().build_system_prompt();

        /* И10.1: снимок рабочего каталога в начале шага. Раньше
         * откатывать правки агента было нечем: undo_edit держит ОДИН слот
         * .orig на файл, и вторая правка того же файла затирала первую.
         * Снимок снимается ДО запроса к модели — то есть до того, как
         * агент узнает, что ему делать, и состояние на снимке принадлежит
         * началу шага, а не его результату. */
        const StepSnapshot shots =
            take_step_snapshot(state_, cb_, this->push_event_);
        /* И10.2: что изменилось между снимком ПРЕДЫДУЩЕГО шага и этим —
         * это и есть работа предыдущего хода, и список кладётся в его
         * сообщение.
         *
         * Лок берётся только на запись части и НИКОГДА на время
         * сравнения: changed_files зовёт git (до kSnapshotTimeoutSec), а
         * UI ждёт тот же мьютекс. Это ровно то, чего нельзя делать в
         * take_step_snapshot, и оба места стоят рядом специально —
         * чтобы правка одного не превратила другое в висящий UI. */
        const snapshot::FileChanges changes =
            snapshot::changed_files(shots.before, shots.after);
        if (changes.ok) {
            std::lock_guard<std::mutex> lk(state_.mtx);
            attach_patch_part(state_.session, shots.before, changes);
        }
        /* Копия снимка, которая не стала уровнем, убирается ПОСЛЕ
         * сравнения: `after` нужен былchanged_files выше, и удаление
         * раньше молча дало бы «изменений нет» при полном списке
         * сделанного. */
        snapshot::discard_copies(shots.discard);

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
        const bool is_last_step = (effective_step + 1 >= total_steps);
        if (is_last_step && step > 0) {
            /* План не закрыт — даём ещё один бюджет вместо принудительного
             * итога. Продление сообщается модели явно: иначе она увидит
             * «продолжай» без причины и снова начнёт с осмотра проекта.
             *
             * И15.4: условие расширено с «есть незакрытые todos» на «есть
             * незакрытые todos ИЛИ были правки». Причина: todowrite зовут
             * не все модели, и задача «сделано 3 файла из 5» без плана
             * раньше упиралась в жёсткий итог. `made_write` — факт, что
             * работа шла, а не догадка. */
            if (budget_extensions < 2 &&
                (has_unfinished_todos() || made_write)) {
                ++budget_extensions;
                total_steps += state_.max_steps;
                this->push_event_(AgentEvent::Status,
                    "Бюджет шагов исчерпан, но работа ещё идёт (правки есть"
                    "/план не закрыт) — продлеваю на " +
                    std::to_string(state_.max_steps) +
                    " шагов и продолжаю (продление " +
                    std::to_string(budget_extensions) + " из 2).");
                append_note(
                    "[reminder] The step budget was extended because the "
                    "task is not finished yet (there are already edits, or "
                    "the plan still has unfinished items). Do NOT re-read "
                    "files you have already read and do NOT repeat the plan. "
                    "Continue with the NEXT tool call needed to finish the "
                    "task. Send the final report only after every todo is "
                    "completed or cancelled.");
            } else {
                append_note(kLastStepReminder);
            }
        }

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
        /* И11.14: ход агента идёт через LlmClient (D23). Именно этот зов
         * делал отмену фикцией: цикл проверял abort_requested только МЕЖДУ
         * шагами, и «стоп» посреди шага ждал ответа провайдера до конца —
         * кнопка «стоп» на самом долгом месте работы не работала ни разу.
         *
         * Токен читается под локом: он СВОЙ у хода, и следующий ход
         * заменит указатель, пока этот ещё ждёт. */
        AbortToken* step_abort = nullptr;
        int step_no = 0;
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            step_abort = state_.turn_abort.get();
            step_no = step + 1;
        }
        /* И11.6: живая доставка дельт в панель стриминга. До И11.6 здесь
         * стоял `nullptr`, то есть дельты вычислялись LlmClient'ом и уходили
         * в никуда: канал появился в И11.14, а потребителя у него не было.
         *
         * Колбэк берёт `state_.mtx` на каждой дельте. Это короткая
         * операция (склейка строки под пределом) и единственное место, где
         * UI-поток и поток агента встречаются, поэтому блокировка взята
         * ОДИН раз на доставку, а не на весь ход: держать её минуту
         * означало бы повесить окно на всё время ответа модели. */
        auto on_event = [this, step_no](const LlmEvent& e) {
            if (e.kind() == LlmEventKind::StepStart) {
                std::lock_guard<std::mutex> lk(state_.mtx);
                stream_panel::feed(state_.stream, e);
                state_.stream.step = step_no;
                return;
            }
            std::lock_guard<std::mutex> lk(state_.mtx);
            stream_panel::feed(state_.stream, e);
        };
        const bool ok = LlmClient::fetch(cb_, sys, model_msgs, events,
                                         on_event, -1, step_abort);
        {
            /* Ход закрылся — панель гаснет. Буфер очищается НЕ сразу: пока
             * цикл не записал ход в историю, текст из буфера — единственное
             * место, где он ещё виден. `finish_turn` только снимает признак
             * активности, содержимое переживает до следующего StepStart. */
            std::lock_guard<std::mutex> lk(state_.mtx);
            stream_panel::finish_turn(state_.stream);
        }
        auto t1 = std::chrono::steady_clock::now();
        const double llm_s =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;

        /* Свёртка событий — единственное, что цикл делает с ответом.
         * Дальше он работает с текстом, вызовами, finish и метриками, а
         * не с блоками и префиксами: до И5.7 здесь был разбор строки
         * extract_action/parse_action и склейка «RESULT [x]:». */
        LlmResponse response = llm_source::fold(events);

        std::cerr << "[wp_coder] step " << (step+1) << " response: text_len=" << response.text().size()
                  << " reasoning_len=" << response.reasoning().size()
                  << " tool_calls=" << response.tool_calls().size()
                  << " finish=" << response.finish_reason() << std::endl;

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
         * получила бы пустую историю и повторила бы тот же запрос.
         *
         * И11.14: ОТМЕНА — не сбой, и разбирается ДО общей ветки. Иначе
         * «стоп» печатал бы «[ошибка] LLM: Прервано пользователем» и
         * уходил с признаком сбоя: человек нажал «стоп», а ему сообщили
         * про провайдера. Ровно то, что И6.8 разводит терминально, и
         * вид сбоя Aborted для этого и заведён. */
        if (!ok && response.failure() == FailureKind::Aborted) {
            this->push_event_(AgentEvent::Status, "[прервано пользователем]");
            full_response += "\n\n[прервано пользователем]";
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.session.clear();
                state_.state = AgentState::Aborted;
            }
            return true;
        }
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

        /* ПУСТОЙ ХОД (ни текста, ни вызова) обрабатывается ДО записи в
         * историю: пустое сообщение ассистента ничего не добавляет модели,
         * но занимает место и сбивает условие завершения.
         *
         * Наблюдение (qwen-flash, DashScope, 2026-11): на запрос, где
         * отправлены tools, провайдер отвечает 200, finish=stop и пустым
         * content — без единой ошибки. Планировщик при этом отвечает
         * нормально: его последняя реплика — «составь план без вызова
         * инструментов», и модель не пытается позвать инструмент. То есть
         * ломается именно НАТИВНЫЙ вызов, а не провайдер и не промпт.
         *
         * Поэтому первый пустой ход не считается ошибкой модели: нативные
         * инструменты выключаются (note_tools_rejected), и следующий шаг
         * идёт текстовым протоколом (блок wp_action). Если и он пуст —
         * провайдер отвечает пустотой всегда, и два хода подряд честно
         * закрывают задачу с ошибкой, а не тратят три шага на напоминания. */
        /* `{}` вместо вызова — вырожденный ход. Не considered empty_turn:
         * провайдер жив, нативные инструменты отключать не за что. Но и
         * в историю это писать нельзя: модель не получит ничего, кроме
         * собственного пустого объекта, и следующий шаг начнётся с него.
         * Шаг не списываем (free_retries), бюджет остаётся на действия. */
        const bool blank_json_turn =
            !response.text().empty() && response.tool_calls().empty() &&
            is_empty_json_object(response.text());
        if (blank_json_turn) {
            /* Не больше пяти бесплатных повторов: если модель шлёт `{}` без
             * конца, дальше это уже её бюджет, а не наш. */
            const int max_free = std::min(total_steps, 5);
            if (free_retries < max_free) ++free_retries;
            this->push_event_(AgentEvent::Status,
                "Модель прислала пустой JSON {} вместо вызова — шаг не "
                "списан, прошу вызвать инструмент.");
            append_note(
                "[reminder] You sent an empty JSON object ({}) instead of a "
                "tool call. Do NOT send {}. Make the next tool call now (one "
                "wp_action block / native call), or, if the task is finished, "
                "write the final report.");
            continue;
        }

        const bool empty_turn =
            response.text().empty() && response.tool_calls().empty();
        if (empty_turn) {
            /* Диагностика: пустой ход при НЕнулевом usage означал бы, что
             * токены провайдер посчитал, а текст не отдал — то есть ответ
             * уехал в канал, который хост не доставляет (например,
             * reasoning_content). */
            std::cerr << "[wp_coder] пустой ход: finish="
                      << response.finish_reason()
                      << " reasoning_len=" << response.reasoning().size()
                      << " usage=" << usage.input << "+" << usage.output
                      << " tok" << std::endl;
            if (!response.reasoning().empty()) {
                /* Модель думает, но не действует. Это не «провайдер сломан»:
                 * выключать инструменты не за что. */
                ++reasoning_only_streak;
                if (reasoning_only_streak >= 2) {
                    const std::string err =
                        "модель дважды подряд прислала только размышление, "
                        "без вызова инструмента и без ответа";
                    this->push_event_(AgentEvent::Error, "[ошибка] " + err);
                    full_response = full_response.empty()
                        ? "[ошибка] " + err : full_response;
                    return false;
                }
                this->push_event_(AgentEvent::Status,
                    "Только размышление, без действия — прошу вызвать "
                    "инструмент или дать итог.");
                /* Reminders are model-facing only (append_note is not shown
                 * in the chat), so they are in English: the model family in
                 * the failing sessions follows English imperatives more
                 * reliably. The status line above stays Russian for the
                 * user. */
                append_note(
                    "[reminder] You sent reasoning only. Call the next tool "
                    "now (one wp_action block), or, if the task is truly "
                    "finished, write the final report of at least " +
                    std::to_string(min_final_len) + " characters.");
                continue;
            }

            reasoning_only_streak = 0;
            if (engine().native_tool_support() != NativeToolSupport::Rejected) {
                engine().note_tools_rejected(true);
                ++empty_streak;
                this->push_event_(AgentEvent::Status,
                    "Ход пришёл пустым: провайдер ответил на запрос с "
                    "нативными инструментами — повторяю шаг без них "
                    "(текстовый протокол).");
                if (empty_streak >= 2) {
                    const std::string err =
                        "провайдер вернул пустой ответ и без нативных "
                        "инструментов";
                    this->push_event_(AgentEvent::Error, "[ошибка] " + err);
                    full_response = full_response.empty()
                        ? "[ошибка] " + err : full_response;
                    return false;
                }
                /* Ход в историю НЕ пишется: это не ответ модели, а сбой
                 * транспорта. Следующий шаг повторит тот же запрос. */
                continue;
            }

            ++empty_streak;
            if (empty_streak >= 2) {
                const std::string err =
                    "провайдер дважды подряд вернул пустой ответ (finish=stop, "
                    "без текста и вызовов)";
                std::string model = "не задана";
                {
                    const RunScope scope = engine().scope_snapshot();
                    if (scope.info) model = scope.info->model();
                }
                const std::string diag =
                    "model=" + model + " protocol=" +
                    (engine().native_tools_blocked() ? "text" : "native");
                this->push_event_(AgentEvent::Error,
                    ("[ошибка] LLM: " + err + " (" + diag + ")").c_str());
                full_response = full_response.empty()
                    ? "[ошибка] " + err : full_response;
                return false;
            }
            this->push_event_(AgentEvent::Status,
                "Ход пришёл пустым (ответ без текста и без вызовов) — "
                "повторяю шаг с напоминанием.");
            append_note(
                "[reminder] Your last message was EMPTY: no text and no tool "
                "call. Continue the task: make the next tool call (one "
                "wp_action block), or, if the task is finished, write the "
                "final report of at least " +
                std::to_string(min_final_len) + " characters.");
            /* Пустое сообщение в историю не пишется — только напоминание
             * выше; следующий шаг начнётся с него. */
            continue;
        }
        empty_streak = 0;
        reasoning_only_streak = 0;

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
        /* Tool-args JSON / XML-разметка — это НЕ «застревание»: такой ход
         * разбирает ветка ToolArgsJson со своим напоминанием. Считать его
         * коротким ответом — значит форсировать отчёт раньше, чем модель
         * успеет понять, что звать надо инструментом (ses_000000000542). */
        if (response.text().size() < kShortResponseLen &&
            response.tool_calls().empty() &&
            !looks_like_tool_args_json(response.text()) &&
            !looks_like_tool_call_markup(response.text())) {
            ++stuck_counter;
            if (stuck_counter >= kStuckThreshold) {
                this->push_event_(AgentEvent::Status,
                    "Модель застряла — запрашиваю итоговый ответ.");
                commit_turn();
                ask_for_summary(sys,
                    "You keep sending very short answers without progress. "
                    "You may NOT call tools any more. Write the final report "
                    "on what has actually been done.",
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

        /* Текст хода НЕ отправляем в чат сразу: это может быть и текст
         * инструментального хода, и план, и обрывок. В чате ответ
         * пользователю должен быть отделён от прогресса, поэтому решение
         * принимается после вердикта: Completed → Assistant, остальное →
         * Status. Это response.text(), а не «строка без блока»: блок вызова
         * сюда не попадает в принципе, его разбор — забота адаптера
         * (core/llm_source.h). */
        const std::string turn_text = response.text();
        /* JSON с аргументами инструмента не попадает в full_response:
         * иначе он уехал бы пользователю как «финальный ответ», если
         * бюджет кончится или цикл решит, что это итог. */
        if (!turn_text.empty() && !looks_like_tool_args_json(turn_text) &&
            !looks_like_tool_call_markup(turn_text)) {
            if (!full_response.empty()) full_response += "\n\n";
            full_response += turn_text;
        }

        /* Размышление модели — отдельный канал и отдельный визуальный
         * блок: MarkdownRenderer понимает <thinking>…</thinking> и
         * показывает его свёрнутым. Так рассуждение не смешивается с
         * ответом пользователю. В историю оно уже попало частью turn. */
        if (!response.reasoning().empty()) {
            std::string reasoning = response.reasoning();
            /* Ограничиваем объём: в чате reasoning нужен для контекста,
             * а не целиком. Очень длинный текст в одном ImGui-элементе
             * раздувает vertex buffer и делает кадр тяжёлым; полный
             * текст остаётся в панели стриминга и сессии. */
            if (reasoning.size() > limits::kMaxStreamPanelChars) {
                reasoning = text::utf8_prefix(
                    reasoning, limits::kMaxStreamPanelChars);
                reasoning += "\n…";
            }
            this->push_event_(AgentEvent::Assistant,
                "<thinking>\n" + reasoning + "\n</thinking>");
        }

        /* Вызовы инструментов. Аргументы приходят разобранными (И5.1/И5.3),
         * поэтому ни parse_action, ни проекция в 8 слотов здесь не нужны:
         * новый параметр инструмента доходит до обработчика сам. */
        const size_t call_count = response.tool_calls().size();
        bool turn_mutated = false;  /* был ли в этом ходе write/exec */
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

            /* Повторное чтение того же файла с теми же аргументами не
             * выполняем: результат уже в истории и не изменился. Модель
             * получает вместо него явное указание перейти к действию.
             * Это лечит именно то, что видно в сессии 0.18.16: шесть
             * подряд read_file test_gui.py и девять read_file gui.py,
             * между которыми не было ни одной правки. */
            const ToolDef* call_def =
                ToolsRegistry::instance().find(call.name);
            /* Признак «ход что-то изменил» ставится ПОСЛЕ выполнения и
             * только если вызов действительно изменил файл (см. ниже).
             * Раньше он ставился здесь, по одному флагу инструмента: write_file
             * с тем же содержимым считался правкой, сбрасывал серию
             * read-only и разрешал модели дальше «улучшать» файл, не меняя в
             * нём ни байта (ses_000000000006: 8 одинаковых write_file подряд). */
            const bool read_only_call =
                call_def && (call_def->flags & TF_READ_ONLY) != 0;
            const bool mutating_call =
                call_def &&
                (call_def->flags & (TF_WRITES_FILES | TF_EXECUTES)) != 0;
            const std::string call_sig = call.name + "\n" +
                canonical_args(call.arguments, call_def ? &call_def->parameters : nullptr);
            /* Повтор вызова, который инструмент уже признал no-op.
             * В сессии 0.18.18 так зацикливались search_replace/edit_file:
             * модель получала «[no-op] …» и всё равно повторяла тот же
             * вызов. Второй раз не выполняем — сразу называем выход.
             *
             * Список, а не ОДНА подпись: в ses_000000000006 модель
             * чередовала search_replace и write_file, каждый по второму
             * разу, и одиночный last_noop_sig каждый раз забывал предыдущий
             * no-op. Подписи, признанные no-op, копятся до первой настоящей
             * правки; любая из них, встреченная снова, останавливает ход. */
            const auto noop_it = nooped_calls.find(call_sig);
            if (noop_it != nooped_calls.end()) {
                ToolOutput dup;
                dup.title = "повтор no-op " + call.name;
                dup.output =
                    "[repeat-noop] You already tried this exact call and the "
                    "tool reported no change. It was NOT executed again.\n"
                    "Change the replacement text (content), use read_file to "
                    "look at the file, or write_file to replace the whole "
                    "file. Do not repeat an identical no-op call.";
                this->push_event_(AgentEvent::Status,
                    "Повтор no-op вызова " + call.name +
                    " — не выполняю второй раз.");
                LlmResponse::reduce(response, LlmEvent::tool_result(
                    call.call_id, std::move(dup)));
                sync_tool_parts(turn, response);
                continue;
            }
            if (read_only_call && !last_read_sig.empty() &&
                call_sig == last_read_sig) {
                ToolOutput dup;
                dup.title = "повтор " + call.name;
                dup.output =
                    "[повтор] You already called " + call.name + " with "
                    "exactly these arguments on a previous step. Its result "
                    "is in the transcript above and has not changed, so it "
                    "was NOT executed again.\n"
                    "Do NOT repeat this call. Take the next step now: if you "
                    "need a different part of the file, call read_file again "
                    "with a DIFFERENT offset/limit; otherwise write or edit "
                    "the file, run a command, or send the final report.";
                this->push_event_(AgentEvent::Status,
                    "Повторное чтение " + call.name +
                    " с теми же аргументами — не выполняю (результат уже в "
                    "истории).");
                LlmResponse::reduce(response, LlmEvent::tool_result(
                    call.call_id, std::move(dup)));
                sync_tool_parts(turn, response);
                continue;
            }
            /* Анти-перебор чтения ОДНОГО И ТОГО ЖЕ файла.
             *
             * Защита выше ловит только СОВПАДАЮЩИЕ аргументы. В
             * ses_000000000006 модель прочитала library.py шесть раз с
             * РАЗНЫМИ offset/limit (60/50/53/55/9/10), каждый раз «ища
             * нужный фрагмент», который уже лежал в истории. Каждый шаг
             * стоил обращения к модели и уводил от правки.
             *
             * Считаем чтения по каноническому пути. Ключ — путь из
             * аргументов (read_file), счётчик — сколько раз файл прочитан
             * ПОСЛЕ последней записи в него. После N-го чтения без правки
             * не выполняем вызов, а называем выход: содержимое уже в
             * истории, и дальше — либо правка, либо итог. */
            std::string read_path;
            if (call_def && call.name == "read_file") {
                read_path = canonical_args_path(call.arguments);
            }
            if (!read_path.empty()) {
                const int seen = reads_per_file[read_path];
                if (seen >= kMaxReadsPerFile) {
                    ToolOutput dup;
                    dup.title = "хватит читать " + read_path;
                    dup.output =
                        "[хватит-читать] " + read_path + " уже прочитан "
                        + std::to_string(seen) + " раза, и с последней правки "
                        "он не менялся. Всё прочитанное — в истории выше.\n"
                        "Не читай этот файл снова (даже с другим offset/"
                        "limit). Сделай ОДИН из двух шагов: правку (edit_file/"
                        "search_replace/write_file) или итоговый отчёт.";
                    this->push_event_(AgentEvent::Status,
                        "Файл " + read_path + " уже прочитан "
                        + std::to_string(seen) + " раза — не читаю снова.");
                    LlmResponse::reduce(response, LlmEvent::tool_result(
                        call.call_id, std::move(dup)));
                    sync_tool_parts(turn, response);
                    continue;
                }
                reads_per_file[read_path] = seen + 1;
            }
            /* Пишущий/исполняющий вызов мог изменить прочитанное —
             * сбрасываем отпечаток. Read-only вызов запоминаем. */
            last_read_sig = read_only_call ? call_sig : std::string();

            ToolRunner tool_runner(state_, cb_, this->push_event_);
            const ToolOutcome outcome =
                tool_runner.run(call.name, call.arguments);
            if (outcome.ok) {
                /* Запоминаем no-op: следующий такой же вызов будет
                 * остановлен выше, не тратя шаг и не спрашивая doom_loop.
                 *
                 * Список, а не одна подпись: модель чередует вызовы
                 * (search_replace ↔ write_file), и одиночный отпечаток
                 * забывал предыдущий no-op (ses_000000000006). */
                if (outcome.output.output.find("[no-op]") != std::string::npos) {
                    nooped_calls.insert(call_sig);
                } else if (!read_only_call) {
                    /* Настоящая правка/выполнение — все прошлые no-op
                     * больше не актуальны: файл изменился, и повтор
                     * любого из них может быть уже осмысленным. */
                    nooped_calls.clear();
                    if (mutating_call) turn_mutated = true;
                    /* Файл, в который записали, можно читать заново —
     * он изменился. Счётчик чтений именно этого файла сбрасываем. */
                    if (!read_only_call &&
                        (call.name == "write_file" ||
                         call.name == "edit_file" ||
                         call.name == "search_replace" ||
                         call.name == "apply_patch")) {
                        const std::string written =
                            canonical_args_path(call.arguments);
                        if (!written.empty()) reads_per_file.erase(written);
                    }
                }
                LlmResponse::reduce(response, LlmEvent::tool_result(
                    call.call_id, cap_result(std::move(outcome.output),
                                             result_budget_for())));
            } else {
                /* Отказ инструмента — это не правка: файл не изменился.
                 * Раньше здесь стоял last_noop_sig.clear(), то есть отказ
                 * стирал память о прошлых no-op, и модель, получив
                 * «текст не найден», шла по тому же кругу заново. */
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
                cb_.chat_event("\x01[ai-coder] ⏸ ожидание разрешения: " + perm_path);
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
                ? ("(the user granted access to " + perm_path
                   + "; the permission is active. Call the tool again with "
                     "the same arguments.)")
                : ("(the user DENIED access to " + perm_path
                   + ". Do not repeat that call: it is forbidden. Tell the "
                     "user what is needed, or do the same thing inside the "
                     "project.)"));
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

        /* Анти-перебор: серия read-only ходов без изменений — модель
         * застревает в чтении (ses_496 и живой прогон). Напоминаем
         * перейти к действию, а не собирать факты бесконечно. */
        /* Сбрасываем серию ТОЛЬКО на правке/выполнении. Грep-текст (JSON,
     * авто-выполненный ниже) — это тоже осмотр, и он НЕ должен обнулять
     * серию, иначе анти-перебор не срабатывает (ses_000000000612). */
        if (turn_mutated)
            read_only_streak = 0;
        else if (call_count > 0 ||
                 looks_like_tool_args_json(response.text()) ||
                 looks_like_tool_call_markup(response.text()))
            ++read_only_streak;
        /* И15.4: успешная правка/выполнение в этом ходе — «работа идёт».
 * Отличаем write/edit/apply_patch (меняют файл) от bash (исполнение): для
 * продления бюджета важен именно факт, что задача продвигается, а не
 * тип. `turn_mutated` ставится и на bash, поэтому этого достаточно. */
        if (turn_mutated) made_write = true;
        if (read_only_streak >= kExploreRemindAt &&
            (read_only_streak == kExploreRemindAt || read_only_streak % 4 == 0)) {
            this->push_event_(AgentEvent::Status,
                "Серия read-only вызовов: " + std::to_string(read_only_streak) +
                " — напоминаю перейти к действию.");
            append_note(
                "[reminder] You have made " + std::to_string(read_only_streak) +
                " read-only tool calls in a row without changing anything. Do not "
                "keep exploring out of habit: if you already know the cause, make "
                "the change now (edit_file/search_replace, or write_file for a full "
                "rewrite) and verify it. For listing/searching use list/glob/"
                "grep_search, not bash ls/find/cat. If this is an analysis task, "
                "finish it and summarize.");
        }

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
            turn_verdict(response, turn, current_user_id, min_final_len);
        if (verdict == TurnVerdict::Completed) {
            /* Гейт проверки: если в сессии есть правки файлов, но НИ ОДНОГО
             * запуска тестов/проверки — не принимаем итог, просим проверить
             * (ses_000000000582: «обе проблемы исправлены» без единого запуска). */
            {
                /* Только КОД требует проверки: правка .md/.txt/.json — не
                 * повод гонять тесты (иначе гейт срабатывал бы на документах). */
                const auto is_code_path = [](const std::string& p) {
                    static const char* const kExts[] = {
                        ".py", ".pyw", ".js", ".jsx", ".ts", ".tsx", ".php",
                        ".java", ".c", ".h", ".cc", ".cpp", ".hpp", ".cs",
                        ".go", ".rs", ".rb", ".swift", ".kt", ".scala", ".sh",
                        ".pl", ".lua", ".r", ".m", ".sql", ".html", ".css",
                        ".vue", ".svelte"};
                    for (const char* e : kExts) {
                        const size_t n = std::char_traits<char>::length(e);
                        if (p.size() >= n && p.compare(p.size() - n, n, e) == 0)
                            return true;
                    }
                    return false;
                };
                bool has_write = false, has_verify = false;
                {
                    std::lock_guard<std::mutex> lk(state_.mtx);
                    for (const Message& m : state_.session) {
                        for (const MessagePart& p : m.parts) {
                            if (!p.is(PartKind::Tool)) continue;
                            const std::string& n = p.tool_name();
                            if (n == "write_file" || n == "edit_file" ||
                                n == "search_replace") {
                                if (is_code_path(p.args().get_string("path")))
                                    has_write = true;
                            } else if (n == "apply_patch") {
                                /* Патч — как правило, по коду. */
                                has_write = true;
                            }
                            if (n == "bash" || n == "pytest_run" ||
                                n == "python_lint" || n == "python_run")
                                has_verify = true;
                        }
                    }
                }
                if (has_write && !has_verify && !verification_reminded) {
                    verification_reminded = true;
                    this->push_event_(AgentEvent::Status,
                        "Изменения есть, но проверки не было — прошу запустить тесты.");
                    append_note(
                        "[reminder] You changed files but did NOT run any verification. "
                        "Run the project's tests or a smoke test (pytest_run, python_lint, "
                        "python_run, or bash) and check the result BEFORE the final report. "
                        "Do not repeat the plan; run the check now.");
                    if (!turn_text.empty())
                        this->push_event_(AgentEvent::Status, turn_text);
                    continue;
                }
            }
            final_given = true;
            /* Заголовок отделяет итог от прогресса. Сам ответ уходит
             * Assistant-сообщением и рендерится как markdown; всё, что
             * было до него, — Status/Tool. */
            this->push_event_(AgentEvent::Status, "=== Итоговый ответ ===");
            if (!turn_text.empty()) {
                this->push_event_(AgentEvent::Assistant, turn_text);
            }
            this->push_event_(AgentEvent::Status, "Готово (финальный ответ).");
            return true;
        }
        if (verdict == TurnVerdict::TooShort) {
            /* Преждевременный финал: текст без вызова инструмента, но и не
             * настоящий итог. Пустой ход сюда больше не попадает: он
             * разбирается выше и в историю не пишется.
             *
             * Напоминание называет не только «вызови инструмент», но и
             * конкретный первый вызов: наблюдение (reflection/beam-free,
             * kilo) — модель отвечает «нужно вызвать инструмент» и не
             * вызывает, потому что не понимает, какой именно. */
            std::string root;
            /* Уже прочитанные файлы меняют следующий шаг: если результат
             * инструмента в истории есть, осматриваться некуда — надо
             * писать. Прежний пример всегда называл repo_map, и модель
             * после осмотра снова шла в repo_map: напоминание уводило её
             * назад, а не к правке файла. */
            bool read_something = false;
            {
                std::lock_guard<std::mutex> lk(state_.mtx);
                root = state_.project_dir;
                for (const Message& m : state_.session) {
                    for (const MessagePart& p : m.parts) {
                        if (p.kind() == PartKind::Tool && p.has_result()) {
                            read_something = true;
                            break;
                        }
                    }
                    if (read_something) break;
                }
            }
            std::string example;
            if (engine().native_tools_blocked()) {
                if (read_something) {
                    /* НЕ write_file по умолчанию: частичная правка — это
                     * edit_file/search_replace, а write_file снёс бы файл
                     * (ses_000000000496). write_file — только для полной
                     * перезаписи, и это сказано явно. */
                    example =
                        "The files are already read — change the PART you need. "
                        "Reply EXACTLY like this:\n"
                        "```\nwp_action\nTOOL: edit_file\nPATH: <file>\n"
                        "K: <first line>\nQUERY: <last line>\n"
                        "CONTENT_BEGIN\n...new lines...\nCONTENT_END\n```\n"
                        "(write_file only to replace the WHOLE file.)";
                } else {
                    example =
                        "Reply EXACTLY like this:\n"
                        "```\nwp_action\nTOOL: repo_map\nROOT: " +
                        (root.empty() ? std::string("<project root>") : root) +
                        "\n```";
                }
            } else {
                example = read_something
                    ? "For example, call edit_file (path, k, query, content) or "
                      "search_replace (path, query, content) to change part of "
                      "the file. Use write_file ONLY to replace the whole file."
                    : "For example, call the repo_map tool with root='" +
                          (root.empty() ? std::string("<project root>") : root) +
                          "'.";
            }
            this->push_event_(AgentEvent::Status,
                "Модель не вызвала инструмент и не дала итог — продолжаю.");
            append_note(
                "[reminder] Your last message had no tool call and is too "
                "short to be a final report. I need a tool call now, not "
                "reasoning. " + example +
                " After the result, continue with the next action.");
        } else if (verdict == TurnVerdict::Truncated) {
            /* Ответ оборван. Причина видна в finish_reason: `length` —
             * упёрлись в лимит токенов; `stop` с незакрытой скобкой или
             * фенсом — шлюз закрыл поток посреди фразы. И то и другое НЕ
             * итог: если принять обрывок, пользователь в чате получает
             * половину ответа и не понимает, где остальное. */
            ++truncated_streak;
            if (truncated_streak >= 2) {
                ask_for_summary(sys,
                    "Your previous answer was cut off. Write a CONCISE final "
                    "report: what is done and what remains. Do not repeat "
                    "reasoning and do not start anything new.",
                    full_response);
                return true;
            }
            const bool by_tokens =
                response.finish_reason() == "length";
            this->push_event_(AgentEvent::Status,
                by_tokens
                    ? "Ответ обрезан лимитом токенов (finish=length) — прошу "
                      "завершить мысль кратко."
                    : "Ответ оборвался на полуслове — прошу продолжить с места "
                      "обрыва.");
            append_note(
                by_tokens
                    ? "[reminder] Your answer hit the token limit "
                      "(finish=length). Do not repeat what you already said: "
                      "either make the next tool call, or write a short final "
                      "report."
                    : "[reminder] Your previous answer was cut mid-sentence "
                      "(an open bracket/quote or code fence). Do not start "
                      "over and do not repeat yourself: continue exactly from "
                      "the cut and finish the report.");
        } else if (verdict == TurnVerdict::CodeAnswer) {
            /* Отдельная причина и отдельное напоминание: общее «дай ИТОГОВЫЙ
             * ответ не короче N символов» на код в чате подтолкнуло бы
             * модель напечатать ЕЩЁ КОД. Наблюдение (qwen-flash, DashScope,
             * 2026-11): модель печатала код калькулятора в ответе, файл не
             * записывала, и цикл 4355 символами «кода-итога» закрывал
             * задачу как выполненную. Напоминание называет действие: код —
             * в файл, отчёт — текстом. */
            this->push_event_(AgentEvent::Status,
                "Итоговый ответ — код вместо отчёта: код нужно записать в файл.");
            append_note(
                "[reminder] Your answer is CODE, not a final report. Code is "
                "written with a tool (write_file, one wp_action block), not "
                "printed in the reply. Write the complete file and then give "
                "the final report: what was done, where the file is, how to "
                "run it — at least " +
                std::to_string(min_final_len) + " characters.");
        } else if (verdict == TurnVerdict::PlanningText) {
            /* Модель описала, что собирается сделать, но не сделала.
             * Принять это за итог — значит закрыть задачу без правки;
             * именно так калькулятор остался недописанным. Просим
             * выполнить следующий шаг инструментом, а не повторять план. */
            ++planning_streak;
            /* Два плана подряд — ещё не повод сдаваться: в сессии 0.18.10
             * именно второй «план вместо действия» закрывал задачу без
             * единой правки. Даём третью попытку и на второй раз просим
             * уже не «следующий шаг», а конкретный вызов записи. */
            if (planning_streak >= 3) {
                ask_for_summary(sys,
                    "You sent a plan instead of an action three times. You "
                    "may NOT call tools any more. Write the final report: "
                    "what was actually done and what remains.",
                    full_response);
                return true;
            }
            this->push_event_(AgentEvent::Status,
                "Это план, а не выполнение — прошу сделать следующий шаг "
                "инструментом.");
            append_note(
                planning_streak == 1
                    ? "[reminder] Your answer is a plan/reasoning, not a "
                      "result. The plan already exists; do not repeat it. "
                      "Call the next tool (write_file/edit_file/apply_patch or "
                      "bash) and after the RESULT keep going until the task "
                      "is done."
                    : "[reminder] You sent a plan again instead of an action. "
                      "I need EXACTLY ONE write call now: write_file (or "
                      "edit_file/apply_patch). No text without a wp_action "
                      "block. If the file is corrupted, rewrite it whole; do "
                      "not re-read it.");
        } else if (verdict == TurnVerdict::HandedOff) {
            /* Отчёт перекладывает работу на пользователя («исправьте
             * вручную строку 12»). Принять его — значит закрыть задачу
             * невыполненной: в ses_000000000006 именно так ушёл итог,
             * хотя файл остался с опечаткой. Просим ОДИН раз сделать
             * работу самому; второй такой отчёт уже принимаем — иначе
             * цикл будет вымогать невозможное. */
            if (!handoff_reminded) {
                handoff_reminded = true;
                this->push_event_(AgentEvent::Status,
                    "Отчёт перекладывает работу на пользователя — прошу "
                    "сделать её самому.");
                append_note(
                    "[reminder] Your report hands the work back to the user "
                    "(\"fix it manually\", \"check it yourself\"). That is not "
                    "a result. You are the one who must do it. If a call "
                    "failed, use the tool's message: for a partial edit call "
                    "edit_file (path, k, query, content) or search_replace "
                    "(path, query, content); to replace the whole file call "
                    "write_file with the FULL corrected content. Then verify "
                    "it yourself (pytest_run, python_lint, python_run, bash) "
                    "and only then write the final report. Do NOT ask the "
                    "user to finish the task.");
            } else {
                /* Второй раз — принимаем: дальше только сожжём бюджет. */
                final_given = true;
                this->push_event_(AgentEvent::Status, "=== Итоговый ответ ===");
                this->push_event_(AgentEvent::Assistant, turn_text);
                this->push_event_(AgentEvent::Status, "Готово (финальный ответ).");
                return true;
            }
        } else if (verdict == TurnVerdict::ToolArgsJson) {
            const std::string& printed = response.text();
            /* СНАЧАЛА: read-only вызов (grep_search/read_file) выполняем за
             * модель. Это ПРОДУКТИВНЫЙ ход, а не «модель 3 раза печатает
             * JSON»: не считаем его в tool_args_streak и не ноем «не печатай
             * JSON» — иначе серия авто-выполненных grep'ов ложно закрывала бы
             * задачу (ses_000000000612). */
            std::string inferred_note;
            if (try_infer_readonly_call(printed, inferred_note)) {
                tool_args_streak = 0;
                this->push_event_(AgentEvent::Status,
                    "JSON-вызов read-only распознан и выполнен за модель.");
                append_note(inferred_note);
            } else {
                ++tool_args_streak;
                if (tool_args_streak >= 3) {
                    ask_for_summary(sys,
                        "You printed tool arguments as JSON three times instead "
                        "of calling the tool. You may NOT call tools any more. "
                        "Write the final report for the user: what was done, "
                        "where the file is, how to run it.",
                        full_response);
                    return true;
                }
                this->push_event_(AgentEvent::Status,
                    "Это JSON-аргументы инструмента, а не итоговый отчёт — "
                    "прошу вызвать инструмент и дать отчёт.");
                /* task (prompt/description) — «запускает субагента» текстом. */
                const bool looks_like_task =
                    printed.find("\"prompt\"") != std::string::npos &&
                    (printed.find("\"description\"") != std::string::npos ||
                     printed.find("\"subagent_type\"") != std::string::npos);
                /* bash: ключ command/cli (ses_000000000496, шаг 26). */
                const bool looks_like_bash =
                    !looks_like_task &&
                    (printed.find("\"command\"") != std::string::npos ||
                     printed.find("\"cli\"") != std::string::npos);
                std::string reminder;
                if (looks_like_task) {
                    reminder =
                        "[reminder] These are task tool arguments, not a final "
                        "report. Do not print JSON. Call the task tool with "
                        "subagent_type (see the tool catalogue), description, "
                        "and prompt. The task tool requires subagent_type.";
                } else if (looks_like_bash) {
                    reminder =
                        "[reminder] This is a bash call printed as JSON text, not "
                        "an executed command. Do not print JSON. Call the bash tool "
                        "with cli=\"<command>\" (the key is cli; command is accepted "
                        "as an alias). After the RESULT, continue or give the final "
                        "report.";
                } else {
                    reminder =
                        "[reminder] You printed tool arguments as JSON text "
                        "instead of calling the tool. Do not print JSON. Call the "
                        "appropriate tool (for a partial file edit use edit_file or "
                        "search_replace; write_file replaces the whole file) with "
                        "those arguments; after the RESULT give the user a final "
                        "report: what was done, where the file is, how to run it.";
                }
                append_note(reminder);
            }
        }

        /* Не итог: текст хода — это прогресс/рассуждение, а не ответ
         * пользователю. Показываем его Status'ом (обрезанным), чтобы в
         * чате не было нескольких «ответов» подряд и было видно, что это
         * ещё работа. */
        if (!turn_text.empty() && verdict != TurnVerdict::ToolArgsJson) {
            this->push_event_(AgentEvent::Status, turn_text);
        }

        /* Серия обрывов считается подряд: законченный ход её сбрасывает.
         * Иначе два обрыва за всю задачу (даже далеко друг от друга)
         * закрывали бы её принудительным итогом. */
        if (verdict != TurnVerdict::Truncated) truncated_streak = 0;
        if (verdict != TurnVerdict::PlanningText) planning_streak = 0;
        if (verdict != TurnVerdict::ToolArgsJson) tool_args_streak = 0;

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
        ask_for_summary(sys, kLastStepReminder, full_response);
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
    /* И11.14: хвостовой ход — тоже ход, и отмена в нём нужна тем же
     * основанием, что в основном цикле (D23). */
    AbortToken* tail_abort = nullptr;
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        tail_abort = state_.turn_abort.get();
    }
    const bool ok = LlmClient::fetch(cb_, sys_prompt, msgs, events,
                                     nullptr, -1, tail_abort);
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
    /* Хвостовой ответ — тоже итог: отделяем его от прогресса так же,
     * как основной путь. */
    this->push_event_(AgentEvent::Status, "=== Итоговый ответ ===");
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

    /* И15.1 (BEHAVIOR_PARITY_PLAN): тот же мягкий бюджет, что у
     * основного цикла. Раньше субагент сдавался ровно на `max_steps`, и
     * «утыкается в лимит даже с субагентами» было верно: ребёнок, который
     * на последнем шаге ещё звал инструмент (то есть работал), обрывался
     * без итога. Правило простое и то же, что у родителя: reached-конец
     * цикла без break означает «модель хочет продолжать» — если это
     * происходит на последнем шаге, продлеваем бюджет (дважды, как у
     * родителя), а не сдаёмся. Если модель действительно закончила, она
     * вышла через break выше, и продление не нужно. */
    const int base_steps = max_steps;
    int total_steps = max_steps;
    int budget_extensions = 0;

    for (int step = 0; step < total_steps; ++step) {
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
        const StepSnapshot shots = take_step_snapshot(state, cb, push_event);
        /* И10.2: список изменений — в сообщение ребёнка. Лока здесь нет
         * и быть не может: история субагента локальна (core/subagent.h),
         * а снимки только что записал тот же поток. */
        attach_patch_part(history, shots.before,
                          snapshot::changed_files(shots.before, shots.after));

        std::vector<LlmEvent> events;
        /* И11.14: субагент идёт через LlmClient с тем же токеном, что и
         * родитель (D23). Токен берётся под локом субагента — состояние
         * у него своё, и общий токен сессии отменил бы ход не того
         * агента. */
        AbortToken* sub_abort = nullptr;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            sub_abort = state.turn_abort.get();
        }
        const bool ok = LlmClient::fetch(
            cb, sys_prompt, to_model_messages(history), events,
            nullptr, -1, sub_abort);
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
                    call.call_id, cap_result(std::move(outcome.output),
                                             result_budget_for())));
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

        /* И15.1: reached-конец бюджета без break означает «модель ещё
         * работает» — на этом шаге был вызов, и она хочет продолжать.
         * Продлеваем один раз ( twice ), а не сдаёмся. Условие то же, что
         * у родителя: незакрытые `todos` (план ребёнка) — но у субагента
         * их обычно нет, поэтому судим по факту «был вызов и finish=tool-calls»,
         * то есть по тому, что цикл дошёл до конца, а не вышел. */
        if (step + 1 >= total_steps && budget_extensions < 2) {
            ++budget_extensions;
            total_steps += base_steps;
            push_event(AgentEvent::Status,
                "Субагент " + state.scope.agent +
                " исчерпал бюджет шагов, но ещё работает — продлеваю на " +
                std::to_string(base_steps) + " (продление " +
                std::to_string(budget_extensions) + " из 2).");
        }
    }
    if (!result.ok && result.error.empty()) {
        result.error =
            "[ошибка] субагент " + state.scope.agent +
            " не закончил задачу за " + std::to_string(total_steps) +
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

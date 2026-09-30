/*
 * test_task_tool.cpp — И8.7: инструмент `task` и путь агента в цикл.
 *
 * Что здесь проверяется и почему именно так
 * -----------------------------------------
 * До И8.7 реестр агентов был наполнен (И8.1–8.6), а цикл его не читал:
 * выбор агента в движке отсутствовал, а Engine::build_system_prompt
 * склеивал только базовый промпт, режим и навыки. Задача 8.7 — написать
 * ПУТЬ, поэтому проверки идут не по функции инструмента, а по тому, что
 * видно СНАРУЖИ: что ушло в модель, что вернулось в историю родителя и
 * что осталось в движке после вложенного хода.
 *
 * Три из проверок существуют именно потому, что Engine и ToolsRegistry —
 * синглтоны процесса, а вложенный ход меняет общее состояние:
 *   - дочерняя сессия НЕ попадает в state_.session (иначе родитель
 *     прочитал бы чужой диалог в своей транскрипте);
 *   - после ребёнка восстановлены и область выполнения, и системный
 *     промпт (иначе СЛЕДУЮЩИЙ ход родителя ушёл бы с промптом субагента
 *     и с его набором инструментов);
 *   - у ребёнка своя история зацикливания (иначе три одинаковых чтения
 *     субагентом спросили бы у пользователя «зацикливание?» вместо
 *     родителя).
 *
 * Проверки на текст ищут ОБЪЯВЛЕННЫЕ места (обязательные поля схемы,
 * конкретные ключи в сообщении об отказе), а не вхождения подстрок:
 * иначе проверка приняла бы сообщение, в котором назван не тот агент.
 */

#include "test_framework.h"
#include "test_printers.h"
#include "test_support.h"
#include "../core/agent_components.h"
#include "../core/agent_registry.h"
#include "../core/base_tools.h"
#include "../core/engine.h"
#include "../core/limits.h"
#include "../core/prompts.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace coder;

namespace {

/* Инструменты: нужны настоящие, потому что проверка «каталог субагента
 * урезан правилами агента» бессмысленна на заглушках, которых в
 * реестре нет. */
void register_tools() {
    static bool done = false;
    if (done) return;
    done = true;
    register_base_tools();
    test_support::approve_all_permissions();
}

/* --- Хост-имитация ---
 *
 * Один и тот же хост обслуживает и ход родителя, и вложенный ход
 * субагента, — как в настоящей работе. Ответы выдаются по порядку, а
 * ВСЁ, что ушло в модель, записывается: по системному промпту запроса
 * видно, кто его собрал, а по транскрипту — что субагент увидел в
 * результате своего вызова. */
struct FakeHost {
    std::vector<std::string> replies;
    size_t next = 0;
    std::vector<std::string> sys_prompts;
    std::vector<std::vector<ModelMessage>> requests;
    bool fail = false;
    std::string error = "сеть недоступна";
    long long prompt_tokens = 100;

    bool chat(const std::string& sys, const std::vector<ModelMessage>& msgs,
              LlmReply& out) {
        sys_prompts.push_back(sys);
        requests.push_back(msgs);
        if (fail) {
            out.error = error;
            return false;
        }
        if (replies.empty()) {
            out.error = "хост без ответов";
            return false;
        }
        out.content = replies[std::min(next, replies.size() - 1)];
        ++next;
        out.finish_reason = "stop";
        out.prompt_tokens = prompt_tokens;
        out.completion_tokens = 20;
        return true;
    }

    size_t calls() const { return requests.size(); }
    std::string sys(size_t index) const {
        return index < sys_prompts.size() ? sys_prompts[index] : std::string();
    }
    std::string transcript(size_t index) const {
        std::string out;
        if (index >= requests.size()) return out;
        for (const ModelMessage& m : requests[index]) out += m.content + "\n";
        return out;
    }
};

/* Гарантированный join. Тест с ожидающим потоком часто падает на
 * ASSERT — а незаjoinенный std::thread зовёт std::terminate, и вместо
 * внятного FAIL получается падение всего прогона без указания строки. */
struct Joiner {
    std::vector<std::thread>& ts;
    explicit Joiner(std::vector<std::thread>& t) : ts(t) {}
    void join() { for (auto& t : ts) if (t.joinable()) t.join(); }
    ~Joiner() { join(); }
};

/* Правила разрешений: пользователь нажал «всегда» на всё, кроме
 * внешних каталогов (иначе заглушка отключила бы PermissionGate).
 * PermissionEngine — член синглтона Engine, и без сброса проверка
 * унаследовала бы решения предыдущего теста. */
struct PermissionGuard {
    PermissionGuard() { Engine::instance().permissions().reset(); }
    ~PermissionGuard() { Engine::instance().permissions().reset(); }
};

/* Реестр агентов — тоже синглтон процесса. В тестах восстанавливается
 * то состояние, которое оставил бы Engine::load_settings: встроенные
 * агенты зарегистрированы, пользовательских нет. */
void reset_registry() {
    AgentRegistry::instance().clear();
    register_builtin_agents(AgentRegistry::instance());
}

std::string long_answer(const std::string& tail) {
    std::string out = "Итог по задаче. ";
    while (out.size() < 320) out += "Работа выполнена, всё проверено. ";
    return out + tail;
}

/* Блок вызова в том виде, в каком его пишет модель: ```json … ```.
 * Второй аргумент — ХВОСТ после имени инструмента, с ведущей запятой:
 * блок собирается как {"tool": "X"<tail>} (так же в test_agent_loop). */
std::string call_block(const std::string& tool, const std::string& args_tail) {
    return "Делаю.\n```json\n{\"tool\": \"" + tool + "\"" + args_tail +
           "}\n```";
}

/* Аргументы `task` одной строкой — как их пришлёт модель. */
std::string task_args(const std::string& description,
                      const std::string& prompt,
                      const std::string& subagent_type) {
    return ", \"description\": \"" + description + "\", \"prompt\": \"" +
           prompt + "\", \"subagent_type\": \"" + subagent_type + "\"";
}

/* Окружение одного теста. Восстанавливает ВСЁ, что меняет вложенный
 * ход: область, промпт, историю, режим, проект, лимиты, разрешения и
 * реестр агентов (правило 5 SESSION_START). */
struct TaskFixture {
    FakeHost host;
    PermissionGuard permissions;
    fs::path project;

    TaskFixture() : permissions() {
        register_tools();
        project = fs::temp_directory_path() /
                  ("wp_coder_task_" + std::to_string(::getpid()) + "_" +
                   std::to_string(host.replies.size()));
        fs::create_directories(project);
    }
    ~TaskFixture() {
        std::error_code ec;
        fs::remove_all(project, ec);
        engine_state().session.clear();
        reset_registry();
    }

    HostCallbacks callbacks() {
        HostCallbacks cb;
        FakeHost* h = &host;
        cb.llm_chat = [h](const std::string& sys,
                          const std::vector<ModelMessage>& msgs,
                          LlmReply& out) {
            return h->chat(sys, msgs, out);
        };
        cb.llm_is_connected = []() { return true; };
        cb.path_data_dir = [this] { return project.string(); };
        cb.settings_set = [](const std::string&, const std::string&) {};
        cb.settings_get = [](const std::string&, const std::string& d) {
            return d;
        };
        cb.chat_event = [](const std::string&) {};
        return cb;
    }

    /* Чистое состояние цикла. Без планирования: оно задаёт отдельный
     * вызов модели и не относится к проверке. */
    void prepare(int max_steps = 6) {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().session.clear();
        engine_state().session.push_back(Message::user("разберись с задачей"));
        engine_state().project_dir = project.string();
        engine_state().mode = 0;
        engine_state().plan_mode = false;
        engine_state().use_planning = false;
        engine_state().max_steps = max_steps;
        engine_state().todos.clear();
        engine_state().recent_calls.clear();
        engine_state().allowed_external_paths.clear();
        engine_state().measured_input_tokens = 0;
        engine_state().total_prompt_tokens = 0;
        engine_state().total_completion_tokens = 0;
        engine_state().model_limits = compaction::ModelLimits();
        engine_state().compaction_config = compaction::CompactionConfig();
        engine_state().abort_requested.store(false);
        engine_state().shutting_down = false;
        engine_state().state = AgentState::Executing;
        engine_state().steps = 0;
    }

    std::vector<Message> history() {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        return engine_state().session;
    }
};

/* Часть-вызов по имени инструмента. */
const MessagePart* tool_part(const Message& m, const std::string& tool) {
    for (const MessagePart& p : m.parts) {
        if (p.is(PartKind::Tool) && p.tool_name() == tool) return &p;
    }
    return nullptr;
}

/* Есть ли в тексте объявленное упоминание инструмента как строки каталога.
 * Каталог печатает «- read_file — …», поэтому ищем с началом строки:
 * вхождение в словах описания ничего не значит. */
bool catalogue_mentions(const std::string& prompt, const std::string& tool) {
    const std::string marker = "\n- " + tool;
    return prompt.find(marker) != std::string::npos;
}

} // namespace

/* ======================================================================
 * 1. Инструмент объявлен так, как его ждёт модель
 * ====================================================================== */

TEST(task_tool_is_registered_with_the_arguments_of_the_port) {
    register_tools();

    /* Реестр — единственный источник истины, и инструмент обязан быть в
     * нём ПОСЛЕ register_base_tools: отдельный вход регистрации был бы
     * местом, где его можно забыть. */
    const ToolDef* def = ToolsRegistry::instance().find("task");
    if (def == nullptr) {
        std::cerr << "  после register_base_tools() нет инструмента task"
                  << std::endl;
    }
    ASSERT_TRUE(def != nullptr);
    ASSERT_TRUE(static_cast<bool>(def->handler));
    ASSERT_TRUE(!def->description.empty());

    /* Подпись порта (tool/task.ts): description, prompt, subagent_type и
     * два необязательных поля, работа которых придёт в 8.9/8.14. */
    const json::JsonValue props = def->parameters.get("properties");
    for (const char* name : {"description", "prompt", "subagent_type",
                             "task_id", "background"}) {
        if (!props.is_object() || !props.get(name).is_object()) {
            std::cerr << "  в схеме task нет параметра " << name << std::endl;
        }
        ASSERT_TRUE(props.is_object() && props.get(name).is_object());
    }
    ASSERT_EQ(std::string(props.get("background").get_string("type")),
              std::string("boolean"));
    ASSERT_EQ(std::string(props.get("prompt").get_string("type")),
              std::string("string"));

    /* Обязательны ровно три: без описания и без задания звать нечего, а
     * task_id/background необязательны (и отклоняются явным отказом). */
    const json::JsonValue req = def->parameters.get("required");
    std::vector<std::string> required;
    for (size_t i = 0; i < req.size(); ++i) required.push_back(req.at(i).as_string());
    std::sort(required.begin(), required.end());
    const std::vector<std::string> expect = {"description", "prompt",
                                             "subagent_type"};
    ASSERT_TRUE(required == expect);

    /* Свой ключ разрешения: «всегда разрешить делегирование» и «всегда
     * разрешить запуск команд» — разные решения пользователя. */
    ASSERT_EQ(def->permission_key, std::string("task"));

    /* Флаги: делегирование долгое (несколько запросов к модели) и не
     * classified-инструмент; правкой файлов оно не занимается (файлы
     * пишут инструменты субагента, и каждый такой вызов проверяется
     * отдельно), поэтому в план-режиме доступно. В Research — нет:
     * обещание «ничего не меняется» нельзя давать агенту, чьи
     * инструменты заранее неизвестны. */
    ASSERT_EQ(def->flags, (unsigned)TF_SLOW);
    ASSERT_TRUE(check_tool_mode_policy("task", *def, 0, true).empty());
    ASSERT_FALSE(check_tool_mode_policy("task", *def, 1, false).empty());
}

/* ======================================================================
 * 2. Путь целиком: цикл родителя → task → ход субагента → результат
 * ====================================================================== */

TEST(task_runs_the_named_subagent_and_its_text_reaches_the_parent) {
    TaskFixture fx;
    /* Порядок ответов: ход родителя (вызов task), два хода субагента
     * (с вызовом инструмента и итогом), финал родителя. */
    fx.host.replies = {
        call_block("task", task_args("Обзор модуля", "Найди, где создаётся сессия.",
                                     "wp_explore")),
        "Читаю проект.\n```json\n{\"tool\": \"list_skills\"}\n```",
        "Сессия создаётся в core/session_store.cpp, класс SessionArchive.",
        long_answer("Обзор готов, продолжаю.") };
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    const std::string parent_prompt_before = engine().build_system_prompt();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [](AgentEvent::Kind, const std::string&) {});
    loop.run(engine().build_system_prompt(), response);

    /* Ровно четыре запроса к модели: шаг родителя, два хода субагента,
     * следующий шаг родителя. Если бы субагент не пошёл, запросов было бы
     * два; если бы его история не обновлялась, модель получила бы вызов
     * без результата и запросов стало бы больше. */
    ASSERT_EQ(fx.host.calls(), (size_t)4);
    if (fx.host.calls() != 4) {
        for (size_t i = 0; i < fx.host.calls(); ++i) {
            std::cerr << "  запрос " << i << ": " << fx.host.transcript(i).substr(0, 200)
                      << std::endl;
        }
    }

    /* Промпт субагента собран ТЕМ ЖЕ кодом, что и родительский: в нём
     * есть и базовый протокол, и роль агента. Промпт агента без базового
     * означал бы, что субагент не знает протокола вызовов, а базовый без
     * роли — что роль потерялась. */
    const std::string child_sys = fx.host.sys(1);
    ASSERT_TRUE(child_sys.find(kBaseSystemPrompt) != std::string::npos);
    ASSERT_TRUE(child_sys.find(kAgentExplorePrompt) != std::string::npos);
    /* Роль чужого агента не подмешивается. */
    ASSERT_TRUE(child_sys.find(kAgentGeneralPrompt) == std::string::npos);
    /* Второй ход субагента ушёл с ТЕМ ЖЕ системным промптом: смена роли
     * или набора инструментов между шагами ребёнка означала бы, что
     * правила агента живут не в одном месте. */
    ASSERT_EQ(fx.host.sys(2), child_sys);

    /* Промпт родителя после вложенного хода — ТОТ ЖЕ, что был до него.
     * Это и есть проверка на утечку состояния: синглтон с подменённой
     * областью ушёл бы в следующий ход с промптом субагента. */
    ASSERT_EQ(fx.host.sys(3), parent_prompt_before);
    ASSERT_TRUE(fx.host.sys(3).find(kAgentExplorePrompt) == std::string::npos);
    const RunScope after = engine().scope_snapshot();
    ASSERT_EQ(after.agent, std::string(""));
    ASSERT_TRUE(after.info == nullptr);
    ASSERT_EQ(after.depth, 0);

    /* Результат вызова — в истории родителя, в ЕГО части-вызове. */
    const std::vector<Message> h = fx.history();
    ASSERT_EQ(h.size(), (size_t)3);
    const MessagePart* part = tool_part(h[1], "task");
    if (part == nullptr) {
        std::cerr << "  в ходе родителя нет вызова task; части: " << h[1].parts.size()
                  << std::endl;
    }
    ASSERT_TRUE(part != nullptr);
    ASSERT_EQ(part->state(), ToolState::Completed);
    ASSERT_TRUE(part->output().output.find("session_store.cpp") != std::string::npos);

    /* Диалог субагента в историю родителя НЕ попал: ни его собственных
     * ходов, ни его промпта. Иначе родитель прочитал бы чужой разговор в
     * своей транскрипте — а число сообщений выросло бы. */
    for (const Message& m : h) {
        ASSERT_TRUE(m.to_model_string().find("Читаю проект.") == std::string::npos);
        ASSERT_TRUE(m.to_model_string().find(kAgentExplorePrompt) == std::string::npos);
    }
    /* Родителю уходит ОТВЕТ субагента, а не его транскрипт: собственный
     * вызов ребёнка в результат не попал — иначе родитель получил бы
     * чужой диалог в поле результата и строил бы выводы по нему. */
    ASSERT_TRUE(part->output().output.find("\"tool\"") == std::string::npos);
    ASSERT_TRUE(part->output().output.find("list_skills") == std::string::npos);
}

/* ======================================================================
 * 3. Каталог субагента урезан ЕГО правилами, а не общими
 * ====================================================================== */

TEST(subagent_sees_only_the_tools_its_own_rules_allow) {
    TaskFixture fx;
    /* Вызов инструмента делает сам тест, поэтому ПЕРВЫЙ ответ имитатора
     * достаётся субагенту, а не родителю: его звать некому. */
    fx.host.replies = {
        "Нашёл: класс SessionArchive в core/session_store.cpp." };
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    /* План задачи в состоянии: у wp_explore инструмента `todo` нет
     * (8.6), и блок с призывом «держи план в актуальном состоянии» у него
     * быть не должен — иначе модель тратила бы шаг на вызов, который
     * отклонят. У родителя блок остаётся. */
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        TodoItem t;
        t.id = "1";
        t.content = "разобраться с сессиями";
        t.status = "in_progress";
        engine_state().todos.push_back(t);
    }
    reset_registry();

    /* Промпт родителя снимаем ДО вложенного хода: он и есть эталон, с
     * которым сравнивается состояние после. */
    const std::string parent_sys = engine().build_system_prompt();

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Обзор");
    call.set("prompt", "Посмотри проект.");
    call.set("subagent_type", "wp_explore");
    const ToolOutput out = ToolsRegistry::instance().run_output("task", call);
    ASSERT_TRUE(out.output.find("session_store.cpp") != std::string::npos);
    if (fx.host.calls() != 1) {
        for (size_t i = 0; i < fx.host.calls(); ++i) {
            std::cerr << "  запрос " << i << ": " << fx.host.transcript(i).substr(0, 200)
                      << std::endl;
        }
    }
    ASSERT_EQ(fx.host.calls(), (size_t)1);

    /* Единственный запрос — ход субагента, и его системный промпт собран
     * по правилам wp_explore: чтение есть, правок нет, плана нет. */
    const std::string child_sys = fx.host.sys(0);
    ASSERT_TRUE(catalogue_mentions(child_sys, "read_file"));
    ASSERT_FALSE(catalogue_mentions(child_sys, "write_file"));
    ASSERT_FALSE(catalogue_mentions(child_sys, "apply_patch"));
    ASSERT_FALSE(catalogue_mentions(child_sys, "edit_file"));
    ASSERT_TRUE(child_sys.find("ПЛАН ЗАДАЧИ") == std::string::npos);
    ASSERT_TRUE(child_sys.find(kAgentExplorePrompt) != std::string::npos);

    /* У родителя всё на месте: и план, и пишущие инструменты. План
     * скрывается только у агента, у которого `todo` запрещён целиком. */
    ASSERT_TRUE(catalogue_mentions(parent_sys, "write_file"));
    ASSERT_TRUE(parent_sys.find("ПЛАН ЗАДАЧИ") != std::string::npos);
    ASSERT_EQ(engine().build_system_prompt(), parent_sys);

    /* Скрытие — по правилам АГЕНТА, а не выпитое из реестра: после хода
     * инструменты на месте. Реестр — синглтон процесса, и уменьшить его
     * навсегда значило бы отнять пишущие инструменты у всех остальных
     * тестов и у самой сессии. */
    ASSERT_TRUE(ToolsRegistry::instance().find("write_file") != nullptr);
    ASSERT_EQ(std::string(permission_action_name(
                  engine().permissions().evaluate("write", "/srv/a"))),
              std::string("разрешить"));
}

/* ======================================================================
 * 4. Кого звать нельзя
 * ====================================================================== */

TEST(task_refuses_agents_that_cannot_be_called_as_subagents) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* mode: primary — выбор пользователя, а не делегирование;
     * hidden — «не показывать», а не «выключить» (И8.1). Оба обязаны быть
     * недоступны для `task`, и оба обязаны называться в отказе по-разному:
     * несуществующее имя и существующее-с-чужим-свойством — разные
     * ошибки, и модель обязана понимать, какую. */
    AgentDef only_primary;
    only_primary.name = "wp_secret";
    only_primary.description = "Только основной";
    only_primary.mode = AgentMode::Primary;
    only_primary.prompt = "Основной агент.";
    std::string err;
    ASSERT_TRUE(AgentRegistry::instance().add(only_primary, &err));

    AgentDef hidden;
    hidden.name = "wp_quiet";
    hidden.description = "Скрытый";
    hidden.mode = AgentMode::Subagent;
    hidden.hidden = true;
    hidden.prompt = "Скрытый агент.";
    ASSERT_TRUE(AgentRegistry::instance().add(hidden, &err));

    struct Case {
        const char* type;
        bool normalized_name_finds_the_agent;
    };
    const Case cases[] = {
        {"wp_secret", false},        /* есть, но не субагент */
        {"wp_quiet", false},         /* есть, субагент, но скрыт */
        {"wp_нет_такого", false},    /* нет такого */
    };

    for (const Case& c : cases) {
        json::JsonValue call = json::JsonValue::object();
        call.set("description", "Проверка отказа");
        call.set("prompt", "Ничего не делать.");
        call.set("subagent_type", c.type);
        const ToolOutput out = ToolsRegistry::instance().run_output("task", call);

        if (out.output.find("[ошибка] task") == std::string::npos) {
            std::cerr << "  «" << c.type << "»: ожидался отказ, получено: "
                      << out.output << std::endl;
        }
        ASSERT_TRUE(out.output.find("[ошибка] task") != std::string::npos);
        /* Список доступных субагентов назван: без него модель повторит то
         * же имя ещё раз. */
        const size_t list_at = out.output.find("Доступные субагенты:");
        if (list_at == std::string::npos) {
            std::cerr << "  «" << c.type << "»: отказ без списка: " << out.output
                      << std::endl;
        }
        ASSERT_TRUE(list_at != std::string::npos);
        ASSERT_TRUE(out.output.substr(0, list_at).find("wp_explore") ==
                    std::string::npos);
        const std::string list = out.output.substr(list_at);
        ASSERT_TRUE(list.find("wp_explore") != std::string::npos);
        /* Недоступные агенты в списке «доступных» оказываться не должны:
         * перечисление с недоступным внутри хуже, чем отсутствие списка. */
        ASSERT_TRUE(list.find("wp_secret") == std::string::npos);
        ASSERT_TRUE(list.find("wp_quiet") == std::string::npos);
    }

    /* Имя нормализуется: " WP_Explore " обязан найти wp_explore, то есть
     * дойти до хода субагента, а не до отказа «агент не найден». */
    {
        fx.host.replies = {"Обзор короткий."};
        json::JsonValue call = json::JsonValue::object();
        call.set("description", "Проверка нормализации");
        call.set("prompt", "Ничего не делать.");
        call.set("subagent_type", " WP_Explore ");
        const ToolOutput out = ToolsRegistry::instance().run_output("task", call);
        if (out.output.find("[ошибка] task") != std::string::npos ||
            out.output.find("Обзор короткий.") == std::string::npos) {
            std::cerr << "  имя с пробелами и в верхнем регистре не нашло агента: "
                      << out.output << std::endl;
        }
        ASSERT_TRUE(out.output.find("[ошибка] task") == std::string::npos);
        ASSERT_TRUE(out.output.find("Обзор короткий.") != std::string::npos);
    }

    /* Отказы не дошли до модели: агент выбирается ДО запроса, иначе
     * несуществующее имя оплачивалось бы запросом к провайдеру. Единственный
     * запрос — от последней проверки, где имя нормализовалось. */
    ASSERT_EQ(fx.host.calls(), (size_t)1);
}

/* ======================================================================
 * 5. Поля, работа которых ещё не написана
 * ====================================================================== */

TEST(task_refuses_arguments_it_cannot_honour_yet) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* Объявленный в схеме параметр, который не делает ничего, — хуже
     * отсутствующего: модель считает, что продолжение задачи состоялось,
     * и ждёт результата. Отказ обязан называть поле. */
    struct Case {
        const char* field;
        const char* extra;
        const char* value;
    };
    const Case cases[] = {
        {"task_id", ",\n \"task_id\": \"abc-123\"", "task_id"},
        {"background", ",\n \"background\": true", "background"},
    };
    for (const Case& c : cases) {
        json::JsonValue call = json::JsonValue::object();
        call.set("description", "Проверка поля");
        call.set("prompt", "Ничего не делать.");
        call.set("subagent_type", "wp_general");
        if (std::string(c.field) == "task_id") call.set("task_id", "abc-123");
        else call.set("background", true);
        const ToolOutput out = ToolsRegistry::instance().run_output("task", call);
        if (out.output.find(c.value) == std::string::npos) {
            std::cerr << "  отказ по полю " << c.field << " не называет его: "
                      << out.output << std::endl;
        }
        ASSERT_TRUE(out.output.find(c.value) != std::string::npos);
        ASSERT_TRUE(out.output.find("[ошибка] task") != std::string::npos);
        ASSERT_TRUE(out.output.find("не поддерживается") != std::string::npos);
        (void)c.extra;
    }

    /* Пустые обязательные поля: схема проверяет НАЛИЧИЕ, а модель
     * присылает `""` — и без своей проверки инструмент пошёл бы дальше с
     * пустым заданием. */
    const char* required[] = {"description", "prompt", "subagent_type"};
    for (const char* field : required) {
        json::JsonValue call = json::JsonValue::object();
        call.set("description", "Проверка поля");
        call.set("prompt", "Ничего не делать.");
        call.set("subagent_type", "wp_general");
        call.set(field, "");
        const ToolOutput out = ToolsRegistry::instance().run_output("task", call);
        if (out.output.find(field) == std::string::npos) {
            std::cerr << "  пустое поле " << field << " не названо в отказе: "
                      << out.output << std::endl;
        }
        ASSERT_TRUE(out.output.find(field) != std::string::npos);
        ASSERT_TRUE(out.output.find("[ошибка] task") != std::string::npos);
    }

    ASSERT_EQ(fx.host.calls(), (size_t)0);
}

/* ======================================================================
 * 6. Глубина вложенности
 * ====================================================================== */

TEST(a_subagent_cannot_open_a_subagent_of_its_own) {
    TaskFixture fx;
    /* Ход субагента: он зовёт `task` ещё раз. Второй уровень обязан быть
     * отказан ДО запроса к модели — иначе `task` внутри `task` стал бы
     * рекурсивным генератором запросов. */
    fx.host.replies = {
        call_block("task", task_args("Вложенная", "Ещё глубже.", "wp_general")),
        "Вложенный субагент не понадобился: я сделал это сам." };
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Внешняя задача");
    call.set("prompt", "Сделай что-то.");
    call.set("subagent_type", "wp_general");
    const ToolOutput out = ToolsRegistry::instance().run_output("task", call);

    /* Два запроса: ход субагента и его следующий ход после отказа. Третьего
     * (вложенный субагент) нет. */
    ASSERT_EQ(fx.host.calls(), (size_t)2);
    if (fx.host.calls() != 2) {
        for (size_t i = 0; i < fx.host.calls(); ++i) {
            std::cerr << "  запрос " << i << ": " << fx.host.transcript(i).substr(0, 200)
                      << std::endl;
        }
    }
    /* Отказ виден СУБАГЕНТУ — он читает его как результат своего вызова
     * и потому не повторяет. */
    const std::string expected =
        std::string("Subagent depth limit reached (") +
        std::to_string(limits::kSubagentDepthLimit) + ")";
    if (fx.host.transcript(1).find(expected) == std::string::npos) {
        std::cerr << "  субагент не увидел отказ по глубине: "
                  << fx.host.transcript(1).substr(0, 300) << std::endl;
    }
    ASSERT_TRUE(fx.host.transcript(1).find(expected) != std::string::npos);
    /* Итог задачи — ответ субагента, а не текст отказа. */
    ASSERT_TRUE(out.output.find("сделал это сам") != std::string::npos);
    ASSERT_TRUE(out.output.find("depth limit") == std::string::npos);
}

/* ======================================================================
 * 7. Инструменты субагента проверяются ЕГО правилами
 * ====================================================================== */

TEST(subagent_tool_calls_are_judged_by_the_subagent_rules) {
    TaskFixture fx;
    AgentDef writer;
    writer.name = "wp_writer";
    writer.description = "Писатель";
    writer.mode = AgentMode::Subagent;
    writer.prompt = "Пишущий субагент.";
    PermissionEntry no_write;
    no_write.key = "write";
    no_write.action = PermissionAction::Deny;
    writer.permission.push_back(no_write);

    /* Порядок ответов: ход субагента зовёт write_file, затем его финал. */
    fx.host.replies = {
        call_block("write_file",
                   ",\n \"path\": \"izmeneno.txt\","
                   "\n \"content\": \"привет\""),
        long_answer("Записать не смог, вот почему.") };
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    std::string err;
    ASSERT_TRUE(AgentRegistry::instance().add(writer, &err));

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Правка файла");
    call.set("prompt", "Запиши файл.");
    call.set("subagent_type", "wp_writer");
    const ToolOutput out = ToolsRegistry::instance().run_output("task", call);

    /* Отказ пришёл от ENFORCEMENT, а не от промпта: субагент увидел его в
     * результате своего вызова. */
    const std::string seen = fx.host.transcript(1);
    if (seen.find("запрещён правилом") == std::string::npos) {
        std::cerr << "  субагент не увидел запрет write: " << seen.substr(0, 300)
                  << std::endl;
    }
    ASSERT_TRUE(seen.find("запрещён правилом") != std::string::npos);
    /* Файла нет: запрещённый вызов не должен был «успеть» до записи. */
    ASSERT_FALSE(fs::exists(fx.project / "izmeneno.txt"));
    /* Итог задачи — ответ субагента, а не пустота и не «готово». */
    ASSERT_TRUE(out.output.find("Записать не смог") != std::string::npos);

    /* Правила ребёнка не действуют на родителя: после вложенного хода
     * сессия по-прежнему может писать. Иначе один запрет в описании
     * субагента навсегда отнял бы запись у всего диалога. */
    ASSERT_EQ(std::string(permission_action_name(
                  engine().permissions().evaluate("write", "a.txt"))),
              std::string("разрешить"));
    const std::string parent_sys = engine().build_system_prompt();
    ASSERT_TRUE(catalogue_mentions(parent_sys, "write_file"));
}

TEST(an_always_answer_reaches_the_subagent_rules) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* Правила агента заморожены при его сборке (И8.4): они сложены из
     * правил СЕССИИ, какие были в тот момент. Ответ «всегда», который
     * пользователь дал ПОСЛЕ этого, обязан дойти и до ребёнка — иначе
     * кнопка «всегда» работала бы в сессии и не работала у субагента,
     * то есть спрашивала бы одно и то же по каждому вызову.
     *
     * Проверка сделана на ЧТЕНИИ, а не на команде, и это не оговорка: у
     * команды «всегда» сужает шаблон до «программа + первый не-флаг»
     * (И3.5) — для `echo привет` это «echo привет *», и та же самая
     * команда этим шаблоном НЕ покрывается (у значения не хватает
     * хвоста). То есть переспросит и сессия, и проверить доставку ответа
     * ребёнку на ней нельзя. У пути suggested равен самому пути, и
     * повторный вызов с теми же аргументами покрыт. */
    engine().permissions().add_rule(
        Rule{"read", "*", PermissionAction::Ask, "тест: чтение спрашивает"});

    fx.host.replies = {
        call_block("read_file", ",\n \"path\": \"odin.txt\""),
        call_block("read_file", ",\n \"path\": \"odin.txt\""),
        "Файл прочитан, больше нечего делать."};
    {
        std::ofstream f(fx.project / "odin.txt");
        f << "содержимое\n";
    }

    /* Пользователь нажимает «всегда» на КАЖДЫЙ вопрос и считает их. */
    std::atomic<int> asked{0};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        for (int i = 0; i < 300; ++i) {
            const std::vector<PermissionRequest> p =
                engine().permissions().pending();
            if (!p.empty() &&
                engine().permissions().reply(p.front().id, PermissionReply::Always)) {
                ++asked;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    Joiner joiner(threads);

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Две команды");
    call.set("prompt", "Запусти две команды.");
    call.set("subagent_type", "wp_general");
    const ToolOutput out = ToolsRegistry::instance().run_output("task", call);
    joiner.join();

    /* Оба вызова выполнены, и вопрос был ОДИН: второй вызов с теми же
     * аргументами субагент уже не переспрашивал. */
    if (asked.load() != 1) {
        std::cerr << "  вопросов о разрешении: " << asked.load()
                  << " (ожидался один: ответ «всегда» должен доходить до"
                     " правил субагента)" << std::endl;
    }
    ASSERT_EQ(asked.load(), 1);
    ASSERT_TRUE(out.output.find("больше нечего делать") != std::string::npos);
    /* Чтение действительно выполнилось: содержимое файла видно в
     * следующем запросе субагента. Иначе «вопрос был один» могло бы
     * означать, что оба вызова не дошли до инструмента. */
    ASSERT_TRUE(fx.host.transcript(2).find("содержимое") != std::string::npos);
    /* И у сессии правило появилось — «всегда» работает и там тоже. */
    ASSERT_EQ(std::string(permission_action_name(
                  engine().permissions().evaluate("read", "odin.txt"))),
              std::string("разрешить"));
}


TEST(subagent_does_not_disturb_the_parent_loop_detector) {
    TaskFixture fx;
    /* Три одинаковых чтения подряд: детектор зацикливания спрашивает
     * пользователя на ТРЕТЬЕМ. У субагента спрашивать нельзя (он не
     * разговаривает с пользователем), и вопрос не должен уходить вместо
     * родителя — но главное здесь другое: отпечатки вызовов ребёнка не
     * должны попасть в счётчик родителя, иначе следующие его вызовы
     * начали бы ловить «зацикливание», которого не было. */
    const std::string read = call_block("read_file", ",\n \"path\": \"odin.txt\"");
    fx.host.replies = {read, read, read, "Файл прочитан."};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    {
        std::ofstream f(fx.project / "odin.txt");
        f << "содержимое\n";
    }

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Чтение");
    call.set("prompt", "Прочитай файл.");
    call.set("subagent_type", "wp_general");
    /* Через ToolRunner, а не напрямую: отпечаток вызова ставит именно он,
     * и без него в счётчике родителя не было бы ничего — и проверять было
     * бы нечего. */
    ToolRunner runner(engine_state(), cb, [](AgentEvent::Kind, const std::string&) {});
    const ToolOutcome outcome = runner.run("task", call);
    ASSERT_TRUE(outcome.ok);
    ASSERT_TRUE(outcome.output.output.find("прочитан") != std::string::npos);

    /* Счётчик родителя: его собственный вызов `task` и НИ ОДНОГО
     * чтения ребёнка. */
    std::vector<std::string> parent_calls;
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        for (const std::string& s : engine_state().recent_calls) {
            parent_calls.push_back(s);
        }
    }
    if (parent_calls.size() != 1) {
        std::cerr << "  в счётчике родителя " << parent_calls.size()
                  << " записей:" << std::endl;
        for (const std::string& s : parent_calls) {
            std::cerr << "    " << s.substr(0, 60) << std::endl;
        }
    }
    ASSERT_EQ(parent_calls.size(), (size_t)1);
    ASSERT_TRUE(parent_calls[0].rfind("task", 0) == 0);
    /* Счётчик субагента умер вместе с областью: держать его в движке
     * незачем, а следующий субагент получил бы чужие отпечатки. */
    const RunScope after = engine().scope_snapshot();
    ASSERT_TRUE(after.loop_calls == nullptr);
}

/* ======================================================================
 * 9. Сбой провайдера у субагента — это сбой, а не успех
 * ====================================================================== */

TEST(subagent_provider_failure_looks_like_a_failure) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Проверка сбоя");
    call.set("prompt", "Ничего не делать.");
    call.set("subagent_type", "wp_general");

    /* (1) Провайдер не ответил. */
    fx.host.fail = true;
    ToolOutput out = ToolsRegistry::instance().run_output("task", call);
    if (out.output.find("[ошибка] task") == std::string::npos ||
        out.output.find("сеть недоступна") == std::string::npos) {
        std::cerr << "  сбой провайдера не назван: " << out.output << std::endl;
    }
    ASSERT_TRUE(out.output.find("[ошибка] task") != std::string::npos);
    ASSERT_TRUE(out.output.find("сеть недоступна") != std::string::npos);

    /* (2) Провайдер ответил ПУСТЫМ ответом: ok, ни текста, ни вызова.
     * Это другой отказ, и он обязан называться своим текстом: пустой
     * успешный результат прочитал бы вызывающий как «субагент отработал и
     * сказал, что делать нечего» (И6.8). */
    fx.host.fail = false;
    fx.host.replies = {""};
    out = ToolsRegistry::instance().run_output("task", call);
    if (out.output.find("[ошибка] task") == std::string::npos ||
        out.output.find("пустым") == std::string::npos) {
        std::cerr << "  пустой ответ пройден как успех: [" << out.output << "]"
                  << std::endl;
    }
    ASSERT_TRUE(out.output.find("[ошибка] task") != std::string::npos);
    ASSERT_TRUE(out.output.find("пустым") != std::string::npos);

    /* И область вернулась: неудавшийся субагент — не причина уйти
     * следующему ходу с его промптом. */
    const RunScope after = engine().scope_snapshot();
    ASSERT_EQ(after.agent, std::string(""));
    ASSERT_TRUE(after.info == nullptr);
    /* Промпт сессии не остался от субагента: в нём нет роли ребёнка. */
    ASSERT_TRUE(engine().build_system_prompt().find(kAgentGeneralPrompt)
                == std::string::npos);
}

/* ======================================================================
 * 10. Токены субагента — токены сессии
 * ====================================================================== */

TEST(subagent_tokens_are_counted_in_the_session_metrics) {
    TaskFixture fx;
    fx.host.replies = {"Субагент отработал."};
    fx.host.prompt_tokens = 100;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Учёт токенов");
    call.set("prompt", "Ничего не делать.");
    call.set("subagent_type", "wp_general");
    ToolsRegistry::instance().run_output("task", call);

    ASSERT_EQ(fx.host.calls(), (size_t)1);
    /* Имитатор честно сообщает usage: 100 входных и 20 выходных за
     * запрос. Числа взяты из ОТВЕТА имитатора, а не подобраны. */
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    ASSERT_EQ(engine_state().total_prompt_tokens, 100);
    ASSERT_EQ(engine_state().total_completion_tokens, 20);
    /* Измеренный вход — последний запрос: по нему решается, переполнено
     * ли окно (И7.2), и запрос субагента занимает то же место, что и
     * запрос родителя. */
    ASSERT_EQ(engine_state().measured_input_tokens, (long long)100);
}

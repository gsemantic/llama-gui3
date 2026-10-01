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
 *
 * Диагностика режет текст через text::utf8_prefix, а НЕ substr: текст
 * кириллический, substr режет по БАЙТАМ и в stderr попадает обрывок
 * UTF-8-символа. Это не косметика — прогон мутаций читает stderr как
 * текст и на таком обрывке падал с UnicodeDecodeError, то есть прогон
 * выглядел сломанным из-за диагностики (правило 13, кириллица).
 */

#include "test_framework.h"
#include "test_printers.h"
#include "test_support.h"
#include "../core/agent_components.h"
#include "../core/agent_registry.h"
#include "../core/base_tools.h"
#include "../core/engine.h"
#include "../core/git_tools.h"
#include "../core/json_utils.h"   /* text::utf8_prefix — см. правило 13 */
#include "../core/limits.h"
#include "../core/prompts.h"
#include "../core/session_store.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"
#include "../modules/wordpress/wp_tools.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
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
    /* WP- и git-инструменты нужны проверкам И8.15 не «для полноты», а
     * чтобы отрицание было содержательным: без них в каталоге нет ни
     * deploy, ни wp_db, и проверка «у wp_theme нет deploy» прошла бы на
     * отсутствии инструмента вообще — то есть проверяла бы пустоту. */
    wp::register_wp_tools();
    register_git_tools();
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
    /* И8.12: ждать отмены вместо ответа. Имитирует ЖИВОЙ запрос: без
     * этого «отмена родителя гасит ребёнка» нельзя проверить, потому что
     * мгновенный ответ нечего отменять — и проверка прошла бы при любом
     * коде. Предикат один с настоящим (state_.abort_requested), как в
     * plugin_main.cpp. */
    bool wait_for_abort = false;
    std::atomic<bool> entered{false};
    std::atomic<bool> saw_abort{false};

    bool chat(const std::string& sys, const std::vector<ModelMessage>& msgs,
              LlmReply& out) {
        sys_prompts.push_back(sys);
        requests.push_back(msgs);
        if (wait_for_abort) {
            entered.store(true);
            for (int i = 0; i < 2000; ++i) {
                if (engine_state().abort_requested.load()) {
                    saw_abort.store(true);
                    /* Так отвечает блокирующий вызов хоста при отмене
                     * ожидания (И6.6): провайдер не виноват. */
                    out.error = "Прервано пользователем";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            out.error = "хост без ответа";
            return false;
        }
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

/* Предел вложенности, каким его видит инструмент `task` (И8.8). */
int depth_limit() {
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    return engine_state().subagent_depth;
}

/* Очередь ответов имитатора — сброс между случаями одного теста. Без него
 * счётчик запросов и накопленные транскрипты прошлого случая сделали бы
 * числа следующего чужими, и проверка прошла бы сама по себе. */
void reset_host(FakeHost& host, std::vector<std::string> replies) {
    host.replies = replies;
    host.next = 0;
    host.sys_prompts.clear();
    host.requests.clear();
}

/* Агент, который САМ разрешил делегирование (И8.10).
 *
 * С 8.10 субагент по умолчанию не может звать `task` — авто-запрет в
 * Info::for_subagent. Проверки лимита глубины поэтому обязаны работать на
 * агенте с явным `task: allow`: иначе они проверяли бы авто-запрет, а
 * лимит глубины не проверялся бы вовсе — то есть его можно было бы
 * сломать, и ничего бы не заметил (то же, что с проверкой «его зовут»). */
bool add_delegator() {
    AgentDef def;
    def.name = "wp_delegator";
    def.description = "Субагент, которому разрешено делегировать";
    def.mode = AgentMode::Subagent;
    def.prompt = "Субагент с правом делегировать.";
    PermissionEntry read;
    read.key = "read";
    read.action = PermissionAction::Allow;
    def.permission.push_back(read);
    PermissionEntry task;
    task.key = "task";
    task.action = PermissionAction::Allow;
    def.permission.push_back(task);
    std::string err;
    if (!AgentRegistry::instance().add(def, &err)) {
        std::cerr << "  агент wp_delegator не зарегистрирован: " << err
                  << std::endl;
        return false;
    }
    return true;
}

/* Агент, которому разрешены команды: нужен там, где проверяется отмена
 * ВНУТРИ инструмента. На агенте без правил `bash` сначала спросил бы
 * разрешения, и команда не была бы запущена вовсе — то есть проверялось бы
 * не то. */
bool add_command_agent() {
    AgentDef def;
    def.name = "wp_commander";
    def.description = "Субагент, которому разрешены команды";
    def.mode = AgentMode::Subagent;
    def.prompt = "Запускаю команды.";
    PermissionEntry bash;
    bash.key = "bash";
    bash.action = PermissionAction::Allow;
    def.permission.push_back(bash);
    std::string err;
    if (!AgentRegistry::instance().add(def, &err)) {
        std::cerr << "  агент wp_commander не зарегистрирован: " << err
                  << std::endl;
        return false;
    }
    return true;
}

/* Агент без единого правила: всё, что не запрещено и не разрешено его
 * собственными правилами, у него спрашивается (И8.10 — субагент не
 * наследует разрешения сессии). */
bool add_bare_agent(const std::string& name, const std::string& prompt) {
    AgentDef def;
    def.name = name;
    def.description = "Агент без правил";
    def.mode = AgentMode::Subagent;
    def.prompt = prompt;
    std::string err;
    if (!AgentRegistry::instance().add(def, &err)) {
        std::cerr << "  агент " << name << " не зарегистрирован: " << err
                  << std::endl;
        return false;
    }
    return true;
}

/* Окружение одного теста. Восстанавливает ВСЁ, что меняет вложенный
 * ход: область, промпт, историю, режим, проект, лимиты, разрешения и
 * реестр агентов (правило 5 SESSION_START). */
struct TaskFixture {
    FakeHost host;
    PermissionGuard permissions;
    fs::path project;
    /* Настройки плагина для этого теста (И8.8). Не пустая карта, а
     * отвечающий хост: значения читает Engine::load_settings, то есть
     * проверка идёт по НАСТОЯЩЕМУ чтению настройки, а не по подставленному
     * в поле число. */
    std::map<std::string, std::string> settings;

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
        /* Предел вложенности (И8.8) и идентификатор сессии (И8.9) — тоже
         * состояние синглтона, и настройка одного теста не должна пережить
         * его: идентификатор, выданный дочерней сессией, иначе сделал бы
         * чужую задачу «своей» для следующего теста, а это ровно тот
         * отказ, который проверяет 8.9. В prepare() они НЕ сбрасываются:
         * prepare() идёт ПОСЛЕ init(), который прочитал настройки, и
         * сброс стёр бы ровно то, что проверяется. */
{
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().subagent_depth = limits::kSubagentDepthLimit;
            engine_state().session_id.clear();
            /* И8.14: очередь фоновых задач и счётчик цепочки автоматических
             * ходов — тоже состояние синглтона. Задача, оставленная в
             * очереди тестом, выполнилась бы в СЛЕДУЮЩЕМ тесте (движок
             * разбирает очередь между ходами) и испортила его, а счётчик
             * цепочки съел бы один из лишних ходов. */
engine_state().background_tasks.clear();
            engine_state().background_turns = 0;
            /* Прерванный тестом токен не переживает тест: следующий увидел
             бы «отменено» и не смог бы ничего запустить. */
            engine_state().turn_abort.reset();
            /* И флага отмены: тест с «стопом» оставляет его выставленным,
             * и следующий тест, забывший prepare(), увидел бы отменённый
             * ход и не смог бы ничего сделать. */
            engine_state().abort_requested.store(false);
        }


        reset_registry();
    }

    HostCallbacks callbacks() {
        HostCallbacks cb;
        FakeHost* h = &host;
        TaskFixture* self = this;
        cb.llm_chat = [h](const std::string& sys,
                          const std::vector<ModelMessage>& msgs,
                          LlmReply& out) {
            return h->chat(sys, msgs, out);
        };
        cb.llm_is_connected = []() { return true; };
        cb.path_data_dir = [this] { return project.string(); };
        cb.settings_set = [](const std::string&, const std::string&) {};
        cb.settings_get = [self](const std::string& key, const std::string& d) {
            auto it = self->settings.find(key);
            return it != self->settings.end() ? it->second : d;
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
        /* И8.14: очередь фоновых задач — с начала теста пустая (см.
         * деструктор). Иначе задача, оставленная в очереди другим тестом,
         * выполнилась бы в этом и испортила его. */
        engine_state().background_tasks.clear();
        engine_state().background_turns = 0;
        /* Свой токен отмены на каждый тест — ровно как это делает submit
         * (И6.7). Без него у инструментов не было бы ЧЕГО отменять
         * (`ctx.abort()` вернул бы nullptr), и проверка «отмена убивает
         * команду ребёнка» тихо проверяла бы разрешение, а не токен:
         * `bash` спросил бы, вопрос отпустила бы отмена — и команда не
         * была бы запущена вовсе. */
        engine_state().turn_abort = std::make_shared<AbortToken>();
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

/* Есть ли в тексте подстрока, и если нет — где искать: диагностика на
 * кириллице обязана печатать её через text::utf8_prefix, а не substr
 * (правило 13: cut/substr режут по БАЙТАМ). */
bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

/* Корень плагина и чтение файла — как в остальных проверках этого
 * репозитория: каждый файл тестов держит свои копии (общий заголовок ради
 * двух строк дороже дублирования). */
fs::path plugin_root() {
    return fs::path(__FILE__).parent_path().parent_path();
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* Текст без комментариев. Нужно, чтобы guard ловил ВЫЗОВ, а не упоминание
 * в прозе: иначе строка «deliver_background_tasks()» в комментарии
 * удовлетворила бы проверку, ничего не проверяя (та же оговорка, что в
 * test_manifest_consistency: кавычки не разбираются). */
std::string without_comments(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    bool in_block = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (in_block) {
            if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/') {
                in_block = false;
                ++i;
            } else {
                r += (s[i] == '\n' ? '\n' : ' ');
            }
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            in_block = true;
            ++i;
            r += "  ";
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') { r += ' '; ++i; }
            r += '\n';
            continue;
        }
        r += s[i];
    }
    return r;
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
     * два необязательных поля. Оба необязательных РАБОТАЮТ: `task_id`
     * продолжение (И8.9), `background` — фон (И8.14). */
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
     * task_id/background необязательны (продолжение и фон — по желанию). */
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
            std::cerr << "  запрос " << i << ": " << text::utf8_prefix(fx.host.transcript(i), 200)
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
            std::cerr << "  запрос " << i << ": " << text::utf8_prefix(fx.host.transcript(i), 200)
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

/* Список субагентов в тексте описания — та же функция, что и в проверках
 * ниже по файлу, объявлена здесь, потому что WP-проверки идут раньше её
 * определения. */
std::string subagent_list_from(const std::string& prompt);

/* ======================================================================
 * 4. WP-субагенты (И8.15): что реально уходит в модель
 * ====================================================================== */

/* Проверки ниже смотрят на СОБЫТИЕ ВЫЗОВА, а не на таблицу встроенных
 * агентов: права WP-агента имеют смысл только там, где доходят до
 * модели. Сверка «в таблице есть deny» проверяла бы наше же объявление,
 * а не то, что модель получила: запрет, не дошедший до каталога, —
 * это модель, которая тратит шаг на заведомо отклонённый вызов. */

TEST(a_wp_theme_subagent_gets_its_prompt_and_its_tool_catalogue) {
    TaskFixture fx;
    /* Первый ответ имитатора достаётся СУБАГЕНТУ: вызов делает сам тест. */
    fx.host.replies = {"Готово: правил style.css и functions.php."};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    const std::string parent_sys = engine().build_system_prompt();
    /* Вызывающий должен знать, кого зовут: иначе WP-специалисты были бы
     * зарегистрированы, но недостижимы — мёртвый код по дороге. */
    const std::string list = subagent_list_from(parent_sys);
    ASSERT_TRUE(list.find("wp_theme") != std::string::npos);
    ASSERT_TRUE(list.find("Субагент по темам") != std::string::npos);

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Тема");
    call.set("prompt", "Оформи тему.");
    call.set("subagent_type", "wp_theme");
    const ToolOutput out = ToolsRegistry::instance().run_output("task", call);
    ASSERT_EQ(fx.host.calls(), (size_t)1);

    /* ЧТО УШЛО В МОДЕЛЬ: системный промпт ребёнка собран по ЕГО правилам и
     * несёт ЕГО промпт. Чужой промпт в нём означал бы, что агент получил
     * инструкцию другой роли (все четыре — с разными границами). */
    const std::string child_sys = fx.host.sys(0);
    if (child_sys.find(kAgentWpThemePrompt) == std::string::npos) {
        std::cerr << "  в промпте ребёнка нет wp_theme: "
                  << text::utf8_prefix(child_sys, 400) << std::endl;
    }
    ASSERT_TRUE(child_sys.find(kAgentWpThemePrompt) != std::string::npos);
    ASSERT_TRUE(child_sys.find(kAgentWpDeployPrompt) == std::string::npos);
    ASSERT_TRUE(child_sys.find(kAgentWpHookPrompt) == std::string::npos);

    /* Каталог по роли: автор пишет и проверяет синтаксис… */
    ASSERT_TRUE(catalogue_mentions(child_sys, "write_file"));
    ASSERT_TRUE(catalogue_mentions(child_sys, "apply_patch"));
    ASSERT_TRUE(catalogue_mentions(child_sys, "php_lint"));
    ASSERT_TRUE(catalogue_mentions(child_sys, "skill_detail"));
    /* …но не выкладывает и не ходит в данные: это граница роли, и запрет
     * целиком убирает инструмент из каталога (И2.8), а не прячет за
     * отказом — иначе модель тратила бы шаг впустую. */
    if (catalogue_mentions(child_sys, "deploy")) {
        std::cerr << "  у wp_theme в каталоге deploy" << std::endl;
    }
    ASSERT_FALSE(catalogue_mentions(child_sys, "deploy"));
    ASSERT_FALSE(catalogue_mentions(child_sys, "wp_db"));

    /* У вызывающего ничего не отнято: сужение ребёнка не утекает в
     * сессию (общий набор инструментов — синглтон процесса). */
    ASSERT_TRUE(catalogue_mentions(parent_sys, "deploy"));
    ASSERT_EQ(engine().build_system_prompt(), parent_sys);
}

TEST(the_wp_deploy_subagent_is_refused_the_tools_that_change_files) {
    TaskFixture fx;
    /* Два ответа, и оба достаются ребёнку: родитель в этом тесте не ходит
     * вовсе. Первый — вызов запрещённого инструмента, второй — ответ после
     * отказа. Третьего ответа нет намеренно: ребёнок завершает ход
     * текстом без вызова, а лишняя запись в очереди была бы числом,
     * которое никто не проверит. */
    fx.host.replies = {
        call_block("write_file",
                   ", \"path\": \"/srv/site/wp-content/themes/t/style.css\","
                   " \"content\": \"x\""),
        "Понял, писать не буду: файлы не трогал."};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* Каталог родителя снимаем ДО вложенного хода — это эталон для
     * сравнения «после». */
    const std::string parent_sys = engine().build_system_prompt();

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Выкладка");
    call.set("prompt", "Выложи.");
    call.set("subagent_type", "wp_deploy");
    const ToolOutput out = ToolsRegistry::instance().run_output("task", call);

    const std::string child_sys = fx.host.sys(0);
    ASSERT_TRUE(child_sys.find(kAgentWpDeployPrompt) != std::string::npos);
    /* Выкладка не пишет код: write_file у неё нет НИ В КАТАЛОГЕ, ни в
     * правах. Проверяется обеими сторонами, потому что это две разные
     * вещи: правило без каталога — модель зря тратит шаг, каталог без
     * правила — обход на уровне enforcement. */
    ASSERT_FALSE(catalogue_mentions(child_sys, "write_file"));
    ASSERT_FALSE(catalogue_mentions(child_sys, "apply_patch"));
    ASSERT_FALSE(catalogue_mentions(child_sys, "edit_file"));
    /* А то, ради чего он существует, — на месте. */
    ASSERT_TRUE(catalogue_mentions(child_sys, "deploy"));
    ASSERT_TRUE(catalogue_mentions(child_sys, "verify"));

    /* ЧТО ПРИШЛО В ИНСТРУМЕНТ: ребёнку отказали по правилу, и отказ
     * попал в ЕГО транскрипт — то есть до модели, а не в stderr. */
    /* Отказ виден ребёнку ВО ВТОРОМ запросе: первый — это исходная
     * задача, а вызов инструмента и его результат попадают в следующий.
     * Индекс 0 здесь прошёл бы на «вызова не было». */
    const std::string child_talk = fx.host.transcript(1);
    const size_t last = child_talk.rfind("write_file");
    if (last == std::string::npos) {
        std::cerr << "  ребёнок не звал write_file: "
                  << text::utf8_prefix(child_talk, 400) << std::endl;
    } else {
        const std::string tail = child_talk.substr(last);
        if (tail.find("запрещён правилом") == std::string::npos) {
            std::cerr << "  после write_file нет отказа: "
                      << text::utf8_prefix(tail, 300) << std::endl;
        }
        ASSERT_TRUE(tail.find("запрещён правилом") != std::string::npos);
    }
    ASSERT_TRUE(last != std::string::npos);
    /* Отказ ребёнка не превращается в отказ родителя: у вызывающего
     * write_file на месте и после вложенного хода. */
    ASSERT_TRUE(catalogue_mentions(parent_sys, "write_file"));
    ASSERT_EQ(engine().build_system_prompt(), parent_sys);
    /* Отказ НЕ убивает ход ребёнка: он увидел отказ, продолжил и вернул
     * итог. Иначе проверка выше прошла бы и при падении инструмента —
     * то есть проверяла бы не запрет, а аварийное завершение. */
    if (out.output.find("файлы не трогал") == std::string::npos) {
        std::cerr << "  ребёнок не вернулся после отказа: "
                  << text::utf8_prefix(out.output, 300) << std::endl;
    }
    ASSERT_TRUE(out.output.find("файлы не трогал") != std::string::npos);
}

TEST(a_wp_subagent_cannot_undo_what_its_own_role_forbids) {
    TaskFixture fx;
    /* Ребёнок пробует то, чего у него в каталоге нет: wp_db. Отказ
     * обязан дойти до модели, иначе сужение правил — декорация. */
    fx.host.replies = {
        call_block("wp_db", ", \"query\": \"SELECT 1\""),
        "Понял, к базе не хожу.",
        "Готово: тему поправил, базу не трогал."};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Правка темы");
    call.set("prompt", "Поправь тему.");
    call.set("subagent_type", "wp_hook");
    ToolsRegistry::instance().run_output("task", call);

    const std::string child_sys = fx.host.sys(0);
    ASSERT_TRUE(child_sys.find(kAgentWpHookPrompt) != std::string::npos);
    ASSERT_FALSE(catalogue_mentions(child_sys, "wp_db"));

    const std::string child_talk = fx.host.transcript(1);
    const size_t last = child_talk.rfind("wp_db");
    ASSERT_TRUE(last != std::string::npos);
    const std::string tail = child_talk.substr(last);
    if (tail.find("запрещён правилом") == std::string::npos) {
        std::cerr << "  после wp_db нет отказа: "
                  << text::utf8_prefix(tail, 300) << std::endl;
    }
    ASSERT_TRUE(tail.find("запрещён правилом") != std::string::npos);
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
 * 5. Пустые обязательные поля
 *
 * И8.9 убрал из этого списка `task_id` (проверки на его отказы — в
 * разделе 12), И8.14 — `background` (работа написана, проверки — в
 * разделе 19). Осталось то, что и было задумано: объявленный параметр,
 * который нельзя выполнить, обязан быть назван в отказе.
 * ====================================================================== */

TEST(task_refuses_arguments_it_cannot_honour_yet) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

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
                      << text::utf8_prefix(out.output, 200) << std::endl;
        }
        ASSERT_TRUE(out.output.find(field) != std::string::npos);
        ASSERT_TRUE(out.output.find("[ошибка] task") != std::string::npos);
    }

    /* Отказ не дошёл до модели: агент выбирается ДО запроса, иначе
     * заведомо пустая задача оплачивалась бы запросом к провайдеру. */
    ASSERT_EQ(fx.host.calls(), (size_t)0);
    /* И никакой работы не заведено: пустой `prompt` — это отказ ДО
     * постановки, а не фоновая задача с пустым заданием (И8.14). */
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        ASSERT_TRUE(engine_state().background_tasks.empty());
    }
}

/* ======================================================================
 * 6. Глубина вложенности
 * ====================================================================== */

TEST(a_subagent_cannot_open_a_subagent_of_its_own) {
    TaskFixture fx;
    /* Ход субагента: он зовёт `task` ещё раз. Второй уровень обязан быть
     * отказан ДО запроса к модели — иначе `task` внутри `task` стал бы
     * рекурсивным генератором запросов.
     *
     * Агент — делегирующий (И8.10): по умолчанию субагент не может звать
     * `task` вовсе, и тогда до лимита глубины дело не доходит, то есть
     * проверка глубины проверяла бы авто-запрет, а не предел. */
    reset_host(fx.host, {
        call_block("task", task_args("Вложенная", "Ещё глубже.", "wp_delegator")),
        "Вложенный субагент не понадобился: я сделал это сам." });
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    ASSERT_TRUE(add_delegator());

    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Внешняя задача");
    call.set("prompt", "Сделай что-то.");
    call.set("subagent_type", "wp_delegator");
    const ToolOutput out = ToolsRegistry::instance().run_output("task", call);

    /* Два запроса: ход субагента и его следующий ход после отказа. Третьего
     * (вложенный субагент) нет. */
    ASSERT_EQ(fx.host.calls(), (size_t)2);
    if (fx.host.calls() != 2) {
        for (size_t i = 0; i < fx.host.calls(); ++i) {
            std::cerr << "  запрос " << i << ": " << text::utf8_prefix(fx.host.transcript(i), 200)
                      << std::endl;
        }
    }
    /* Отказ виден СУБАГЕНТУ — он читает его как результат своего вызова
     * и потому не повторяет.
     *
     * Число в отказе — 1, а НЕ константа limits::kSubagentDepthLimit:
     * настройки нет, и проверка обязана утверждать, что дефолт равен
     * единице, а не «какому бы ни было дефолту». Сама константа
     * закреплена в limits_file_is_single_source_of_truth. */
    const std::string expected = "Subagent depth limit reached (1).";
    if (fx.host.transcript(1).find(expected) == std::string::npos) {
        std::cerr << "  субагент не увидел отказ по глубине: "
                  << text::utf8_prefix(fx.host.transcript(1), 300) << std::endl;
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
        std::cerr << "  субагент не увидел запрет write: " << text::utf8_prefix(seen, 300)
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
    /* Агент БЕЗ правил (И8.10): субагент не наследует разрешения сессии,
     * поэтому чтение у него спрашивается само по себе. На встроенном
     * агенте с `read: allow` вопроса не было бы — и проверка доставки
     * ответа «всегда» прошла бы вхолостую, ничего не дожидаясь. */
    ASSERT_TRUE(add_bare_agent("wp_asker", "Субагент, у которого спрашивают."));

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
    call.set("subagent_type", "wp_asker");
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
            std::cerr << "    " << text::utf8_prefix(s, 60) << std::endl;
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

    /* И8.11: сбой приходит ТОЙ ЖЕ обёрткой, но с state="error".
     * Признак «это не успех» больше не ищется по слову «ошибка» в тексте,
     * а читается из атрибута: раньше проверка искала подстроку, и форма
     * отказа могла бы съесть это слово незаметно для кода. */
    const std::string error_state = "state=\"error\"";

    /* (1) Провайдер не ответил. */
    fx.host.fail = true;
    ToolOutput out = ToolsRegistry::instance().run_output("task", call);
    if (out.output.find(error_state) == std::string::npos ||
        out.output.find("сеть недоступна") == std::string::npos) {
        std::cerr << "  сбой провайдера не назван: "
                  << text::utf8_prefix(out.output, 200) << std::endl;
    }
    ASSERT_TRUE(out.output.find(error_state) != std::string::npos);
    ASSERT_TRUE(out.output.find("сеть недоступна") != std::string::npos);
    /* Идентификатор задачи в отказе тоже есть: задача была создана, и её
     * можно продолжить (И8.9) — а тест без него проверял бы только
     * «красный текст», и потеря task_id прошла бы молча. */
    ASSERT_TRUE(out.output.find(out.metadata.get_string("session_id")) !=
                std::string::npos);

    /* (2) Провайдер ответил ПУСТЫМ ответом: ok, ни текста, ни вызова.
     * Это другой отказ, и он обязан называться своим текстом: пустой
     * успешный результат прочитал бы вызывающий как «субагент отработал и
     * сказал, что делать нечего» (И6.8). */
    fx.host.fail = false;
    fx.host.replies = {""};
    out = ToolsRegistry::instance().run_output("task", call);
    if (out.output.find(error_state) == std::string::npos ||
        out.output.find("пустым") == std::string::npos) {
        std::cerr << "  пустой ответ пройден как успех: ["
                  << text::utf8_prefix(out.output, 200) << "]" << std::endl;
    }
    ASSERT_TRUE(out.output.find(error_state) != std::string::npos);
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

/* ======================================================================
 * 11. И8.8: предел вложенности настраивается
 *
 * Проверки идут по ТОМУ, ЧТО ВИДИТ МОДЕЛЬ, а не по полю состояния:
 * настройка читается в Engine::load_settings, поэтому «значение
 * прочиталось» доказывается отказом на третьем уровне и её отсутствием
 * на втором, а не утверждением «state.subagent_depth == 2».
 * ====================================================================== */

/* Запуск одного вызова `task` с текущими настройками и ответами имитатора.
 *
 * Агент — делегирующий (add_delegator): с 8.10 субагент по умолчанию не
 * может звать `task`, и проверки лимита глубины на агенте без такого
 * права проверяли бы авто-запрет вместо предела. */
ToolOutput run_task_call() {
    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Задача родителя");
    call.set("prompt", "Сделай что-то.");
    call.set("subagent_type", "wp_delegator");
    return ToolsRegistry::instance().run_output("task", call);
}

TEST(subagent_depth_setting_decides_how_deep_delegation_goes) {
    TaskFixture fx;
    fx.settings["wp_coder.subagent_depth"] = "2";
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    ASSERT_TRUE(add_delegator());
    ASSERT_EQ(depth_limit(), 2);

    /* Предел 2: субагент (глубина 1) вправе звать субагента (глубина 2).
     * Очередь ответов общая для всех уровней, а ПЕРВЫЙ достаётся тому,
     * кто спросил, — то есть ходу субагента (И8.7):
     *   1 — ход субагента зовёт `task`;
     *   2 — ход вложенного субагента (его ответ виден только через
     *       транскрипт запроса 3, поэтому он назван так, как его должен
     *       прочитать вызывающий);
     *   3 — ход субагента после результата. */
    reset_host(fx.host, {
        call_block("task", task_args("Вложенная", "Ещё глубже.", "wp_delegator")),
        "ВНУТРЕННИЙ_ОТВЕТ",
        "Вложенный субагент сказал: ВНУТРЕННИЙ_ОТВЕТ"});

    const ToolOutput out = run_task_call();

    /* Три запроса: ход субагента, ход вложенного, финальный ход субагента.
     * С дефолтом (1) их было бы два — отказ пришёл бы вместо вложенного
     * хода. */
    if (fx.host.calls() != 3) {
        std::cerr << "  предел 2: запросов " << fx.host.calls()
                  << " (ожидалось 3), ответы по порядку:"
                  << std::endl;
        for (size_t i = 0; i < fx.host.calls(); ++i) {
            std::cerr << "    запрос " << i << ": "
                      << text::utf8_prefix(fx.host.transcript(i), 160) << std::endl;
        }
    }
    ASSERT_EQ(fx.host.calls(), (size_t)3);
    /* Ответа вложенного субагента нет в ИТОГЕ (итог — это финал субагента),
     * но он обязан быть в транскрипте его следующего запроса: иначе
     * «три запроса» означали бы, что ребёнок отработал вхолостую. */
    if (fx.host.transcript(2).find("ВНУТРЕННИЙ_ОТВЕТ") == std::string::npos) {
        std::cerr << "  ответ вложенного не дошёл до модели субагента: "
                  << text::utf8_prefix(fx.host.transcript(2), 300) << std::endl;
    }
    ASSERT_TRUE(fx.host.transcript(2).find("ВНУТРЕННИЙ_ОТВЕТ") !=
                std::string::npos);
    ASSERT_TRUE(out.output.find("ВНУТРЕННИЙ_ОТВЕТ") != std::string::npos);
    /* Отказа по глубине не было вовсе — предел это допускает. */
    for (size_t i = 0; i < fx.host.calls(); ++i) {
        if (fx.host.transcript(i).find("depth limit") != std::string::npos ||
            fx.host.sys(i).find("depth limit") != std::string::npos) {
            std::cerr << "  при пределе 2 отказ по глубине в запросе " << i
                      << std::endl;
        }
        ASSERT_TRUE(fx.host.transcript(i).find("depth limit") ==
                    std::string::npos);
        ASSERT_TRUE(fx.host.sys(i).find("depth limit") == std::string::npos);
    }
    /* Глубина верхнего вызова — 1, и это видно по метаданным: они
     * достались бы читателю события, поэтому число там не выдумано. */
    ASSERT_EQ(out.metadata.get_int("depth", 0), (long long)1);
    /* Область после вложенных ходов вернулась в сессию — иначе следующий
     * ход родителя пошёл бы с чужой глубиной. */
    const RunScope after = engine().scope_snapshot();
    ASSERT_EQ(after.depth, 0);
    ASSERT_EQ(after.agent, std::string(""));
}

TEST(subagent_depth_refusal_names_the_configured_limit) {
    TaskFixture fx;
    fx.settings["wp_coder.subagent_depth"] = "2";
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    ASSERT_TRUE(add_delegator());
    ASSERT_EQ(depth_limit(), 2);

    /* Тот же предел 2, но глубина 3 запрещена. Очередь:
     *   1 — субагент зовёт `task` (глубина 2, разрешено);
     *   2 — вложенный субагент зовёт `task` (глубина 3, отказ);
     *   3 — его следующий ход: здесь отказ ОБЯЗАН быть виден модели;
     *   4 — финал субагента. */
    reset_host(fx.host, {
        call_block("task", task_args("Вложенная", "Ещё глубже.", "wp_delegator")),
        call_block("task", task_args("Третья", "Совсем глубоко.", "wp_delegator")),
        "Третий уровень не понадобился.",
        "Вложенный субагент отработал."});

    const ToolOutput out = run_task_call();

    if (fx.host.calls() != 4) {
        std::cerr << "  предел 2, попытка глубины 3: запросов "
                  << fx.host.calls() << " (ожидалось 4)" << std::endl;
    }
    ASSERT_EQ(fx.host.calls(), (size_t)4);
    /* Отказ виден модели вложенного субагента — по ТРАНСКРИПТУ его второго
     * запроса, а не по счётчику: именно это чинит повторный вызов. */
    const std::string seen = fx.host.transcript(2);
    if (seen.find("Subagent depth limit reached (2).") == std::string::npos) {
        std::cerr << "  отказ по глубине 3 не назван настроенным пределом (2): "
                  << text::utf8_prefix(seen, 300) << std::endl;
    }
    ASSERT_TRUE(seen.find("Subagent depth limit reached (2).") !=
                std::string::npos);
    /* И НЕ назван дефолтом: константа в тексте отказа означала бы, что
     * модель предлагает человеку поднять несуществующую настройку. */
    ASSERT_TRUE(seen.find("depth limit reached (1)") == std::string::npos);
    /* Название настройки — чтобы модель могла сказать человеку, что
     * поднять, а не «не вышло». */
    ASSERT_TRUE(seen.find("wp_coder.subagent_depth") != std::string::npos);
    ASSERT_TRUE(out.output.find("Вложенный субагент отработал") !=
                std::string::npos);
}

TEST(broken_subagent_depth_setting_keeps_the_default_limit) {
    TaskFixture fx;

    /* Дефолт ПОЛЯ — тоже число, и проверяется без движка: EngineState
     * конструируется сам по себе, и дефолтом «0» сессия, которая ничего
     * не настраивала, получила бы запрет делегирования (то есть
     * настройка, которой нет, вела бы себя как выключенная). */
    const EngineState fresh;
    ASSERT_EQ(fresh.subagent_depth, limits::kSubagentDepthLimit);
    ASSERT_TRUE(add_delegator());   /* run_task_call зовёт именно его */

    /* Неразобранная настройка НЕ должна читаться как «предела нет»:
     * это вернуло бы ровно то, ради чего предел написан (И8.7).
     * Проверяется не «значение поля», а поведение: на втором уровне
     * делегирование по-прежнему отказано с дефолтом 1.
     *
     * «3000000000» в списке — не опечатка в тесте: число помещается в
     * long long и не помещается в int, то есть это единственный случай,
     * где «разобралось» и « годится» — разные вещи. */
    const char* broken[] = {"", "abc", "-3", "1.5", "2abc", "1 2", "0x2",
                            "3000000000"};
    for (const char* value : broken) {
        fx.settings["wp_coder.subagent_depth"] = value;
        HostCallbacks cb = fx.callbacks();
        engine().init(cb);
        fx.prepare();
        reset_registry();
        /* Реестр пересоздаётся в каждом случае, и делегирующий агент
         * регистрируется после него же: зарегистрированный раньше агент
         * исчез бы, и верхний вызов отказался бы «агент не найден» —
         * то есть проверка прошла бы мимо своего предмета. */
        ASSERT_TRUE(add_delegator());
        if (depth_limit() != 1) {
            std::cerr << "  настройка \"" << value << "\" дала предел "
                      << depth_limit() << " вместо 1" << std::endl;
        }
        ASSERT_EQ(depth_limit(), 1);

        reset_host(fx.host, {
            call_block("task",
                       task_args("Вложенная", "Ещё глубже.", "wp_general")),
            "Сделал сам."});
        const ToolOutput out = run_task_call();
        if (fx.host.calls() != 2 ||
            fx.host.transcript(1).find("Subagent depth limit reached (1).") ==
                std::string::npos) {
            std::cerr << "  настройка \"" << value
                      << "»: запросов " << fx.host.calls()
                      << ", отказ виден: "
                      << (fx.host.calls() > 1 &&
                          fx.host.transcript(1).find("depth limit") !=
                              std::string::npos)
                      << std::endl;
        }
        ASSERT_EQ(fx.host.calls(), (size_t)2);
        ASSERT_TRUE(fx.host.transcript(1).find(
                        "Subagent depth limit reached (1).") !=
                    std::string::npos);
        ASSERT_TRUE(out.output.find("Сделал сам") != std::string::npos);
    }
}

TEST(subagent_depth_zero_forbids_delegation_entirely) {
    TaskFixture fx;
    fx.settings["wp_coder.subagent_depth"] = "0";
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    ASSERT_TRUE(add_delegator());
    ASSERT_EQ(depth_limit(), 0);

    /* Ноль — не мусор, а запрет делегирования вообще: подменить его
     * дефолтом значило бы сделать «субагентов нет» неотличимым от
     * опечатки. Отказ — до запроса к модели, как и любой отказ выбора
     * агента (несуществующее имя тоже не оплачивается запросом). */
    reset_host(fx.host, {"Сделал сам."});
    const ToolOutput out = run_task_call();

    if (fx.host.calls() != 0 ||
        out.output.find("Subagent depth limit reached (0).") ==
            std::string::npos) {
        std::cerr << "  предел 0: запросов " << fx.host.calls()
                  << ", ответ: " << text::utf8_prefix(out.output, 200) << std::endl;
    }
    ASSERT_EQ(fx.host.calls(), (size_t)0);
    ASSERT_TRUE(out.output.find("Subagent depth limit reached (0).") !=
                std::string::npos);
}


/* ======================================================================
 * 12. И8.9: дочерняя сессия
 *
 * Проверяется не «файл записался», а три вещи, которые ломаются тихо:
 *   - дочерняя сессия НЕ попадает в правило resume (иначе следующий
 *     запуск открыл бы диалог субагента вместо диалога человека);
 *   - при продолжении история ПРЕДЫДУЩЕГО хода видна модели в ТРАНСКРИПТЕ
 *     её нового запроса (иначе `task_id` работал бы, ничего не добавляя);
 *   - `task_id` извне не превращается в путь к файлу: идентификатор
 *     приходит от модели.
 * ====================================================================== */

/* Файлы задач субагента, лежащие на диске. */
std::vector<std::string> child_session_files(const fs::path& project) {
    std::vector<std::string> out;
    const fs::path dir = project / "wp_coder" / "sessions" / "sub";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/* Прочитать файл сессии. Пустая строка — прочитать не удалось. */
SessionFile read_session_file(const std::string& path, std::string* error) {
    SessionFile out;
    std::vector<std::string> warnings;
    SessionArchive::load(path, out, error, &warnings);
    return out;
}

/* Вызов `task`: имя агента задаётся явно.
 *
 * Имя параметром, а не «всегда wp_general»: половина проверок И8.10 —
 * про то, что агент БЕЗ своих правил и агент С ЗАПРЕТОМ ведут себя иначе,
 * и вызов с зашитым именем проверял бы встроенного агента вместо
 * нужного — и проходил бы. */
ToolOutput run_task_agent(const std::string& agent,
                          const std::string& description,
                          const std::string& prompt,
                          const std::string& task_id = "") {
    json::JsonValue call = json::JsonValue::object();
    call.set("description", description);
    call.set("prompt", prompt);
    call.set("subagent_type", agent);
    if (!task_id.empty()) call.set("task_id", task_id);
    return ToolsRegistry::instance().run_output("task", call);
}

/* Вызов `task` с необязательным task_id (встроенный субагент). */
ToolOutput run_task_with_id(const std::string& description,
                            const std::string& prompt,
                            const std::string& task_id) {
    return run_task_agent("wp_general", description, prompt, task_id);
}

TEST(a_subagent_task_is_saved_as_a_child_session_of_this_dialog) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    /* Идентификатор родителя выдаётся доходом сессии; он же — parent_id
     * дочерней. */
    const std::string parent_id = engine().ensure_session_id();

    reset_host(fx.host, {"Субагент закончил: всё разобрано."});
    const ToolOutput out = run_task_with_id("Обзор проекта",
                                            "Посмотри проект и скажи главное.",
                                            "");

    const std::string session_id = out.metadata.get_string("session_id");
    ASSERT_FALSE(session_id.empty());
    ASSERT_TRUE(SessionArchive::is_session_id(session_id));
    ASSERT_TRUE(out.metadata.get_bool("session_saved", false));
    /* Файл лежит ТАМ, где resume его не видит: иначе следующий запуск
     * открыл бы диалог субагента (И8.9, current_file нерекурсивен). */
    const std::vector<std::string> files = child_session_files(fx.project);
    if (files.size() != 1) {
        std::cerr << "  файлов задач субагента: " << files.size() << std::endl;
    }
    ASSERT_EQ(files.size(), (size_t)1);

    std::string error;
    const SessionFile child = read_session_file(files[0], &error);
    if (!error.empty()) std::cerr << "  файл задачи: " << error << std::endl;
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(child.session_id, session_id);
    ASSERT_EQ(child.parent_id, parent_id);
    /* Название — по плану: описание и агент в скобках. */
    ASSERT_EQ(child.title, std::string("Обзор проекта (@wp_general subagent)"));
    /* История ребёнка — его собственная: задание и ответ, без реплик
     * родителя. Первое сообщение — задание субагента. */
    ASSERT_EQ(child.messages.size(), (size_t)2);
    if (!child.messages.empty()) {
        ASSERT_TRUE(child.messages.front().text().find(
                        "Посмотри проект") != std::string::npos);
    }
    /* Корень цепочки один: оба хода висят на первом сообщении, иначе в
     * файле был бы «разговор, у которого нет начала». */
    if (child.messages.size() == 2) {
        ASSERT_EQ(child.messages[1].parent_id, child.messages[0].id);
    }

    /* Правило resume не выбрало задачу субагента. Пишем сессию
     * пользователя и сравниваем: без этого сравнения проверка была бы
     * верна и при «resume открывает ребёнка», пока файла родителя нет. */
    engine().save_session();
    const std::string current = SessionArchive::current_file(fx.project.string());
    if (current.find("/sub/") != std::string::npos) {
        std::cerr << "  resume выбрал задачу субагента: " << current
                  << std::endl;
    }
    ASSERT_TRUE(current.find("/sub/") == std::string::npos);
    ASSERT_FALSE(current.empty());
}

TEST(task_id_resumes_the_child_session_instead_of_starting_a_new_one) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    reset_host(fx.host, {"Первый ответ: найдено три места."});
    const ToolOutput first =
        run_task_with_id("Обзор проекта", "Найди, где читается конфиг.", "");
    const std::string session_id = first.metadata.get_string("session_id");
    ASSERT_FALSE(session_id.empty());

    /* Продолжение: тот же идентификатор, новое задание. Ответ первого
     * хода обязан быть В ТРАНСКРИПТЕ нового запроса — это и есть
     * продолжение; проверка «файл перезаписан» доказала бы только, что
     * файл существует. */
    reset_host(fx.host, {"Второй ответ: вот точные места."});
    const ToolOutput second =
        run_task_with_id("Уточнение", "Теперь покажи вызовы.", session_id);

    ASSERT_EQ(second.metadata.get_string("session_id"), session_id);
    ASSERT_TRUE(second.metadata.get_bool("session_saved", false));
    const std::string seen = fx.host.transcript(0);
    if (seen.find("Первый ответ") == std::string::npos) {
        std::cerr << "  субагент не увидел прошлый ход при продолжении: "
                  << text::utf8_prefix(seen, 300) << std::endl;
    }
    ASSERT_TRUE(seen.find("Первый ответ") != std::string::npos);
    ASSERT_TRUE(seen.find("Найди, где читается конфиг") != std::string::npos);
    ASSERT_TRUE(second.output.find("вот точные места") != std::string::npos);

    /* Одна задача, один файл: продолжение не завело второго ребёнка. */
    const std::vector<std::string> files = child_session_files(fx.project);
    if (files.size() != 1) {
        std::cerr << "  после продолжения файлов задач: " << files.size()
                  << std::endl;
    }
    ASSERT_EQ(files.size(), (size_t)1);
    /* В файле оба хода и оба задания, а название осталось прежним:
     * описание продолжения — подпись этого вызова, а не имя задачи. */
    std::string error;
    const SessionFile child = read_session_file(files[0], &error);
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(child.title, std::string("Обзор проекта (@wp_general subagent)"));
    ASSERT_EQ(child.messages.size(), (size_t)4);
    bool has_old = false;
    bool has_new = false;
    for (const Message& m : child.messages) {
        if (m.text().find("Найди, где читается") != std::string::npos) has_old = true;
        if (m.text().find("Теперь покажи вызовы") != std::string::npos) has_new = true;
    }
    ASSERT_TRUE(has_old);
    ASSERT_TRUE(has_new);
    /* Корень цепочки прежний: продолжение не начинало новый разговор. */
    ASSERT_EQ(child.messages[1].parent_id, child.messages[0].id);
    ASSERT_EQ(child.messages[3].parent_id, child.messages[0].id);
}

TEST(task_id_from_another_dialog_or_a_foreign_string_is_refused) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    const std::string parent_id = engine().ensure_session_id();

    /* Сессия пользователя: её идентификатор — не задача субагента. */
    engine().save_session();

    /* Задача ЧУЖОГО диалога: кладём файл руками, с другим родителем. */
    SessionFile alien;
    alien.session_id = "ses_000000007777";
    alien.parent_id = "ses_000000099999";
    alien.title = "Чужая задача";
    alien.messages.push_back(Message::user("секретный разговор"));
    std::string error;
    const std::string alien_path =
        SessionArchive::subagent_file_path(fx.project.string(), alien.session_id);
    ASSERT_TRUE(SessionArchive::save(alien_path, alien, &error));

    /* ПОДДЕЛКА ВНЕ КАТАЛОГА СЕССИЙ.
     *
     * Путь к файлу склеивается из `task_id`, поэтому без проверки
     * «идентификатор ли это» строка `../../подделка` уводит поиск из
     * `sessions/sub/` в `wp_coder/`. Подделка кладётся ТАМ и делается
     * такой, чтобы её приняли: `parent_id` — текущий диалог, разбор
     * проходит. Тогда код без проверки не просто «не заметит» отказа, а
     * ПРОЧИТАЕТ файл и подставит его содержимое в контекст субагента —
     * то есть модель получит то, чего никто не собирался ей показывать.
     * Проверка на отказ сама по себе этого не ловила: «не найдена» и
     * «это не идентификатор» — оба отказа, и проверка принимала первый
     * (её нашёл прогон мутаций). */
    {
        std::error_code ec;
        fs::create_directories(fx.project / "wp_coder", ec);
        std::ofstream decoy(fx.project / "wp_coder" / "подделка.json");
        decoy << "{\n"
              << "  \"version\": 1,\n"
              << "  \"session\": \"ses_000000000001\",\n"
              << "  \"parent_id\": \"" << parent_id << "\",\n"
              << "  \"title\": \"Подделка\",\n"
              << "  \"messages\": [{\"id\": \"msg_000000000001\","
              << " \"role\": \"user\","
              << " \"parts\": [{\"kind\": \"text\","
              << " \"text\": \"СЕКРЕТ_ИЗ_ЧУЖОГО_ФАЙЛА\"}]}]\n"
              << "}\n";
        decoy.close();
        ASSERT_TRUE(fs::exists(fx.project / "wp_coder" / "подделка.json"));
    }

    struct Case {
        const char* id;
        const char* why;
        /* Что обязан сказать отказ. Для не-идентификатора это важно
         * отдельно: отказ «не найдена» означал бы, что до диска дошли,
         * то есть проверки формата не было. */
        const char* must_say;
    };
    const Case cases[] = {
        {"abc-123", "не идентификатор", "не идентификатор"},
        {"../../подделка", "не идентификатор: путь", "не идентификатор"},
        {"ses_000000007777", "чужой диалог", "другому диалогу"},
        {parent_id.c_str(), "не задача субагента", "не найдена"},
        {"ses_000000008888", "не найдена", "не найдена"},
    };
    for (const Case& c : cases) {
        reset_host(fx.host, {"Не понадобится."});
        const ToolOutput out =
            run_task_with_id("Продолжение", "Продолжи.", c.id);
        if (out.output.find("[ошибка] task") == std::string::npos ||
            out.output.find(c.must_say) == std::string::npos) {
            std::cerr << "  task_id «" << c.id << "» (" << c.why
                      << ") — отказ не тот: "
                      << text::utf8_prefix(out.output, 200) << std::endl;
        }
        ASSERT_TRUE(out.output.find("[ошибка] task") != std::string::npos);
        ASSERT_TRUE(out.output.find(c.must_say) != std::string::npos);
        /* Отказ ДО запроса к модели: продолжения, которого не будет, не
         * должно стоить денег, и чужой файл не должен попасть в контекст
         * субагента. */
        ASSERT_EQ(fx.host.calls(), (size_t)0);
        for (size_t i = 0; i < fx.host.requests.size(); ++i) {
            if (fx.host.transcript(i).find("СЕКРЕТ_ИЗ_ЧУЖОГО_ФАЙЛА") !=
                std::string::npos) {
                std::cerr << "  содержимое чужого файла дошло до модели"
                          << std::endl;
            }
        }
        /* И никакой новой задачи: отказ не должен создавать сессию. */
        ASSERT_EQ(child_session_files(fx.project).size(), (size_t)1);
    }

    /* Ни один отказ не тронул файл чужой задачи. */
    std::string alien_error;
    const SessionFile still_alien =
        read_session_file(alien_path, &alien_error);
    ASSERT_TRUE(alien_error.empty());
    ASSERT_EQ(still_alien.title, std::string("Чужая задача"));
    ASSERT_EQ(still_alien.messages.size(), (size_t)1);
}

TEST(resumed_task_ids_do_not_clash_with_the_ids_already_in_the_file) {
    /* Файл задачи мог быть записан ПРОШЛЫМ запуском плагина: его
     * идентификаторы тогда были крупнее, чем нынешний счётчик процесса.
     * SessionArchive::load поднимает счётчик по прочитанному — иначе
     * первый же новый ход получил бы номер, который в истории уже занят,
     * и два сообщения слиплись бы в одно молча (И5.6). */
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    SessionFile seeded;
    seeded.session_id = "ses_000000090000";
    seeded.parent_id = engine().ensure_session_id();
    seeded.title = "Задача из прошлого запуска";
    Message first = Message::user("старый ход");
    first.id = "msg_000000090001";
    Message answer = Message::assistant(first.id);
    answer.id = "msg_000000090002";
    answer.parts.push_back(MessagePart::text("старый ответ"));
    seeded.messages.push_back(first);
    seeded.messages.push_back(answer);
    std::string error;
    ASSERT_TRUE(SessionArchive::save(
        SessionArchive::subagent_file_path(fx.project.string(),
                                           seeded.session_id),
        seeded, &error));

    reset_host(fx.host, {"Продолжение после перезапуска."});
    const ToolOutput out =
        run_task_with_id("Продолжение", "Ещё немного.", seeded.session_id);
    ASSERT_TRUE(out.output.find("[ошибка] task") == std::string::npos);
    ASSERT_TRUE(out.output.find("Продолжение после перезапуска") !=
                std::string::npos);

    /* Ни одно новое сообщение не получило номер из старой истории. */
    std::string load_error;
    const SessionFile child = read_session_file(
        SessionArchive::subagent_file_path(fx.project.string(),
                                           seeded.session_id),
        &load_error);
    ASSERT_TRUE(load_error.empty());
    ASSERT_EQ(child.messages.size(), (size_t)4);
    for (size_t i = 2; i < child.messages.size(); ++i) {
        if (id_number(child.messages[i].id) <= 90002) {
            std::cerr << "  новое сообщение получило занятый номер: "
                      << child.messages[i].id << std::endl;
        }
        ASSERT_TRUE(id_number(child.messages[i].id) > 90002);
    }
}

/* ======================================================================
 * 13. И8.10: наследование разрешений — на живом движке
 *
 * Проверки 12-го раздела живут в test_agent_config.cpp и смотрят на
 * СБОРКУ правил. Здесь — то, что важнее: куда доходит отказ и что видит
 * модель. Три утверждения, и каждое было бы «вроде obvious»:
 *   - разрешение сессии ребёнку НЕ достаётся, и ребёнок об этом
 *     спрашивает пользователя сам (а не молча делает по разрешению сессии);
 *   - запрет сессии доходит, и инструмент у ребёнка ещё и скрыт из
 *     каталога;
 *   - про `external_directory` решают правила РЕБЁНКА: иначе наследование
 *     было бы мёртвым, а запрет агента не действовал бы.
 * ====================================================================== */

/* Агент без своих правил: всё, что не запрещено сессией, спрашивается. */
bool add_probe_agent() {
    return add_bare_agent("wp_probe", "Агент без правил, всё спрашивает.");
}

TEST(a_subagent_does_not_inherit_allows_and_asks_the_user_instead) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    ASSERT_TRUE(add_probe_agent());

    /* Сессия разрешает ВСЁ (approve_all_permissions) и вдобавок запрещает
     * запись — запрет перейти должен, разрешение — нет. */
    engine().permissions().add_rule(
        Rule{"write", "*", PermissionAction::Deny, "тест: запись запрещена"});

    {
        std::ofstream f(fx.project / "odin.txt");
        f << "содержимое\n";
    }
    /* Ребёнок читает (спрашивает) и пишет (отказ по правилу). */
    reset_host(fx.host, {
        call_block("read_file", ",\n \"path\": \"odin.txt\""),
        call_block("write_file", ",\n \"path\": \"zapis.txt\","
                                "\n \"content\": \"привет\""),
        "Прочитал, записать не смог."});

    /* Пользователь отвечает «всегда» на вопросы ребёнка. */
    std::atomic<int> asked{0};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        for (int i = 0; i < 300; ++i) {
            const std::vector<PermissionRequest> p =
                engine().permissions().pending();
            if (!p.empty() &&
                engine().permissions().reply(p.front().id,
                                             PermissionReply::Always)) {
                ++asked;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    Joiner joiner(threads);

    const ToolOutput out = run_task_agent("wp_probe", "Чтение и запись",
                                         "Прочитай и запиши.");
    joiner.join();

    /* Вопрос был: разрешение сессии ребёнку не досталось. Без этого
     * утверждения тест прошёл бы и при наследовании — и проверил бы
     * тогда не то. */
    if (asked.load() != 1) {
        std::cerr << "  вопросов ребёнка: " << asked.load()
                  << " (ожидался один: разрешение сессии не наследуется)"
                  << std::endl;
    }
    ASSERT_EQ(asked.load(), 1);
    /* Чтение состоялось: ответ «всегда» дошёл до правил ребёнка (И8.7). */
    ASSERT_TRUE(fx.host.transcript(2).find("содержимое") != std::string::npos);
    /* Запись запрещена правилом, которое ребёнок УНАСЛЕДОВАЛ, и файла нет. */
    const std::string seen = fx.host.transcript(2);
    if (seen.find("запрещён правилом") == std::string::npos) {
        std::cerr << "  ребёнок не увидел запрет на запись: "
                  << text::utf8_prefix(seen, 300) << std::endl;
    }
    ASSERT_TRUE(seen.find("запрещён правилом") != std::string::npos);
    ASSERT_FALSE(fs::exists(fx.project / "zapis.txt"));
    ASSERT_TRUE(out.output.find("записать не смог") != std::string::npos);

    /* Запрет виден РАНЬШЕ отказа: инструмент убран из каталога ребёнка
     * (denies_whole_key), то есть модель о нём даже не знает. У сессии он
     * тоже скрыт — запрет-то сессионный, и проверять тут нечего: разницу
     * «своё правило агента против унаследованного» показывает
     * the_child_rules_decide_about_paths_outside_the_project. */
    ASSERT_FALSE(catalogue_mentions(fx.host.sys(0), "write_file"));
}

TEST(a_subagent_cannot_delegate_or_rewrite_the_session_plan_by_default) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    reset_host(fx.host, {
        call_block("task", task_args("Вложенная", "Ещё глубже.", "wp_general")),
        call_block("todowrite", ",\n \"todos\": [{\"id\": 1,"
                                "\n \"content\": \"мой план\","
                                "\n \"status\": \"pending\","
                                "\n \"priority\": \"high\"}]"),
        "Ни делегировать, ни план вести не смог."});
    const ToolOutput out = run_task_with_id("Попытка", "Попробуй.", "");
    ASSERT_TRUE(out.output.find("[ошибка] task") == std::string::npos);

    /* Оба отказа — ПО ПРАВИЛУ, а не лимитом глубины: субагент не умеет
     * делегировать по умолчанию (И8.10), и лимит глубины тут ни при чём
     * (его проверяет агент с правом `task`, см. раздел 6). */
    const std::string seen = fx.host.transcript(2);
    if (seen.find("Инструмент task запрещён правилом") == std::string::npos ||
        seen.find("todowrite запрещён правилом") == std::string::npos) {
        std::cerr << "  отказы по правилу не видны ребёнку: "
                  << text::utf8_prefix(seen, 400) << std::endl;
    }
    ASSERT_TRUE(seen.find("Инструмент task запрещён правилом") !=
                std::string::npos);
    ASSERT_TRUE(seen.find("todowrite запрещён правилом") != std::string::npos);
    ASSERT_TRUE(seen.find("depth limit") == std::string::npos);
    /* Три запроса: два отказа пришли без обращения к модели, а третий
     * ход — это реакция ребёнка на них. */
    ASSERT_EQ(fx.host.calls(), (size_t)3);

    /* План СЕССИИ не тронут: план — то, что человек видит и в чём
     * участвует, и переписанный ребёнком план выглядел бы как решение
     * пользователя. */
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    ASSERT_TRUE(engine_state().todos.empty());
}

TEST(the_child_rules_decide_about_paths_outside_the_project) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* Сессия разрешает всё, включая выход за пределы проекта. */
    engine().permissions().add_rule(
        Rule{"external_directory", "*", PermissionAction::Allow,
             "тест: сессия пускает куда угодно"});

    /* Агент, которому выход запрещён: его правило идёт после
     * унаследованного, то есть перекрывает его. */
    AgentDef caged;
    caged.name = "wp_caged";
    caged.description = "За пределы проекта не ходит";
    caged.mode = AgentMode::Subagent;
    caged.prompt = "Работаю внутри проекта.";
    /* Читать ему разрешено, иначе проверка остановилась бы на вопросе о
     * самом чтении и до гейта не дошла: гейт (external_directory) —
     * вот что здесь проверяется. */
    PermissionEntry read;
    read.key = "read";
    read.action = PermissionAction::Allow;
    caged.permission.push_back(read);
    PermissionEntry no_outside;
    no_outside.key = "external_directory";
    no_outside.action = PermissionAction::Deny;
    caged.permission.push_back(no_outside);
    std::string err;
    ASSERT_TRUE(AgentRegistry::instance().add(caged, &err));

    /* Файл ВНЕ проекта, в доверенном каталоге /tmp. */
    const fs::path outside = fs::path("/tmp") /
                             ("wp_coder_outside_" + std::to_string(::getpid()) +
                              ".txt");
    {
        std::ofstream f(outside);
        f << "снаружи\n";
    }

    reset_host(fx.host, {
        call_block("read_file", ",\n \"path\": \"" + outside.string() + "\""),
        "За пределы проекта не хожу."});
    const ToolOutput out = run_task_agent("wp_caged", "Выход наружу",
                                         "Прочитай файл снаружи.");
    const std::string seen = fx.host.transcript(1);
    if (seen.find("запрещён правилом") == std::string::npos) {
        std::cerr << "  ребёнок прошёл за пределы проекта: "
                  << text::utf8_prefix(seen, 300) << std::endl;
    }
    ASSERT_TRUE(seen.find("запрещён правилом") != std::string::npos);
    /* Отказ гейта, а не вопрос без ответа: «спросить и не дождаться» и
     * «запрещено правилом» выглядели бы для модели одинаково. */
    ASSERT_TRUE(seen.find("Пользователь не разрешил") == std::string::npos);
    ASSERT_TRUE(out.output.find("не хожу") != std::string::npos);

    /* Сессия при этом может: запрет ребёнка не стал запретом диалога.
     * Без этой половины проверка прошла бы и при сужении правил СЕССИИ,
     * то есть проверяла бы не то место. */
    json::JsonValue parent_call = json::JsonValue::object();
    parent_call.set("path", outside.string());
    const ToolOutput parent_out =
        ToolsRegistry::instance().run_output("read_file", parent_call);
    if (parent_out.output.find("снаружи") == std::string::npos) {
        std::cerr << "  сессия не прочитала файл снаружи: "
                  << text::utf8_prefix(parent_out.output, 200) << std::endl;
    }
    ASSERT_TRUE(parent_out.output.find("снаружи") != std::string::npos);

    std::error_code ec;
    fs::remove(outside, ec);
}

/* ======================================================================
 * 14. И8.11: результат в обёртке — и идентификатор в ней же
 *
 * Проверка двойная по одной причине: обёртка без идентификатора была бы
 * красивой формой, а идентификатор без обёртки — вещью, о которой
 * модель не знает. Смысл 8.11 в том, что одно доезжает в другом.
 * ====================================================================== */

TEST(the_task_result_carries_the_task_id_the_model_can_continue_by) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    reset_host(fx.host, {"Итог: нашёл три места."});
    const ToolOutput out = run_task_agent("wp_general", "Обзор",
                                         "Посмотри и скажи.");

    const std::string session_id = out.metadata.get_string("session_id");
    ASSERT_FALSE(session_id.empty());
    /* Обёртка порта: идентификатор, состояние, тело результата. Именно
     * ИДЕНТИФИКАТОР, а не описание задачи — по нему модель продолжит. */
    const std::string opening = "<task id=\"" + session_id +
                                "\" state=\"completed\">";
    if (out.output.find(opening) == std::string::npos) {
        std::cerr << "  обёртка не названа верно: "
                  << text::utf8_prefix(out.output, 200) << std::endl;
    }
    ASSERT_TRUE(out.output.find(opening) != std::string::npos);
    ASSERT_TRUE(out.output.find("<task_result>") != std::string::npos);
    ASSERT_TRUE(out.output.find("Итог: нашёл три места.") !=
                std::string::npos);
    ASSERT_TRUE(out.output.find("</task_result>") != std::string::npos);
    ASSERT_TRUE(out.output.back() == '>');

    /* Формат ОБЪЯВЛЕН в системном промпте (общее правило D2): строка
     * формата, о которой модель не знает, ею и не пользуется. */
    ASSERT_TRUE(engine().build_system_prompt().find("<task_result>") !=
                std::string::npos);
    ASSERT_TRUE(engine().build_system_prompt().find("task_id") !=
                std::string::npos);

    /* И это не бумажный идентификатор: продолжение с ним работает. */
    reset_host(fx.host, {"Продолжил: вот точные места."});
    const ToolOutput second = run_task_agent("wp_general", "Уточнение",
                                             "Теперь покажи вызовы.",
                                             session_id);
    ASSERT_EQ(second.metadata.get_string("session_id"), session_id);
    ASSERT_TRUE(second.output.find("Продолжил") != std::string::npos);
    /* Прерванная линия, которой не было в 8.9: отказ по лимиту приходит
     * обёрткой и называет настроенный предел. */
    reset_host(fx.host, {"Ещё."});
    const ToolOutput third = run_task_agent("wp_general", "Ещё раз",
                                            "И ещё немного.", session_id);
    ASSERT_TRUE(third.output.find("<task id=\"" + session_id) !=
                std::string::npos);
}

TEST(a_failed_task_answers_with_the_same_wrapper_and_says_why) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* Провайдер молчит: ход не состоялся. */
    fx.host.fail = true;
    const ToolOutput out = run_task_agent("wp_general", "Провайдер",
                                         "Ничего не делать.");
    const std::string session_id = out.metadata.get_string("session_id");

    const std::string opening = "<task id=\"" + session_id +
                                "\" state=\"error\">";
    if (out.output.find(opening) == std::string::npos) {
        std::cerr << "  отказ без обёртки или с чужим состоянием: "
                  << text::utf8_prefix(out.output, 200) << std::endl;
    }
    ASSERT_TRUE(out.output.find(opening) != std::string::npos);
    ASSERT_TRUE(out.output.find("state=\"completed\"") == std::string::npos);
    /* Причина внутри, а не вместо: вызывающий должен знать и что делать
     * дальше, и что именно сломалось. */
    ASSERT_TRUE(out.output.find("сеть недоступна") != std::string::npos);
    ASSERT_TRUE(out.output.find("не выполнил задачу") != std::string::npos);

    /* ОТКАЗЫ до хода обёрткой НЕ идут: задачи не было. Иначе появление
     * идентификатора в отказе читалось бы как «работа началась». Проверяются
     * ДВА отказа и не один: отказ по имени агента и отказ по полю — они
     * пишутся разными строками, и проверка одного из них ничего не сказала
     * бы о втором. */
    /* Сколько задач было до отказов: их создавать нельзя. Первая часть
     * проверки задачу создала, поэтому сравниваем снимок, а не ноль. */
    const size_t tasks_before = child_session_files(fx.project).size();

    json::JsonValue bad = json::JsonValue::object();
    bad.set("description", "Несуществующий");
    bad.set("prompt", "Ничего.");
    bad.set("subagent_type", "wp_нет_такого");
    const ToolOutput refused = ToolsRegistry::instance().run_output("task", bad);
    ASSERT_TRUE(refused.output.find("<task ") == std::string::npos);
    ASSERT_TRUE(refused.output.find("[ошибка] task") != std::string::npos);

    /* Второй отказ — пустое задание при background: true. Раньше здесь стоял
     * отказ по самому полю `background` (работа за ним — И8.14); теперь
     * отказов до хода два, и оба обязаны быть ДО постановки в фон, иначе
     * пустая задача уехала бы в очередь. */
    json::JsonValue bg = json::JsonValue::object();
    bg.set("description", "В фон");
    bg.set("prompt", "");
    bg.set("subagent_type", "wp_general");
    bg.set("background", true);
    const ToolOutput no_bg = ToolsRegistry::instance().run_output("task", bg);
    ASSERT_TRUE(no_bg.output.find("[ошибка] task") != std::string::npos);
    if (no_bg.output.find("<task ") != std::string::npos) {
        std::cerr << "  отказ по полю обёрнут задачей: "
                  << text::utf8_prefix(no_bg.output, 200) << std::endl;
    }
    ASSERT_TRUE(no_bg.output.find("<task ") == std::string::npos);
    /* И задачи не создано: отказ до хода не оставляет файла. */
    ASSERT_EQ(child_session_files(fx.project).size(), tasks_before);
    /* …и ничего не поставлено в фон (очередь чиста — см. фикстуру). */
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        ASSERT_TRUE(engine_state().background_tasks.empty());
    }
}


/* ======================================================================
 * 15. И8.12: каскад отмены
 *
 * Отмена родителя должна гасить ребёнка В ЖИВОМ запросе, а не между его
 * шагами: иначе «стоп» ждёт, пока провайдер договорит, а это ровно тот
 * случай, ради которого в И6.6 переписали блокирующий вызов. Проверка
 * идёт на живой запрос (имитатор ждёт отмены), потому что на мгновенном
 * ответе «отмена ничего не ждёт» прошла бы при любом коде.
 * ====================================================================== */

/* Отменить родителя, как только ребёнок ушёл в запрос. */
void abort_when_the_subagent_is_in_flight(FakeHost& host) {
    std::thread aborter([&host] {
        for (int i = 0; i < 400 && !host.entered.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        engine().request_abort();
    });
    aborter.detach();
}

TEST(aborting_the_parent_stops_the_subagent_in_its_live_request) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    fx.host.wait_for_abort = true;
    /* Поток отмены: без него проверка ждала бы истёкшего ожидания хоста и
     * прошла бы с «провайдер не ответил» при работающем коде. */
    abort_when_the_subagent_is_in_flight(fx.host);

    const ToolOutput out = run_task_agent("wp_general", "Долгая задача",
                                         "Смотри проект.");
    /* Живой запрос ребёнка прервался по флагу отмены — тот же предикат,
     * что у родителя. */
    if (!fx.host.saw_abort.load()) {
        std::cerr << "  ожидание запроса ребёнка не прервалось: "
                  << text::utf8_prefix(out.output, 200) << std::endl;
    }
    ASSERT_TRUE(fx.host.saw_abort.load());

    /* Отмена названа ОТМЕНОЙ, а не сбоем провайдера: человек нажал «стоп»,
     * и в ленте событий и в ответе модели должно быть написано то же самое.
     * Раньше здесь был текст «провайдер не ответил» — он и появлялся бы. */
    if (out.output.find("[прервано пользователем]") == std::string::npos) {
        std::cerr << "  отмена не названа отменой: "
                  << text::utf8_prefix(out.output, 200) << std::endl;
    }
    ASSERT_TRUE(out.output.find("[прервано пользователем]") !=
                std::string::npos);
    ASSERT_TRUE(out.output.find("state=\"error\"") != std::string::npos);
    ASSERT_TRUE(out.output.find("Провайдер не ответил") == std::string::npos);
    /* Следующего шага ребёнка не было: один запрос, один отказ. */
    ASSERT_EQ(fx.host.calls(), (size_t)1);

    /* Превосходная задача не потеряна: сессия ребёнка записана даже при
     * отмене (И8.9), и её можно продолжить. */
    const std::vector<std::string> files = child_session_files(fx.project);
    ASSERT_EQ(files.size(), (size_t)1);
    std::string error;
    const SessionFile child = read_session_file(files[0], &error);
    ASSERT_TRUE(error.empty());
    ASSERT_FALSE(child.session_id.empty());
}

TEST(aborting_the_parent_releases_the_subagent_waiting_for_permission) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    /* Ребёнок без правил спрашивает разрешение на чтение — и ждёт. */
    ASSERT_TRUE(add_bare_agent("wp_asker", "Субагент, у которого спрашивают."));
    {
        std::ofstream f(fx.project / "odin.txt");
        f << "содержимое\n";
    }
    reset_host(fx.host, {
        call_block("read_file", ",\n \"path\": \"odin.txt\""),
        "Файл прочитан."});

    /* Отмена приходит, пока ребёнок висит на вопросе пользователю: без
     * неё «стоп» не вытащил бы ребёнка из ожидания, и родитель продолжил бы
     * работу вместо того, чтобы остановиться. */
    std::atomic<bool> saw_question{false};
    /* Отсчёт от появления вопроса: до него ждать нечего, и «долго» значило
     * бы только то, что вопрос долго не появлялся. */
    std::atomic<long long> asked_at_ms{0};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        for (int i = 0; i < 400; ++i) {
            if (!engine().permissions().pending().empty()) {
                saw_question.store(true);
                asked_at_ms.store(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now()
                            .time_since_epoch()).count());
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        engine().request_abort();
    });
    Joiner joiner(threads);

    const ToolOutput out = run_task_agent("wp_asker", "Вопрос",
                                         "Прочитай файл.");
    joiner.join();

    ASSERT_TRUE(out.output.find("[прервано пользователем]") !=
                std::string::npos);
    ASSERT_TRUE(out.output.find("state=\"error\"") != std::string::npos);
    /* Вопрос ДЕЙСТВИТЕЛЬНО был: иначе проверка прошла бы потому, что читать
     * разрешили без спроса, и каскад отмены в неё не попал бы вовсе. */
    if (!saw_question.load()) {
        std::cerr << "  вопроса о разрешении не было — отменять было нечего"
                  << std::endl;
    }
    ASSERT_TRUE(saw_question.load());
    /* Вопрос снят, а не висит: иначе UI остался бы с открытым вопросом
     * после остановки. */
    ASSERT_TRUE(engine().permissions().pending().empty());
    /* И снят ОТМЕНОЙ, а не истечением ожидания: страховочный таймаут в
     * тестах — 2 с, и если бы сработал он, «стоп» в приложении ждал бы
     * столько же на каждом вопросе. Порог 1.5 с с запасом на медленную
     * машину; при правильном коде освобождение мгновенное. */
    const long long waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() -
        asked_at_ms.load();
    if (waited > 1500) {
        std::cerr << "  освобождение вопроса заняло " << waited
                  << " мс — значит, сработал таймаут ожидания, а не отмена"
                  << std::endl;
    }
    ASSERT_TRUE(waited < 1500);
    /* И область вернулась: остановка ребёнка не должна оставить его промпт
     * следующему ходу родителя. */
    const RunScope after = engine().scope_snapshot();
    ASSERT_EQ(after.agent, std::string(""));
    ASSERT_TRUE(after.info == nullptr);
}

TEST(aborting_the_parent_kills_the_command_the_subagent_is_running) {
    /* Самый глубокий случай каскада: отмена приходит, когда ребёнок внутри
     * ИНСТРУМЕНТА. Здесь работает не флаг между шагами, а токен отмены
     * хода (core/abort.h): `bash` убивает ГРУППУ процессов (И6.7), и без
     * отмены токена «стоп» ждал бы конца команды — то есть до получаса
     * `sleep 30`.
     *
     * Проверка идёт на времени намеренно: «ответ пришёл» и «ответ пришёл
     * быстро» — разные вещи, и второе здесь и есть предмет. */
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* Параметр `cli`, а не `command`: у bash схема требует cli, и вызов с
     * «command» отклонялся проверкой аргументов — команда не запускалась
     * вовсе, а проверка проходила, ничего не делая. Нашлось это тем, что
     * мутация «токен не прерывается» выжила. */
    ASSERT_TRUE(add_command_agent());
    /* Команда взята из allowlist политики (И3) и она ДЛИННАЯ: `sleep` в
     * allowlist нет, и вызов отклонялся политикой мгновенно — команда не
     * запускалась, а проверка проходила, ничего не делая. Нашлось это тем,
     * что мутация «токен не прерывается» выжила. */
    reset_host(fx.host, {
        call_block("bash", ",\n \"cli\": \"ping -c 25 127.0.0.1\""),
        "Команда не нужна."});

    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        engine().request_abort();
    });
    Joiner joiner(threads);
    const ToolOutput out = run_task_agent("wp_commander", "Долгая команда",
                                         "Запусти долгую команду.");
    joiner.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    if (out.output.find("[прервано пользователем]") == std::string::npos) {
        std::cerr << "  отмена не остановила команду ребёнка ("
                  << elapsed << " мс): "
                  << text::utf8_prefix(out.output, 200) << std::endl;
    }
    ASSERT_TRUE(out.output.find("[прервано пользователем]") !=
                std::string::npos);
    /* Команда была убита, а не доработана: 30 с ожидания при сорока
     * миллисекундах до отмены. Порог с запасом (5 с), чтобы прогон не
     * «мигал» на загруженной машине. */
    if (elapsed > 5000) {
        std::cerr << "  команда ребёнка не убита: " << elapsed << " мс"
                  << std::endl;
    }
    ASSERT_TRUE(elapsed < 5000);
    /* Следующего шага не было: оборванный ход не продолжается. */
    ASSERT_EQ(fx.host.calls(), (size_t)1);
    /* И задача сохранена: прерванную работу можно продолжить (И8.9). */
    ASSERT_EQ(child_session_files(fx.project).size(), (size_t)1);
}

/* ======================================================================
 * 16. И8.13: описание `task` собирается под вызывающего
 *
 * Список субагентов попадает в КАЖДЫЙ запрос, поэтому проверяется не
 * функция сборки, а то, что реально уехало в модель: блок описания в
 * системном промпте. Отдельно — что список отфильтрован по правилам
 * ВЫЗЫВАЮЩЕГО, а не по наличию агента: «агент есть» и «его можно
 * позвать» — разные вещи.
 * ====================================================================== */

/* Список субагентов в тексте описания (между заголовком и концом). */
std::string subagent_list_from(const std::string& prompt) {
    const std::string head = "Доступные субагенты:\n";
    const size_t at = prompt.find(head);
    if (at == std::string::npos) return std::string();
    const size_t from = at + head.size();
    /* Конец списка — первая строка параметров описания («description — …»)
     * либо конец текста: список идёт до конца описания инструмента. */
    const size_t tail = prompt.find("\n    description", from);
    return prompt.substr(from, tail == std::string::npos
                                   ? std::string::npos
                                   : tail - from);
}

TEST(the_task_description_lists_the_available_subagents) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    const std::string prompt = engine().build_system_prompt();
    if (!catalogue_mentions(prompt, "task")) {
        std::cerr << "  в каталоге модели нет task: "
                  << text::utf8_prefix(prompt, 300) << std::endl;
    }
    ASSERT_TRUE(catalogue_mentions(prompt, "task"));

    /* Заголовок списка — в промпте, а извлекается уже сам список. */
    ASSERT_TRUE(prompt.find("Доступные субагенты") != std::string::npos);
    const std::string list = subagent_list_from(prompt);
    if (list.empty()) {
        std::cerr << "  список субагентов пуст: "
                  << text::utf8_prefix(prompt.substr(prompt.find("\n- task")),
                                       300)
                  << std::endl;
    }
    ASSERT_FALSE(list.empty());
    /* Имя плюс ЕГО ОПИСАНИЕ: модели нужно знать, чем этот агент
     * занимается, а «wp_explore» само по себе не говорит ничего. */
    ASSERT_TRUE(list.find("wp_general") != std::string::npos);
    ASSERT_TRUE(list.find("wp_explore") != std::string::npos);
    ASSERT_TRUE(list.find("Субагент-поиск") != std::string::npos);
    /* Список строкой на агента, а не одной простынёй: иначе не видно,
     * где кончилось одно описание и началось другое. */
    ASSERT_TRUE(list.find("\n- ") != std::string::npos);
}

TEST(a_subagent_the_caller_may_not_use_is_hidden_from_the_description) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* Человек запретил делегировать к одному агенту. */
    engine().permissions().add_rule(
        Rule{"task", "wp_explore", PermissionAction::Deny,
             "тест: к wp_explore не звать"});

    const std::string list = subagent_list_from(engine().build_system_prompt());
    if (list.find("wp_explore") != std::string::npos) {
        std::cerr << "  запрещённый субагент в списке: "
                  << text::utf8_prefix(list, 300) << std::endl;
    }
    ASSERT_TRUE(list.find("wp_explore") == std::string::npos);
    /* Остальные на месте: запрет одного не должен был вычеркнуть всех. */
    ASSERT_TRUE(list.find("wp_general") != std::string::npos);

    /* И до места, где зовут, доведено: вызов запрещённого агента отказ,
     * а не «агент не найден» — запрет по имени виден по ключу `task`. */
    reset_host(fx.host, {"Не понадобится."});
    json::JsonValue call = json::JsonValue::object();
    call.set("description", "Обзор");
    call.set("prompt", "Посмотри.");
    call.set("subagent_type", "wp_explore");
    /* Через ToolRunner, а не напрямую: enforcement по ключу `task` живёт
     * там, и прямой вызов реестра его не проходит — то есть проверил бы
     * не отказ, а обработчик инструмента. */
    ToolRunner runner(engine_state(), cb,
                      [](AgentEvent::Kind, const std::string&) {});
    const ToolOutcome outcome = runner.run("task", call);
    if (!outcome.ok &&
        outcome.error.find("запрещён правилом") == std::string::npos) {
        std::cerr << "  запрет по имени дал не тот отказ: "
                  << text::utf8_prefix(outcome.error, 200) << std::endl;
    }
    ASSERT_FALSE(outcome.ok);
    ASSERT_TRUE(outcome.error.find("запрещён правилом") != std::string::npos);
    /* Отказ называет ИМЯ агента, а не только ключ: иначе модель не поняла
     * бы, что именно нельзя, и позвала бы другого. */
    ASSERT_TRUE(outcome.error.find("wp_explore") != std::string::npos);
    ASSERT_EQ(fx.host.calls(), (size_t)0);
}

TEST(the_list_belongs_to_the_calling_agent_and_not_to_the_session) {
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* Ребёнок, которому делегирование разрешено, но не к агенту
     * wp_explore. Сессия при этом ничего не запрещала: если бы список
     * брался у сессии, wp_explore остался бы в списке — и модель
     * потратила бы шаг на заведомо отклонённый вызов. */
    AgentDef picky;
    picky.name = "wp_picky";
    picky.description = "Делегирует не всему подряд";
    picky.mode = AgentMode::Subagent;
    picky.prompt = "Делегирую.";
    PermissionEntry task;
    task.key = "task";
    task.action = PermissionAction::Allow;
    picky.permission.push_back(task);
    PermissionEntry no_explore;
    no_explore.key = "task";
    no_explore.pattern = "wp_explore";
    no_explore.action = PermissionAction::Deny;
    picky.permission.push_back(no_explore);
    std::string err;
    ASSERT_TRUE(AgentRegistry::instance().add(picky, &err));

    reset_host(fx.host, {"Готово."});
    run_task_agent("wp_picky", "Проверка списка", "Ничего не делать.");

    const std::string child_prompt = fx.host.sys(0);
    const std::string child_list = subagent_list_from(child_prompt);
    if (child_list.find("wp_explore") != std::string::npos ||
        child_list.find("wp_general") == std::string::npos) {
        std::cerr << "  список у ребёнка не его: "
                  << text::utf8_prefix(child_list, 300) << std::endl;
    }
    ASSERT_TRUE(child_list.find("wp_explore") == std::string::npos);
    ASSERT_TRUE(child_list.find("wp_general") != std::string::npos);
    /* А у сессии wp_explore по-прежнему доступен — правила разные. */
    ASSERT_TRUE(subagent_list_from(engine().build_system_prompt())
                    .find("wp_explore") != std::string::npos);

    /* И обратная сторона И2.8: агенту, которому делегировать запрещено
     * целиком, инструмент `task` не показывается вовсе — вместе со
     * списком. Иначе модель читала бы «доступные субагенты» под
     * инструментом, которого нельзя вызвать. */
    reset_host(fx.host, {"Готово."});
    run_task_agent("wp_general", "Проверка скрытия", "Ничего не делать.");
    ASSERT_FALSE(catalogue_mentions(fx.host.sys(0), "task"));
    ASSERT_TRUE(fx.host.sys(0).find("Доступные субагенты") ==
                std::string::npos);
}

TEST(an_empty_subagent_list_says_so_and_does_not_look_like_the_whole_list) {
    /* Список пуст, а инструмент виден: запреты заданы ПО ИМЕНАМ, поэтому
     * правила «всё под ключом» нет и инструмент не скрыт (И2.8). Молчащий
     * пустой список выглядел бы как «субагенты не загрузились» — и модель
     * либо выбрала бы имя наугад, либо решила, что делегирование сломано.
     * Сказать «доступных нет» можно только здесь: при нормальной
     * конфигурации список не пуст. */
    TaskFixture fx;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    for (const std::string& n : AgentRegistry::instance().subagent_names()) {
        engine().permissions().add_rule(
            Rule{"task", n, PermissionAction::Deny, "тест: никого не звать"});
    }
    ASSERT_FALSE(AgentRegistry::instance().subagent_names().empty());

    const std::string prompt = engine().build_system_prompt();
    /* Инструмент на месте — запреты по именам, а не catch-all. */
    ASSERT_TRUE(catalogue_mentions(prompt, "task"));
    const std::string list = subagent_list_from(prompt);
    if (list.find("нет") == std::string::npos) {
        std::cerr << "  пустой список не назван: ["
                  << text::utf8_prefix(list, 200) << "]" << std::endl;
    }
    ASSERT_TRUE(list.find("нет") != std::string::npos);
    /* И ни одного имени в списке: иначе «нет» и имя рядом читались бы как
     * «доступен вот этот». */
    for (const std::string& n : AgentRegistry::instance().subagent_names()) {
        if (list.find(n) != std::string::npos) {
            std::cerr << "  в «пустом» списке есть " << n << std::endl;
        }
        ASSERT_TRUE(list.find(n) == std::string::npos);
    }
}

/* ======================================================================
 * 17. И8.14: `background: true` — возврат сразу, результат отдельным
 *     сообщением
 *
 * Что здесь проверяется и почему именно так
 * ----------------------------------------
 * «Фон» — это доставка результата, а не поток: очередь задач разбирается
 * МЕЖДУ ходами родителя (обоснование выбора — в core/subagent.h). Поэтому
 * проверки идут по тому, что видно СНАРУЖИ:
 *   - вызов `task` вернулся, а субагент ещё не работал (запросов к модели
 *     столько же, сколько шагов родителя, и ни одного запроса ребёнка);
 *   - ребёнок отработал ПОСЛЕ того, как ход родителя закончился, и под
 *     СВОИМИ правилами (его системный промпт собран по его агенту);
 *   - его итог ПРОЧИТАЛА модель: обёртка с идентификатором и текст результата
 *     лежат в транскрипте следующего запроса к модели, а не «в состоянии
 *     сессии где-то есть».
 *
 * Считаются не числа инструментов, а свойства и запросы к модели: реестр
 * общий, и число вызовов `task` зависит от того, кто отработал раньше.
 * ====================================================================== */

/* Вызов `task` с background: true. */
ToolOutput queue_background_task(const std::string& agent,
                                 const std::string& description,
                                 const std::string& prompt) {
    json::JsonValue call = json::JsonValue::object();
    call.set("description", description);
    call.set("prompt", prompt);
    call.set("subagent_type", agent);
    call.set("background", true);
    return ToolsRegistry::instance().run_output("task", call);
}

/* Идентификатор фоновой задачи из части-вызова в истории родителя. */
std::string queued_id(const std::vector<Message>& history) {
    for (const Message& m : history) {
        if (const MessagePart* part = tool_part(m, "task")) {
            return part->output().metadata.get_string("session_id");
        }
    }
    return std::string();
}

/* Размер очереди фоновых задач (снимок под локом). */
size_t background_queue_size() {
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    return engine_state().background_tasks.size();
}

/* Сколько раз движок уже начал автоматический ход по фоновым результатам. */
int background_turns_now() {
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    return engine_state().background_turns;
}

TEST(a_background_task_returns_at_once_and_its_result_comes_in_the_next_turn) {
    TaskFixture fx;
    /* Ответы имитатора — общая очередь для родителя и ребёнка, и ПЕРВЫЙ
     * достаётся тому, кто спросил: шаг родителя, его итог, ход фонового
     * ребёнка, ход родителя, читающий результат. */
    const std::string child_answer_text =
        "Сессия создаётся в core/session_store.cpp, класс SessionArchive.";
    fx.host.replies = {
        call_block("task", task_args("Обзор модуля",
                                     "Найди, где создаётся сессия.",
                                     "wp_explore")
                   + ",\n \"background\": true"),
        long_answer("Обзор поставлен в фон, жду результата."),
        child_answer_text,
        long_answer("Итог обзора получен, продолжаю.") };
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [](AgentEvent::Kind, const std::string&) {});
    loop.run(engine().build_system_prompt(), response);

    /* Ход родителя — два запроса: постановка и итог. Третьего нет: субагент
     * не пошёл, и это и есть «возврат сразу». Число считается здесь, а не
     * подгоняется под удобное. */
    ASSERT_EQ(fx.host.calls(), (size_t)2);
    if (fx.host.calls() != 2) {
        for (size_t i = 0; i < fx.host.calls(); ++i) {
            std::cerr << "  запрос " << i << ": "
                      << text::utf8_prefix(fx.host.transcript(i), 200)
                      << std::endl;
        }
    }
    /* И ни файла задачи: работа не начата. */
    ASSERT_EQ(child_session_files(fx.project).size(), (size_t)0);

    /* Ответ вызова — принятие, а не итог: ни текста ребёнка, ни обёртки
     * задачи в нём быть не может, потому что их ещё не существует. */
    const std::vector<Message> after_call = fx.history();
    const std::string task_id = queued_id(after_call);
    if (!SessionArchive::is_session_id(task_id)) {
        std::cerr << "  у фоновой постановки нет идентификатора: «"
                  << task_id << "»" << std::endl;
    }
    ASSERT_TRUE(SessionArchive::is_session_id(task_id));
    const MessagePart* part = nullptr;
    for (const Message& m : after_call) {
        if (const MessagePart* p = tool_part(m, "task")) part = p;
    }
    ASSERT_TRUE(part != nullptr);
    const std::string accepted = part->output().output;
    if (accepted.find(child_answer_text) != std::string::npos) {
        std::cerr << "  вызов task вернул итог фоновой задачи: "
                  << text::utf8_prefix(accepted, 200) << std::endl;
    }
    ASSERT_TRUE(accepted.find(child_answer_text) == std::string::npos);
    ASSERT_TRUE(accepted.find("<task id=") == std::string::npos);
    /* Принятие называет задачу (её идентификатор) и ЗАПРЕЩАЕТ опрос —
     * без этого запрета модель опрашивала бы задачу повторными вызовами,
     * а они отклоняются (следующая проверка). */
    ASSERT_TRUE(accepted.find(task_id) != std::string::npos);
    if (accepted.find("НЕ ОПРАШИВАЙ") == std::string::npos) {
        std::cerr << "  принятие не запрещает опрос прогресса: "
                  << text::utf8_prefix(accepted, 300) << std::endl;
    }
    ASSERT_TRUE(accepted.find("НЕ ОПРАШИВАЙ") != std::string::npos);

    /* --- Разбор очереди: ход ребёнка и ход, который читает результат --- */
    engine().deliver_background_tasks();

    /* Четыре запроса: два шага родителя, ход ребёнка и ход родителя,
     * которому модель читает доставленный результат. */
    ASSERT_EQ(fx.host.calls(), (size_t)4);
    if (fx.host.calls() != 4) {
        for (size_t i = 0; i < fx.host.calls(); ++i) {
            std::cerr << "  запрос " << i << ": "
                      << text::utf8_prefix(fx.host.transcript(i), 200)
                      << std::endl;
        }
    }
    /* Ребёнок отработал ПОСЛЕ хода родителя (его запрос — третий), а не
     * вместо него: иначе «фон» был бы обычным вызовом, просто с другой
     * подписью. Тот же порядок — и причина, по которой область выполнения
     * не конкурентна. */
    ASSERT_TRUE(fx.host.sys(2).find(kAgentExplorePrompt) != std::string::npos);
    ASSERT_TRUE(fx.host.sys(1).find(kAgentExplorePrompt) == std::string::npos);
    ASSERT_TRUE(fx.host.sys(0).find(kAgentExplorePrompt) == std::string::npos);
    /* И в шаге, который поставил задачу, итога не было — доставка не
     * подмешалась в ход, который её вызвал. */
    ASSERT_TRUE(fx.host.transcript(1).find(child_answer_text) ==
                std::string::npos);

    /* ГЛАВНОЕ: результат ПРОЧИТАЛА модель. Проверяется транскрипт запроса,
     * а не состояние сессии: «где-то в сессии лежит» ничего не говорит о
     * том, дошло ли до модели. */
    const std::string read_back = fx.host.transcript(3);
    const std::string opening = "<task id=\"" + task_id + "\" state=\"completed\">";
    if (read_back.find(child_answer_text) == std::string::npos ||
        read_back.find(opening) == std::string::npos) {
        std::cerr << "  модель не увидела результат фоновой задачи: "
                  << text::utf8_prefix(read_back, 400) << std::endl;
    }
    ASSERT_TRUE(read_back.find(opening) != std::string::npos);
    ASSERT_TRUE(read_back.find(child_answer_text) != std::string::npos);
    /* Доставка названа словами: молчаливое сообщение с обёрткой читалось бы
     * как обычная реплика пользователя. */
    ASSERT_TRUE(read_back.find("[фоновые задачи завершены]") !=
                std::string::npos);

    /* Дочерняя сессия записана — по идентификатору из постановки её можно
     * продолжить (И8.9). Файл один: ровно одна задача была и осталась. */
    const std::vector<std::string> files = child_session_files(fx.project);
    ASSERT_EQ(files.size(), (size_t)1);
    if (files.size() == 1) {
        ASSERT_TRUE(files[0].find(task_id) != std::string::npos);
    }
    /* Очередь разобрана: повторный разбор не должен был бы выполнить ту же
     * задачу ещё раз. */
    ASSERT_EQ(background_queue_size(), (size_t)0);
}

TEST(a_queued_background_task_runs_under_its_own_rules_and_gives_them_back) {
    TaskFixture fx;
    /* Ответы: ход фонового ребёнка, затем ответ родителя, которому модель
     * читает доставленный итог. Второй ответ — полноценный итог, иначе
     * цикл родителя не счёл бы ход законченным и пошёл бы на следующий
     * шаг (проверке это ни к чему). */
    reset_host(fx.host, {"Сессия создаётся в core/session_store.cpp.",
                         long_answer("Итог обзора получен, продолжаю.")});
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* Промпт сессии снимается ДО фоновой работы: он и есть эталон, с
     * которым сравнивается состояние после (то же, что у вложенного хода
     * в разделе 2). */
    const std::string session_sys = engine().build_system_prompt();

    queue_background_task("wp_explore", "Обзор", "Посмотри проект.");
    ASSERT_EQ(fx.host.calls(), (size_t)0);

    engine().deliver_background_tasks();
    /* Два запроса: ход ребёнка и ход родителя, читающий результат. Промпт
     * ребёнка собран по ПРАВИЛАМ РЕБЁНКА: у wp_explore есть чтение и нет
     * правок (И8.6). Область подменяется на время фоновой работы так же,
     * как на время обычного вложенного хода. */
    ASSERT_EQ(fx.host.calls(), (size_t)2);
    if (fx.host.calls() != 2) {
        for (size_t i = 0; i < fx.host.calls(); ++i) {
            std::cerr << "  запрос " << i << ": "
                      << text::utf8_prefix(fx.host.transcript(i), 200)
                      << std::endl;
        }
    }
    const std::string child_sys = fx.host.sys(0);
    ASSERT_TRUE(child_sys.find(kAgentExplorePrompt) != std::string::npos);
    ASSERT_TRUE(catalogue_mentions(child_sys, "read_file"));
    if (catalogue_mentions(child_sys, "write_file")) {
        std::cerr << "  у фонового wp_explore появились правки" << std::endl;
    }
    ASSERT_FALSE(catalogue_mentions(child_sys, "write_file"));

    /* Область и промпт сессии возвращены: иначе СЛЕДУЮЩИЙ ход родителя ушёл
     * бы с промптом ребёнка и его набором инструментов. */
    const RunScope after = engine().scope_snapshot();
    ASSERT_EQ(after.agent, std::string(""));
    ASSERT_TRUE(after.info == nullptr);
    ASSERT_EQ(after.depth, 0);
    ASSERT_EQ(engine().build_system_prompt(), session_sys);
    ASSERT_TRUE(catalogue_mentions(session_sys, "write_file"));
}

TEST(a_queued_task_cannot_be_polled_before_it_has_run) {
    TaskFixture fx;
    reset_host(fx.host, {"Итог фоновой задачи."});
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    const ToolOutput queued = queue_background_task(
        "wp_general", "Обзор", "Посмотри проект.");
    const std::string task_id = queued.metadata.get_string("session_id");
    ASSERT_TRUE(SessionArchive::is_session_id(task_id));
    ASSERT_EQ(background_queue_size(), (size_t)1);

    /* ОПРОС: тот же вызов с идентификатором поставленной задачи. Отказ
     * обязан быть здесь, а не «продолжением»: дочерней сессии у живой
     * задачи ещё нет, и без этой проверки модель получила бы «задача не
     * найдена» на заведомо идущей работе и решила бы, что постановка не
     * сработала. */
    const ToolOutput poll = run_task_with_id("Обзор", "Ещё раз?", task_id);
    if (poll.output.find(task_id) == std::string::npos ||
        poll.output.find("[ошибка] task") == std::string::npos) {
        std::cerr << "  опрос фоновой задачи не отклонён: "
                  << text::utf8_prefix(poll.output, 300) << std::endl;
    }
    ASSERT_TRUE(poll.output.find("[ошибка] task") != std::string::npos);
    ASSERT_TRUE(poll.output.find(task_id) != std::string::npos);
    /* И сказано, ЧТО не так: «не найдена» модель поняла бы как «сбой». */
    ASSERT_TRUE(poll.output.find("в фоне") != std::string::npos);
    /* Опрос не выполнил задачу и не поставил вторую. */
    ASSERT_EQ(fx.host.calls(), (size_t)0);
    ASSERT_EQ(background_queue_size(), (size_t)1);

    /* А после выполнения тот же вызов — законное продолжение, а не опрос:
     * дочерняя сессия появилась, и отказать в нём уже нельзя (И8.9). */
    engine().deliver_background_tasks();
    ASSERT_EQ(background_queue_size(), (size_t)0);
    reset_host(fx.host, {"Продолжение: уточнил."});
    const ToolOutput after_run = run_task_with_id("Обзор", "Ещё раз?", task_id);
    if (after_run.output.find("[ошибка] task") != std::string::npos) {
        std::cerr << "  законченная фоновая задача не продолжилась: "
                  << text::utf8_prefix(after_run.output, 300) << std::endl;
    }
    ASSERT_TRUE(after_run.output.find("[ошибка] task") == std::string::npos);
    ASSERT_TRUE(after_run.output.find(task_id) != std::string::npos);
}

TEST(a_background_task_keeps_the_depth_it_was_queued_at) {
    TaskFixture fx;
    /* Порядок: ход делегирующего агента ставит фоновую задачу, следующий
     * его ход читает отказ. */
    reset_host(fx.host, {
        call_block("task", task_args("Вложенная в фон", "Ещё глубже.",
                                     "wp_general")
                   + ",\n \"background\": true"),
        "Вложенная фоновая задача не понадобилась: я сделал это сам." });
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    ASSERT_TRUE(add_delegator());

    run_task_agent("wp_delegator", "Внешняя задача", "Сделай что-то.");

    /* Глубина считается в момент ПОСТАНОВКИ: задача, поставленная
     * субагентом, — это второй уровень, и предел (1) отказывает ей здесь,
     * а не через два шага, когда её очередь разберут. Разбор очереди не
     * состоялся — отказ был раньше, и ни одной фоновой задачи в очереди
     * быть не должно. */
    const std::string expected = "Subagent depth limit reached (1).";
    if (fx.host.transcript(1).find(expected) == std::string::npos) {
        std::cerr << "  субагент не увидел отказ по глубине в фоне: "
                  << text::utf8_prefix(fx.host.transcript(1), 300) << std::endl;
    }
    ASSERT_TRUE(fx.host.transcript(1).find(expected) != std::string::npos);
    ASSERT_EQ(background_queue_size(), (size_t)0);
}

TEST(the_automatic_turn_for_background_results_is_bounded_and_resettable) {
    /* Предел цепочки — limits::kBackgroundTurnLimit, и проверка идёт по
     * ОБЕИМ его сторонам: при одном шаге до предела ход начинается, на
     * пределе — нет. Односторонняя проверка пропустила бы вдвое больше
     * автоматических ходов, чем задумано. */
    for (int turns : {limits::kBackgroundTurnLimit - 1,
                      limits::kBackgroundTurnLimit}) {
        TaskFixture fx;
        reset_host(fx.host, {"Итог фоновой задачи.",
                             long_answer("Итог получен, продолжаю.")});
        HostCallbacks cb = fx.callbacks();
        engine().init(cb);
        fx.prepare();
        reset_registry();
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().background_turns = turns;
        }
        const size_t events_before = engine_state().events.size();

        queue_background_task("wp_general", "Обзор", "Посмотри проект.");
        /* Состояние ДО разбора — то, что оставил ход родителя: его
         * cleanup ставит Done. Оно и есть эталон: разбор очереди не ход, и
         * оставленное «исполняется» на пределе цепочки (где следующего хода
         * не будет) висело бы в UI до чужого запроса. */
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().state = AgentState::Done;
        }
        engine().deliver_background_tasks();

        /* Задача выполнена в обоих случаях: на пределе цепочки не
         * останавливается работа, а только автоматический ход, который её
         * читает. */
        ASSERT_EQ(fx.host.calls(), (size_t)(turns < limits::kBackgroundTurnLimit
                                                ? 2 : 1));
        /* Результат в сессию положен в обоих случаях — иначе работа
         * потеряла бы смысл вместе с ходом. */
        bool delivered = false;
        for (const Message& m : fx.history()) {
            if (m.to_model_string().find("<task id=\"ses_") != std::string::npos) {
                delivered = true;
            }
        }
        if (!delivered) {
            std::cerr << "  при background_turns=" << turns
                      << " результат не доставлен в сессию" << std::endl;
        }
        ASSERT_TRUE(delivered);

        /* На пределе человек узнаёт, что делать дальше: молча оставленный
         * в сессии результат модель прочла бы только в следующий раз. */
        std::string tail;
        for (size_t i = events_before; i < engine_state().events.size(); ++i) {
            tail += engine_state().events[i].text + "\n";
        }
        const bool over = turns >= limits::kBackgroundTurnLimit;
        if (over && tail.find(std::to_string(limits::kBackgroundTurnLimit)) ==
                        std::string::npos) {
            std::cerr << "  на пределе цепочки не сказано, какой он: "
                      << text::utf8_prefix(tail, 300) << std::endl;
        }
        ASSERT_TRUE(!over ||
                    tail.find(std::to_string(limits::kBackgroundTurnLimit)) !=
                        std::string::npos);
        ASSERT_EQ(background_turns_now(), over ? turns : turns + 1);
        /* Движок не остался «исполняется» ни в одном случае: при
         * автоматическом ходе состояние ставит его cleanup, на пределе —
         * возвращает разбор очереди. Проверяется здесь потому, что на
         * пределе цепочки ничего, кроме этого, и не происходит. */
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            if (engine_state().state != AgentState::Done) {
                std::cerr << "  после разбора очереди состояние движка — "
                          << agent_state_name(engine_state().state)
                          << ", а не Done" << std::endl;
            }
            ASSERT_TRUE(engine_state().state == AgentState::Done);
        }
    }

    /* Ответ человека обнуляет цепочку: после него модель снова может
     * работать обычными ходами, и лимит не копится на всю сессию. */
    TaskFixture fx;
    reset_host(fx.host, {"Итог."});
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().background_turns = limits::kBackgroundTurnLimit;
    }
    engine().submit("ещё одна задача");
    if (background_turns_now() != 0) {
        std::cerr << "  запрос человека не обнулил цепочку" << std::endl;
    }
    ASSERT_EQ(background_turns_now(), 0);
    /* submit() кладёт задачу в очередь воркера и трогает состояние хода —
     * возвращаем чистоту, иначе следующий тест увидит чужой ход. */
    fx.prepare();
}

TEST(a_background_task_carries_the_depth_it_was_queued_at) {
    TaskFixture fx;
    /* Предел 2 — иначе фоновую задачу субагента не поставить вовсе, и
     * глубина, с которой она уйдёт, была бы неразличима (см. предыдущую
     * проверку). Настройка читается в Engine::load_settings, то есть
     * проверка идёт по НАСТОЯЩЕМУ её чтению. */
    fx.settings["wp_coder.subagent_depth"] = "2";
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();
    ASSERT_TRUE(add_delegator());
    ASSERT_EQ(depth_limit(), 2);

    /* Ответы имитатора (общая очередь, первый достаётся тому, кто спросил):
     *   0,1 — делегирующий субагент: постановка фоновой задачи и его итог;
     *   2,3 — ход ПОСТАВЛЕННОЙ задачи: его попытка делегировать дальше и
     *         его шаг, в котором он читает отказ;
     *   4 — ход родителя, читающий доставленный результат. */
    reset_host(fx.host, {
        call_block("task", task_args("Вложенная в фон", "Ещё глубже.",
                                     "wp_delegator")
                   + ",\n \"background\": true"),
        long_answer("Фоновая задача поставлена, жду."),
        call_block("task", task_args("Третий уровень", "Слишком глубоко.",
                                     "wp_delegator")
                   + ",\n \"background\": true"),
        long_answer("Глубже вкладывать нельзя, сделал сам."),
        long_answer("Результат фоновой задачи получен.")});

    /* Субагент на глубине 1 ставит фоновую задачу: она уходит на глубине 2,
     * и предел 2 её разрешает. */
    run_task_agent("wp_delegator", "Внешняя задача", "Сделай что-то.");
    ASSERT_EQ(fx.host.calls(), (size_t)2);
    ASSERT_EQ(background_queue_size(), (size_t)1);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        ASSERT_TRUE(!engine_state().background_tasks.empty());
        if (!engine_state().background_tasks.empty()) {
            const int queued_depth = engine_state().background_tasks.front().depth;
            if (queued_depth != 2) {
                std::cerr << "  фоновая задача записана с глубиной "
                          << queued_depth << ", а поставлена была с глубины 2"
                          << std::endl;
            }
            ASSERT_EQ(queued_depth, 2);
        }
    }

    engine().deliver_background_tasks();

    /* Пять запросов — по два на каждый из двух ходов субагентов и один на
     * ход родителя. Третьего уровня нет: его отказ виден в транскрипте
     * запроса 3. */
    ASSERT_EQ(fx.host.calls(), (size_t)5);
    if (fx.host.calls() != 5) {
        for (size_t i = 0; i < fx.host.calls(); ++i) {
            std::cerr << "  запрос " << i << ": "
                      << text::utf8_prefix(fx.host.transcript(i), 160)
                      << std::endl;
        }
    }
    /* ГЛАВНОЕ: поставленная задача работает на ТОЙ глубине, с какой её
     * поставили (2), поэтому её собственная попытка вложиться дальше (3)
     * отклонена. Если бы глубина взялась заново — при разборе очереди, где
     * область принадлежит сессии, — она была бы 1, вложение прошло бы, и
     * фоновая задача стала бы обходом предела вложенности. */
    const std::string expected = "Subagent depth limit reached (2).";
    if (fx.host.transcript(3).find(expected) == std::string::npos) {
        std::cerr << "  поставленная в фон задача увидела не тот отказ: "
                  << text::utf8_prefix(fx.host.transcript(3), 300) << std::endl;
    }
    ASSERT_TRUE(fx.host.transcript(3).find(expected) != std::string::npos);
    /* И вложенной фоновой задачи не появилось: отказ был до постановки. */
    ASSERT_EQ(background_queue_size(), (size_t)0);
}

TEST(a_stopped_background_task_is_not_delivered_into_the_dialog) {
    TaskFixture fx;
    /* Имитатор ждёт отмены: без этого «стоп» нечего отменять, и проверка
     * прошла бы при коде, который ничего не останавливает. */
    fx.host.wait_for_abort = true;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    /* Две задачи в очереди: отмена посреди работы должна остановить первую и
     * НЕ ДОПУСТИТЬ старта второй — иначе «стоп» означал бы «останови первую
     * из пяти», а человек ждал бы остановки всей работы. */
    queue_background_task("wp_general", "Первая", "Первое задание.");
    queue_background_task("wp_general", "Вторая", "Второе задание.");
    ASSERT_EQ(background_queue_size(), (size_t)2);
    const size_t events_before = engine_state().events.size();
    /* Состояние до разбора — то, что оставил ход родителя (его cleanup
     * ставит Done). После «стоп» оно обязано стать Aborted, а не вернуться
     * в Done: И6.8 разводит отмену и сбой именно по этому признаку, и
     * «готово» после нажатой кнопки было бы враньём. */
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().state = AgentState::Done;
    }
    /* Поток отмены отпускается сразу: ждать его нечего, он и нужен затем,
     * чтобы отмена пришла, пока ребёнок в запросе. */
    abort_when_the_subagent_is_in_flight(fx.host);

    engine().deliver_background_tasks();
    /* Ровно один запрос — ход первой задачи, прерванный отменой. Второй не
     * начался (при снятой проверке отмены запросов стало бы два, и каждый
     * прожил бы все 10 с ожидания — тест остался бы верным по смыслу, но
     * медленным; счётчик запросов ловит мутацию сразу). */
    ASSERT_EQ(fx.host.calls(), (size_t)1);

    /* Результат в диалог НЕ доставлен: цикл на отмене чистит историю, и
     * доставка в неё воскресила бы диалог, от которого человек отказался. */
    for (const Message& m : fx.history()) {
        if (!contains(m.to_model_string(), "<task id=\"ses_")) continue;
        std::cerr << "  прерванная фоновая задача доставлена в диалог"
                  << std::endl;
        ASSERT_TRUE(false);
    }
    /* И автоматического хода не было: читать нечего. */
    ASSERT_EQ(background_turns_now(), 0);

    /* Человек узнаёт, что произошло, и что работа сохранена: молча пропавшие
     * фоновые задачи выглядели бы как «агент забыл». Названо и СКОЛЬКО
     * выполнено — иначе «остановлены» читалось бы как «ничего не
     * началось» при обратном. */
    std::string tail;
    for (size_t i = events_before; i < engine_state().events.size(); ++i) {
        tail += engine_state().events[i].text + "\n";
    }
    if (!contains(tail, "НЕ доставлены")) {
        std::cerr << "  остановка фоновых задач не сказана человеку: "
                  << text::utf8_prefix(tail, 300) << std::endl;
    }
    ASSERT_TRUE(contains(tail, "НЕ доставлены"));
    ASSERT_TRUE(contains(tail, "1 из 2"));
    /* Состояние — «прервано», а не «готово» и не «исполняется». */
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        if (engine_state().state != AgentState::Aborted) {
            std::cerr << "  после «стоп» состояние движка — "
                      << agent_state_name(engine_state().state)
                      << ", а не Aborted" << std::endl;
        }
        ASSERT_TRUE(engine_state().state == AgentState::Aborted);
    }
}

TEST(the_worker_runs_the_background_queue_after_every_turn) {
    /* Место, где фоновые задачи вообще начинают работать, — цикл воркера,
     * а он поток и в тесте не поднимается. Проверка поведения здесь
     * невозможна, поэтому она МЕХАНИЧЕСКАЯ: вызов обязан стоять в ТЕЛЕ
     * worker_main и ПОСЛЕ run_task.
     *
     * Тело вырезается явно, а не «от строки worker_main до конца файла»:
     * определение самой deliver_background_tasks() стоит в этом же файле
     * рядом, и первый вариант проверки находил ИМЯ ФУНКЦИИ вместо вызова —
     * прогон мутаций это вскрыл (закомментированный вызов проходил). */
    const std::string src = without_comments(read_file(plugin_root() / "core" /
                                                       "engine.cpp"));
    ASSERT_FALSE(src.empty());
    const size_t worker = src.find("void Engine::worker_main()");
    if (worker == std::string::npos) {
        std::cerr << "  в core/engine.cpp нет worker_main" << std::endl;
    }
    ASSERT_TRUE(worker != std::string::npos);
    /* Тело функции — до закрывающей скобки в начале строки. */
    const size_t end = src.find("\n}\n", worker);
    ASSERT_TRUE(end != std::string::npos);
    const std::string body = src.substr(worker, end - worker);

    const size_t run = body.find("run_task(task)");
    const size_t deliver = body.find("deliver_background_tasks()");
    if (deliver == std::string::npos) {
        std::cerr << "  worker_main не разбирает очередь фоновых задач"
                  << std::endl;
    }
    ASSERT_TRUE(deliver != std::string::npos);
    /* Порядок обязателен: до run_task фоновых задач ещё не поставлено, и
     * доставка была бы пустой. */
    ASSERT_TRUE(run != std::string::npos);
    if (deliver < run) {
        std::cerr << "  очередь разбирается до хода родителя" << std::endl;
    }
    ASSERT_TRUE(deliver > run);
}

TEST(stopping_the_engine_says_how_many_background_tasks_it_dropped) {
    TaskFixture fx;
    reset_host(fx.host, {"Итог."});
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    reset_registry();

    queue_background_task("wp_general", "Первая", "Первое задание.");
    queue_background_task("wp_general", "Вторая", "Второе задание.");
    ASSERT_EQ(background_queue_size(), (size_t)2);
    const size_t events_before = engine_state().events.size();

    engine().stop();

    /* Очередь снята: остановка движка не должна оставлять задачи, которые
     * никто и никогда не выполнит. */
    ASSERT_EQ(background_queue_size(), (size_t)0);
    /* И сказано СКОЛЬКО и ЧТО с ними: человек видел постановки, и молча
     * исчезнувшие фоновые задачи выглядели бы как «агент забыл». Число —
     * из очереди, а не подогнанное: оно и есть смысл сообщения. */
    std::string tail;
    for (size_t i = events_before; i < engine_state().events.size(); ++i) {
        tail += engine_state().events[i].text + "\n";
    }
    if (!contains(tail, "2") || !contains(tail, "не потеряна")) {
        std::cerr << "  остановка движка не сказала про снятые фоновые задачи: "
                  << text::utf8_prefix(tail, 300) << std::endl;
    }
    ASSERT_TRUE(contains(tail, "2"));
    ASSERT_TRUE(contains(tail, "не потеряна"));
    /* stop() поднимает shutting_down — возвращаем чистоту, иначе следующий
     * тест не смог бы ни запустить ход, ни разобрать очередь. */
    fx.prepare();
}

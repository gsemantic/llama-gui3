/*
 * test_engine_entry.cpp — И13.9: НАСТОЯЩИЙ путь входа в агента.
 *
 * Откуда задача. Покрытие `core/` оказалось 91 %, то есть формально критерий
 * 13.9 («≥ 80 %») был взят ещё до того, как его кто-нибудь померил, — но
 * список непокрытого показал другое: `Engine::start`, `worker_main`,
 * `run_task`, `wait_response`, `permission_reply`, `check_external_permission`
 * и `pending_discard` НЕ вызывались ни одним из 1389 тестов.
 *
 * Почему так вышло. Единственный вход в этот код — `Engine::start()`, а он
 * зовётся из `src/plugin_main.cpp:257`, а `src/` в сборку тестов не входит
 * (тот же класс, что И13.8 нашла для слотов деградации: слой живёт там, куда
 * тесты не доходят). Тесты звали `AgentLoop::run` напрямую, то есть
 * проверяли цикл и НЕ проверяли то, что его разворачивает: постановку в
 * очередь, запуск потока, планировщик, вывод исхода и очистку. А именно там
 * живут решения вроде «LLM не подключён — это сбой, а не успех» (И6.8) и
 * «одноразовое разрешение снимается в конце задачи».
 *
 * Что здесь проверяется — ПУТЬ, а не его части:
 *   submit → worker_main → run_task (планировщик → цикл) → cleanup → ответ.
 * Проверки идут на настоящем потоке движка, а не на подставном цикле:
 * подставной цикл прошёл бы при сломанной очереди и при сломанном ответе из
 * wait_response, то есть проверял бы не то место, где живут эти решения.
 *
 * Побочный предмет проверок: ответы человека. `permission_reply`,
 * `permission_pending`, `permission_allow_once`, `permission_allow_always`,
 * `permission_reject` — это ровно то, чем окно отвечает движку, и до сих пор
 * не было вызвано ниоткуда: тесты отвечали прямо в `PermissionEngine`,
 * обходя те самые четыре метода, ради которых существует `ui/coder_window.cpp`.
 *
 * Порядок в прогоне: файл стоит в CMakeLists сразу после test_engine.cpp —
 * раньше проверок, которым нужен чистый цикл, потому что он поднимает
 * НАСТОЯЩИЙ поток движка и оставляет в синглтоне следы (очередь, флаг
 * остановки, счётчики задачи).
 */

#include "test_framework.h"
#include "test_printers.h"
#include "test_support.h"

#include "../core/agent_components.h"
#include "../core/base_tools.h"
#include "../core/command_policy.h"
#include "../core/compaction.h"
#include "../core/engine.h"
#include "../core/json_utils.h"
#include "../core/permission.h"
#include "../core/permission_engine.h"
#include "../core/project.h"
#include "../core/snapshot.h"
#include "../core/text_edit.h"
#include "../core/tools_registry.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace coder;
namespace fs = std::filesystem;

namespace {

/* Инструменты нужны настоящие: проверка «движок дошёл до ответа» на пустом
 * реестре прошла бы, а цикл молча упал бы на первом же вызове — и такой
 * провал читался бы как «агент не дошёл до конца». */
void register_entry_tools() {
    static bool done = false;
    if (done) return;
    done = true;
    register_base_tools();
}

/* Хост-имитация. Ответы по очереди, последний повторяется: сколько шагов
 * насчитает агент, автор теста не решает. Заодно считает ТОЧНО, что было
 * спрошено: без счётчика «циклом прошла хотя бы одна задача» неотличима от
 * «цикл упал на первом же шаге, а ответ пришёл из заготовки». */
struct EntryHost {
    std::vector<std::string> replies;
    size_t next = 0;
    std::vector<std::vector<ModelMessage>> requests;
    /* Настройки, которые движок записывает сам (разрешённые внешние пути).
     * Без их записи «всегда» выглядело бы так же, как «один раз»: разница
     * видна только здесь. */
    std::vector<std::pair<std::string, std::string>> settings_written;
    bool connected = true;

    bool chat(const std::string& sys, const std::vector<ModelMessage>& msgs,
              LlmReply& out) {
        requests.push_back(msgs);
        if (replies.empty()) {
            out.error = "хост без ответов";
            return false;
        }
        out.content = replies[std::min(next, replies.size() - 1)];
        ++next;
        out.finish_reason = "stop";
        out.prompt_tokens = 120;
        out.completion_tokens = 30;
        return true;
    }

    size_t calls() const { return requests.size(); }

    std::string transcript(size_t index) const {
        std::string out;
        if (index >= requests.size()) return out;
        for (const ModelMessage& m : requests[index]) out += m.content + "\n";
        return out;
    }

    std::string whole_transcript() const {
        std::string out;
        for (size_t i = 0; i < requests.size(); ++i) out += transcript(i);
        return out;
    }
};

/* Правила разрешений: вход — сброс, выход — сброс. Правила живут в
 * синглтоне, и без сброса тест унаследовал бы решение предыдущего. */
struct PermissionGuard {
    PermissionGuard() { Engine::instance().permissions().reset(); }
    ~PermissionGuard() { Engine::instance().permissions().reset(); }
};

/* Окружение проверок НАСТОЯЩЕГО пути.
 *
 * Здесь важно всё, что перечислено в prepare(): Engine — синглтон, и путь
 * входа трогает состояние, которое обычные проверки цикла не трогают.
 * Отдельно две вещи, без которых путь не воспроизводится:
 *
 *   inbox — очередь задач. В `test_permission_engine.cpp` задача остаётся
 *   в очереди после своей проверки, и worker поднял бы ЕЁ первой: тест
 *   измерял бы чужую задачу. Очередь чистится ДО старта и ПОСЛЕ него.
 *
 *   shutting_down — `Engine::stop()` выставляет его навсегда, а второй
 *   `start()` поднимает поток, который сразу выходит. Тест, поднявший
 *   движок, обязан вернуть флаг в false, иначе следующая проверка, полагающаяся
 *   на цикл, прошла бы (или упала) по причине, которой нет в её предмете. */
struct EntryFixture {
    EntryHost host;
    fs::path project;
    PermissionGuard permissions;
    bool worker_started = false;

    EntryFixture() : permissions() {
        register_entry_tools();
        test_support::approve_all_permissions();
        project = fs::temp_directory_path() /
                  ("wp_coder_entry_" + std::to_string(::getpid()) + "_" +
                   std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(project / "data");
        register_tmp_tree(project.string());
        {
            std::ofstream f(project / "config.txt");
            f << "порт=8080\n";
        }
    }

    ~EntryFixture() {
        stop_worker();
        std::error_code ec;
        fs::remove_all(project, ec);
    }

    HostCallbacks callbacks() {
        HostCallbacks cb;
        EntryHost* h = &host;
        cb.llm_chat = [h](const std::string& sys,
                          const std::vector<ModelMessage>& msgs,
                          LlmReply& out) {
            return h->chat(sys, msgs, out);
        };
        cb.llm_is_connected = [this] { return host.connected; };
        cb.path_data_dir = [this] { return (project / "data").string(); };
        cb.settings_set = [this](const std::string& key, const std::string& value) {
            host.settings_written.emplace_back(key, value);
        };
        cb.settings_get = [](const std::string&, const std::string& d) { return d; };
        cb.chat_event = [](const std::string&) {};
        return cb;
    }

    /* init() идёт ПЕРВЫМ: он зовёт load_settings(), а та читает project_dir,
     * max_steps и прочее из настроек. Обратный порядок — тест измерял бы
     * настройки, а не свой сценарий. */
    void prepare(bool planning = true) {
        engine().init(callbacks());
        test_support::approve_all_permissions();
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        drain_inbox_locked();
        engine_state().session.clear();
        engine_state().project_dir = project.string();
        engine_state().mode = 0;
        engine_state().plan_mode = false;
        engine_state().use_planning = planning;
        engine_state().max_steps = 4;
        engine_state().session_budget = 60000;
        engine_state().todos.clear();
        engine_state().recent_calls.clear();
        engine_state().allowed_external_paths.clear();
        engine_state().once_path.clear();
        engine_state().pending.clear();
        engine_state().measured_input_tokens = 0;
        engine_state().model_limits = compaction::ModelLimits();
        engine_state().compaction_config = compaction::CompactionConfig();
        engine_state().abort_requested.store(false);
        engine_state().shutting_down = false;
        engine_state().continue_conversation = false;
        engine_state().preserve_session = false;
        engine_state().response_ready = false;
        engine_state().last_response.clear();
        engine_state().steps = 0;
        engine_state().outcome = TaskOutcome::None;
        engine_state().state = AgentState::Idle;
        engine_state().background_tasks.clear();
        engine_state().background_turns = 0;
        /* Область выполнения: чужой агент нашёлся бы правилами своими, и
         * PermissionGate спросил бы не сессию, а ребёнка (И8.10). Проверка
         * на внешний путь тогда измеряла бы не сессию. */
        engine_state().scope.agent.clear();
        engine_state().scope.info.reset();
        engine_state().scope.depth = 0;
        engine_state().last_snapshot = snapshot::Snapshot();
        engine_state().snapshots_taken = 0;
    }

    void drain_inbox_locked() {
        while (!engine_state().inbox.empty()) engine_state().inbox.pop();
    }

    void start_worker() {
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().shutting_down = false;
        }
        engine().start();
        worker_started = true;
    }

    void stop_worker() {
        if (!worker_started) return;
        worker_started = false;
        engine().stop();
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        /* stop() выставил флаг навсегда; возвращать его — дело теста,
         * который его поднял. */
        engine_state().shutting_down = false;
        engine_state().abort_requested.store(false);
        drain_inbox_locked();
        engine_state().session.clear();
        engine_state().scope.agent.clear();
        engine_state().scope.info.reset();
        engine_state().scope.depth = 0;
        engine_state().pending.clear();
        engine_state().allowed_external_paths.clear();
    }

    /* Ждать состояния движка. Опрос вместо condvar: состояние переводит
     * поток движка, а condvar на изменение состояния не подписан. */
    template <typename Pred>
    bool wait_until(Pred pred, int tries = 600) {
        for (int i = 0; i < tries; ++i) {
            {
                std::lock_guard<std::mutex> lk(engine_state().mtx);
                if (pred(engine_state())) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    std::vector<Message> history() {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        return engine_state().session;
    }

    std::string history_text() {
        std::string out;
        for (const Message& m : history()) out += m.text() + "\n";
        return out;
    }
};

/* Ответ модели той длины, которой требует правило завершения хода: короткий
 * текст без вызова инструмента цикл считает «началом работы». */
std::string long_answer(const std::string& tail) {
    std::string out = "Итог по задаче. ";
    while (out.size() < 320) out += "Работа выполнена, всё проверено. ";
    return out + tail;
}

std::string call_block(const char* tool, const std::string& json_args) {
    return std::string("Смотрю.\n```json\n{\"tool\": \"") + tool + "\"" +
           json_args + "}\n```";
}

/* План, который составит планировщик: обычный текст без вызова. */
const char* kPlanReply = "1. Прочитать config.txt. 2. Сказать, что в нём.";

} // namespace

/* ======================================================================
 * Путь целиком
 * ====================================================================== */

TEST(задача_проходит_от_очереди_до_ответа_на_настоящем_потоке_движка) {
    EntryFixture fx;
    fx.host.replies = {
        kPlanReply,
        call_block("read_file", ",\n \"path\": \"config.txt\""),
        long_answer("В файле порт 8080.")};
    fx.prepare(/*planning=*/true);

    fx.start_worker();
    engine().submit("что в config.txt?");
    const std::string answer = engine().wait_response(20000);

    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        /* Исход и состояние — решения И6.8, и они живут в cleanup(), а не
         * в цикле: подставной цикл их не проверяет вовсе. */
        if ((int)engine_state().outcome != (int)TaskOutcome::Completed) {
            std::cerr << "  исход: " << task_outcome_name(engine_state().outcome)
                      << ", причина: " << engine_state().outcome_reason
                      << std::endl;
        }
        ASSERT_EQ((int)engine_state().outcome, (int)TaskOutcome::Completed);
        ASSERT_EQ((int)engine_state().state, (int)AgentState::Done);
        /* Шаги посчитаны циклом, а не хостом: метрика приходит из хода. */
        ASSERT_EQ(engine_state().steps, 2);
        /* Метрики токенов копятся в ходе (record_turn_usage), и без них
         * панель токенов показывала бы нули на живой задаче. */
        ASSERT_TRUE(engine_state().total_completion_tokens > 0);
    }
    if (answer.find("порт 8080") == std::string::npos) {
        std::cerr << "  ответ без итога: " << text::utf8_prefix(answer, 200)
                  << std::endl;
    }
    ASSERT_TRUE(answer.find("порт 8080") != std::string::npos);

    /* Планировщик отработал ДО цикла: план лежит в истории отдельным
     * сообщением, и именно его находит message_is_plan. Без этой проверки
     * путь «спланировать → выполнить» выглядел бы так же, как путь без
     * планирования: тот же ответ, ноль планов. */
    const std::string history = fx.history_text();
    ASSERT_TRUE(history.find("[ПЛАН]") != std::string::npos);
    ASSERT_TRUE(history.find("Прочитать config.txt") != std::string::npos);

    /* Результат инструмента дошёл до модели — цикл закрыл ход. */
    ASSERT_TRUE(fx.host.whole_transcript().find("порт=8080") !=
                std::string::npos);
    /* Ровно три запроса: план, шаг с вызовом, финальный шаг. */
    ASSERT_EQ(fx.host.calls(), (size_t)3);

    fx.stop_worker();
}

/* Первое, что делает run_task, — спрашивает живой ли хост. Ответ «нет» обязан
 * давать СБОЙ, а не Done: иначе список последних задач показывал бы
 * выполненную задачу, которой не было (И6.8). Этот путь не проверялся ничем:
 * все 1389 тестов либо не доходили до run_task, либо подставляли живой хост. */
TEST(задача_без_модели_даёт_сбой_а_не_видимость_успеха) {
    EntryFixture fx;
    fx.host.replies = {long_answer("этот ответ не должен быть получен")};
    fx.host.connected = false;
    fx.prepare();

    fx.start_worker();
    engine().submit("спроси модель");
    const std::string answer = engine().wait_response(5000);

    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        ASSERT_EQ((int)engine_state().outcome, (int)TaskOutcome::Failed);
        ASSERT_EQ((int)engine_state().state, (int)AgentState::Error);
        ASSERT_EQ(engine_state().steps, 0);
    }
    ASSERT_TRUE(answer.find("LLM не подключён") != std::string::npos);
    /* Модель не звали: один запрос — это был бы обход раннего выхода. */
    ASSERT_EQ(fx.host.calls(), (size_t)0);

    fx.stop_worker();
}

/* ======================================================================
 * Ответы человека: те четыре метода, которыми пользуется окно
 * ====================================================================== */

TEST(вопрос_о_вызове_инструмента_человек_отвечает_через_движок_а_разовый_ответ_не_становится_постоянным) {
    EntryFixture fx;
    fx.host.replies = {
        kPlanReply,
        call_block("read_file", ",\n \"path\": \"config.txt\""),
        long_answer("Прочитал: порт 8080.")};
    fx.prepare();

    /* Вопрос задаёт ПРАВИЛО, а не заглушка теста: правило идёт после
     * approve_all, то есть перекрывает его (последнее совпадение выигрывает).
     * Так спрашивает настоящий PermissionEngine, и ответ «разовый» проверяется
     * на живой машине правил, а не подменой адресата вопроса. */
    Rule ask_read;
    ask_read.permission = "read";
    ask_read.pattern = "*";
    ask_read.action = PermissionAction::Ask;
    ask_read.comment = "тест: чтение спрашивает";
    engine().permissions().add_rule(ask_read);

    fx.start_worker();
    engine().submit("прочитай config.txt");

    /* Отвечает ТЕСТОВЫЙ поток — так же, как это делает окно: вопрос приходит
     * на ходу двигателя, и ждать его оттуда же нельзя, цикл на этом месте
     * стоит. Звать PermissionEngine напрямую нельзя ещё и по смыслу: именно
     * `permission_reply`/`permission_pending` — те два метода, которыми
     * пользуется ui/coder_window.cpp, и до сих пор они не были вызваны
     * ни одним тестом. */
    PermissionRequest question;
    const bool asked = fx.wait_until([](const EngineState&) {
        return !engine().permissions().pending().empty();
    });
    if (asked) question = engine().permission_pending().front();

    if (question.id == 0) {
        std::cerr << "  вопрос не задан: состояние "
                  << agent_state_name(engine_state().state)
                  << ", шагов " << engine_state().steps << std::endl;
    }
    ASSERT_TRUE(question.id != 0);
    ASSERT_EQ(question.permission, std::string("read"));
    engine().permission_reply(question.id, PermissionReply::Once);

    const std::string answer = engine().wait_response(20000);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        if ((int)engine_state().outcome != (int)TaskOutcome::Completed) {
            std::cerr << "  исход: " << task_outcome_name(engine_state().outcome)
                      << ", причина: " << engine_state().outcome_reason
                      << std::endl;
        }
        ASSERT_EQ((int)engine_state().outcome, (int)TaskOutcome::Completed);
    }
    /* Инструмент отработал ПОСЛЕ ответа человека: без этого проверка прошла
     * бы и на движке, который задал вопрос и пошёл дальше не глядя. */
    ASSERT_TRUE(fx.host.whole_transcript().find("порт=8080") != std::string::npos);
    ASSERT_TRUE(answer.find("8080") != std::string::npos);

    /* «Разовый» не пишет правило: после задачи чтение спрашивает снова, иначе
     * один ответ человека тихо разрешал бы инструмент навсегда (И13.6). */
    ASSERT_TRUE((int)engine().permissions().evaluate("read", "config.txt") ==
                (int)PermissionAction::Ask);

    fx.stop_worker();
}

/* ======================================================================
 * Путь вне проекта: гейт, ожидание и три ответа
 * ====================================================================== */

namespace {

/* Провести одну задачу до вопроса о внешнем файле и остановиться на нём.
 * Возвращает путь, о котором спросили (пустой — вопрос не задан). */
std::string run_until_external_question(EntryFixture& fx,
                                        const std::string& outside) {
    fx.host.replies = {
        kPlanReply,
        call_block("read_file", ",\n \"path\": \"" + outside + "\""),
        long_answer("этот ответ не должен быть получен")};
    fx.prepare();
    fx.start_worker();
    engine().submit("прочитай файл снаружи");
    fx.wait_until([](const EngineState& st) {
        return st.state == AgentState::WaitingPermission;
    });
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    return engine_state().pending_permission_path;
}

} // namespace

TEST(файл_вне_проекта_останавливает_ход_а_разовое_разрешение_забывается_в_конце_задачи) {
    EntryFixture fx;
    /* Файл ВНЕ проекта и ВНЕ доверенного каталога: /tmp и каталог данных в
     * разрешённых, и вопроса не было бы — то есть проверка прошла бы, ничего
     * не спросив. */
    const std::string outside = "/etc/hostname";
    ASSERT_TRUE(fs::exists(outside));

    const std::string asked = run_until_external_question(fx, outside);
    if (asked != outside) {
        std::cerr << "  спросили не про " << outside << ", а про «" << asked
                  << "»" << std::endl;
    }
    ASSERT_EQ(asked, outside);

    engine().permission_allow_once(outside);
    const std::string answer = engine().wait_response(20000);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        ASSERT_EQ((int)engine_state().outcome, (int)TaskOutcome::Completed);
        /* Очистка в конце задачи снимает разовое разрешение: иначе оно
         * пережило бы задачу и открыло бы файл всем следующим. */
        const std::vector<std::string>& paths = engine_state().allowed_external_paths;
        ASSERT_TRUE(std::find(paths.begin(), paths.end(), outside) == paths.end());
        ASSERT_EQ(engine_state().once_path, std::string(""));
        ASSERT_EQ(engine_state().pending_permission_path, std::string(""));
    }
    /* Человеку сказано ровно то, что решил он: разрешил — и модель это видит
     * в заметке, а не в пустом ходе. */
    const std::string history = fx.history_text();
    ASSERT_TRUE(history.find("пользователь разрешил доступ") != std::string::npos);
    ASSERT_TRUE(answer.find("8080") == std::string::npos);

    fx.stop_worker();
}

TEST(всегда_на_внешний_путь_переживает_задачу_и_пишется_в_настройки) {
    EntryFixture fx;
    const std::string outside = "/etc/hostname";
    ASSERT_TRUE(fs::exists(outside));

    const std::string asked = run_until_external_question(fx, outside);
    ASSERT_EQ(asked, outside);

    engine().permission_allow_always(outside);
    engine().wait_response(20000);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        const std::vector<std::string>& paths = engine_state().allowed_external_paths;
        /* Разница с «разовым» — ровно в этом: путь остаётся. Проверка без
         * контраста была бы зелёной и на «всегда», и на «разовый». */
        ASSERT_TRUE(std::find(paths.begin(), paths.end(), outside) != paths.end());
    }
    /* И решение уходит в настройки, иначе пережил бы только этот запуск. */
    bool found = false;
    for (const auto& kv : fx.host.settings_written) {
        if (kv.first == "wp_coder.allowed_external_paths" &&
            kv.second.find(outside) != std::string::npos) {
            found = true;
        }
    }
    if (!found) {
        std::cerr << "  в настройки не ушли внешние пути, записей: "
                  << fx.host.settings_written.size() << std::endl;
    }
    ASSERT_TRUE(found);

    fx.stop_worker();
}

TEST(отказ_оставляет_путь_закрытым_и_говорит_об_этом_модели) {
    EntryFixture fx;
    const std::string outside = "/etc/hostname";
    ASSERT_TRUE(fs::exists(outside));

    const std::string asked = run_until_external_question(fx, outside);
    ASSERT_EQ(asked, outside);

    engine().permission_reject(outside);
    engine().wait_response(20000);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        const std::vector<std::string>& paths = engine_state().allowed_external_paths;
        ASSERT_TRUE(std::find(paths.begin(), paths.end(), outside) == paths.end());
        /* Вопрос закрыт, а не подвешен: иначе цикл ждал бы вечно и ход не
         * закончился бы. */
        ASSERT_TRUE((int)engine_state().state != (int)AgentState::WaitingPermission);
    }
    ASSERT_TRUE(fx.history_text().find("пользователь ОТКАЗАЛ") !=
                std::string::npos);

    fx.stop_worker();
}

/* ======================================================================
 * Кнопка «отклонить» у предложенной правки
 * ====================================================================== */

TEST(отклонение_правки_убирает_нужное_и_чужой_индекс_не_трогает_список) {
    EntryFixture fx;
    fx.prepare(/*planning=*/false);

    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().pending.push_back(PendingWrite{"первая.txt", "1"});
        engine_state().pending.push_back(PendingWrite{"вторая.txt", "2"});
    }
    engine().pending_discard(0);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        /* После снятия первого индексы сдвигаются, и оставшимся должен быть
         * ВТОРОЙ: снять элемент по старому индексу здесь означало бы
         * стереть не то предложение. */
        ASSERT_EQ(engine_state().pending.size(), (size_t)1);
        ASSERT_EQ(engine_state().pending[0].path, std::string("вторая.txt"));
    }
    /* Индекс за пределами списка — не падение и не порча: окно зовёт это по
     * снимку начала кадра (D25), где индекс уже мог устареть. */
    engine().pending_discard(7);
    engine().pending_discard(0);
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        ASSERT_EQ(engine_state().pending.size(), (size_t)0);
    }
}

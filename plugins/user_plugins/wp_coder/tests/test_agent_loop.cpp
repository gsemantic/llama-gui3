/*
 * test_agent_loop.cpp — И5.7–И5.9: цикл агента на событиях.
 *
 * Проверяется то, ради чего писалась И5: ход модели ложится в историю
 * ОДНИМ сообщением с частями, вызов и его результат остаются связанными
 * по call_id, а транскрипт для модели содержит и то, что модель вызвала,
 * и то, что она получила.
 *
 * Почему нужен именно цикл, а не только свёртка (тест_llm_events.cpp) и
 * модель сообщений (test_message.cpp). Свёртка умеет собрать ответ, а
 * message.h — разложить историю; но «цикл правильно их соединил» не
 * проверяется ни тем, ни другим. Ошибка здесь выглядит правдоподобно:
 * агент отвечает, файлы меняются, а в истории через ход лежит «RESULT
 * [read_file]: …» без вызова, который его вызвал, — и модель на следующем
 * шаге не знает, что уже сделано.
 *
 * Хост здесь — имитация: отдаёт заранее заданные ответы и записывает всё,
 * что ему прислали. Ни сети, ни провайдера.
 */

#include "../core/prompts.h"   /* kCompactionSystemPrompt: запрос сводки виден по системному промпту */

#include "test_framework.h"
#include "test_printers.h"
#include "test_support.h"
#include "../core/agent_components.h"
#include "../core/base_tools.h"
#include "../core/snapshot.h"
#include "../core/git_tools.h"
#include "../core/tools_registry.h"
#include "../core/permission_engine.h"
#include "../modules/wordpress/wp_tools.h"
#include "../modules/python/python_tools.h"
#include "../modules/devops/devops_tools.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using namespace coder;
namespace fs = std::filesystem;

namespace {

/* Инструменты нужны настоящие: проверка «вызов closed и с результатом»
 * бессмысленна на заглушке, которая ничего не выполняет. */
void register_every_tool() {
    static bool done = false;
    if (done) return;
    done = true;
    register_base_tools();
    register_rag_tools();
    register_git_tools();
    wp::register_wp_tools();
    python::register_python_tools();
    devops::register_devops_tools();
}

/* Хост-имитация. */
struct FakeHost {
    /* Ответы по очереди. Последний ответ повторяется, если ходов больше:
     * так тест «цикл дошёл до конца» не зависит от того, сколько шагов
     * насчитал автор теста. */
    std::vector<std::string> replies;
    size_t next = 0;
    /* Что хост получал: по одному списку реплик на запрос. */
    std::vector<std::vector<ModelMessage>> requests;
    bool fail = false;
    std::string error = "сеть недоступна";
    /* Что сделать перед ответом: имитация того, что пользователь написал
     * в чат, пока агент работал. */
    std::function<void()> on_chat;
    /* Сколько входных токенов «померил» провайдер. По умолчанию 100: у
     * настоящего провайдера это правда, а тесты переполнения окна (И7.10)
     * обязаны подставлять своё число, иначе измеренное значение спрячет
     * оценку, а переполнение не наступит никогда. */
    long long prompt_tokens = 100;
    /* Ответ на запрос СВОДКИ. Пусто — такого запроса не бывает, и ветка
     * не трогает остальные тесты. */
    std::string compaction_reply;
    int compaction_calls = 0;

    bool chat(const std::string& sys, const std::vector<ModelMessage>& msgs,
              LlmReply& out) {
        if (on_chat) on_chat();
        requests.push_back(msgs);
        if (!compaction_reply.empty() && sys == kCompactionSystemPrompt) {
            out.content = compaction_reply;
            out.finish_reason = "stop";
            out.prompt_tokens = prompt_tokens;
            out.completion_tokens = 20;
            ++compaction_calls;
            /* После сжатия провайдер меряет СЛЕДУЮЩИЙ запрос, а он уже
             * маленький. Без этого тест утверждал бы, что цикл сжимает
             * историю на каждом шаге, — но только потому, что имитатор
             * продолжает врать про 15000 токенов. Повторное сжатие на
             * реальных числах проверяется у движка отдельной проверкой. */
            prompt_tokens = 500;
            return true;
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

    /* Весь текст, который увидела модель в n-м запросе. */
    std::string transcript(size_t index) const {
        std::string out;
        if (index >= requests.size()) return out;
        for (const ModelMessage& m : requests[index]) out += m.content + "\n";
        return out;
    }
};

/* Чистые правила разрешений на входе и на выходе теста: правила живут в
 * синглтоне, и без сброса тест унаследовал бы решения предыдущего. */
struct PermissionGuard {
    PermissionGuard() { Engine::instance().permissions().reset(); }
    ~PermissionGuard() { Engine::instance().permissions().reset(); }
};

/* Счётчик совпадений — для «-refusal должен попасть в историю РОВНО ОДИН
 * раз»: раньше ToolRunner писал отказ в сессию сам, а цикл писал туда же
 * вторую строку, и модель видела один отказ как два результата. */
size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t n = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

/* Окружение одного теста цикла.
 *
 * Engine — синглтон, и состояние между тестами протекает, поэтому здесь
 * восстанавливается всё, что цикл меняет: сессия, режим, проект, лимиты,
 * план, разрешения и правила (PermissionGuard). Иначе тест измерял бы
 * то, что осталось от предыдущего. */
struct LoopFixture {
    FakeHost host;
    std::vector<std::string> pushed;   /* события, ушедшие в UI-лог */
    fs::path project;
    PermissionGuard permissions;

    /* Правила разрешений: пользователь разрешил всё, кроме внешних
     * каталогов (иначе заглушка отключила бы PermissionGate). */
    LoopFixture() : permissions() {
        register_every_tool();
        test_support::approve_all_permissions();
        project = fs::temp_directory_path() /
                  ("wp_coder_loop_" + std::to_string(::getpid()));
        fs::create_directories(project);
    }
    ~LoopFixture() {
        std::error_code ec;
        fs::remove_all(project, ec);
        engine_state().session.clear();
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

    /* Чистое состояние цикла: без планирования (оно задаёт отдельный вызов
     * LLM, а проверяем мы ход), с коротким лимитом шагов. */
    void prepare(int max_steps = 6) {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().session.clear();
        engine_state().session.push_back(Message::user("посчитай навыки"));
        engine_state().project_dir = project.string();
        engine_state().mode = 0;
        engine_state().plan_mode = false;
        engine_state().use_planning = false;
        engine_state().max_steps = max_steps;
        engine_state().session_budget = 60000;
        engine_state().todos.clear();
        engine_state().recent_calls.clear();
        engine_state().allowed_external_paths.clear();
        /* И7.2: измерение контекста тоже состояние синглона, и без
         * сброса тест унаследовал бы измерение чужого хода. */
        engine_state().measured_input_tokens = 0;
        /* И7.10: то же и про окно с настройками сжатия. Без сброса тест
         * унаследовал бы чужие лимиты (и сжатие молча выключилось бы, если
         * у кого-то стоял compaction_auto=false) — и проверял бы не цикл,
         * а чужую настройку. Числа нулевые: лимиты неизвестны → сжатия
         * нет, и это состояние ПО УМОЛЧАНИЮ для тестов, которые про сжатие
         * не думают. */
        engine_state().model_limits = compaction::ModelLimits();
        engine_state().compaction_config = compaction::CompactionConfig();
        engine_state().abort_requested.store(false);
        engine_state().shutting_down = false;
        engine_state().state = AgentState::Executing;
        engine_state().steps = 0;
        /* И10.1: счётчик снимков — тоже состояние синглона. Без сброса
         * проверка «цикл снимает снимок на каждом шаге» считала бы чужие
         * шаги и прошла бы на цикле, который не снимает ничего. */
        engine_state().last_snapshot = snapshot::Snapshot();
        engine_state().snapshots_taken = 0;
    }

    /* Прогон цикла. Возвращается признак «задача завершена» из самого
     * AgentLoop::run, а не «ответ непустой»: два разных исхода
     * (провал провайдера и пустой ответ) выглядели бы в тесте одинаково. */
    bool run(AgentLoop& loop, std::string& response) {
        return loop.run("системный промпт", response);
    }

    std::vector<Message> history() {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        return engine_state().session;
    }
};

/* Итоговый ответ модели такой длины, какой требует текущее правило
 * завершения хода: короткий текст без вызова инструмента цикл считает
 * «началом работы» и просит продолжить (kFinalAnswerMinLen). И5.8 заменит
 * это правило на finish-семантику, и тест перестанет зависеть от длины. */
std::string long_answer(const std::string& tail) {
    std::string out = "Итог по задаче. ";
    while (out.size() < 320) out += "Работа выполнена, всё проверено. ";
    return out + tail;
}

std::string call_block(const char* tool, const char* json_args) {
    return std::string("Смотрю.\n```json\n{\"tool\": \"") + tool + "\"" +
           json_args + "}\n```";
}

/* Первая часть-инструмент хода. */
const MessagePart* first_tool_part(const Message& m) {
    for (const MessagePart& p : m.parts) {
        if (p.is(PartKind::Tool)) return &p;
    }
    return nullptr;
}

} // namespace

/* ======================================================================
 * И5.7: ход = одно сообщение с частями
 * ====================================================================== */

TEST(agent_loop_writes_one_assistant_message_per_turn) {
    LoopFixture fx;
    fx.host.replies = {call_block("list_skills", ""),
                       long_answer("Навыки: wp, python, devops.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string& text) {
                       fx.pushed.push_back(text);
                   });
    fx.run(loop, response);

    const std::vector<Message> h = fx.history();
    /* Ровно три сообщения: задача, ход с вызовом, ход с ответом. Раньше
     * ход с вызовом давал четыре сообщения (текст, вызов, «RESULT [x]:»,
     * следующий текст), и модель читала текст после результата как
     * отдельную реплику. */
    ASSERT_EQ(h.size(), (size_t)3);
    ASSERT_TRUE(h[0].is_user());
    ASSERT_TRUE(h[1].is_assistant());
    ASSERT_TRUE(h[2].is_assistant());
    /* Ход с вызовом: текст и вызов внутри ОДНОГО сообщения. */
    ASSERT_EQ(h[1].parts.size(), (size_t)2);
    ASSERT_TRUE(h[1].parts[0].is(PartKind::Text));
    ASSERT_EQ(h[1].parts[0].text(), std::string("Смотрю."));

    /* parent_id хода — на сообщение, его породившее. */
    ASSERT_EQ(h[1].parent_id, h[0].id);
    ASSERT_EQ(h[2].parent_id, h[0].id);
    ASSERT_TRUE(!h[1].id.empty());
    ASSERT_TRUE(h[1].id != h[2].id);

    /* Текст ответа не потерян по дороге. */
    ASSERT_TRUE(h[2].text().find("Навыки: wp, python, devops.") != std::string::npos);
    ASSERT_TRUE(response.find("Навыки: wp, python, devops.") !=
                std::string::npos);
}

TEST(agent_loop_closes_the_tool_part_with_the_result) {
    LoopFixture fx;
    fx.host.replies = {call_block("list_skills", ""), long_answer("Готово.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    ASSERT_TRUE(fx.run(loop, response));

    const std::vector<Message> h = fx.history();
    const MessagePart* part = first_tool_part(h[1]);
    ASSERT_TRUE(part != nullptr);
    ASSERT_EQ(part->tool_name(), std::string("list_skills"));
    ASSERT_EQ(part->state(), ToolState::Completed);
    ASSERT_TRUE(part->has_result());
    ASSERT_FALSE(part->is_open());
    ASSERT_TRUE(!part->output().output.empty());
    /* Заголовок результата — часть ToolOutput, а не потерянная строка:
     * раньше ToolRunner::run возвращал только вывод, и в истории оставалось
     * голое тело без названия операции. */
    ASSERT_TRUE(!part->output().title.empty());
    /* Сырой блок вызова сохранён: модель на следующем шаге должна увидеть
     * тот же текст, который написала сама. */
    ASSERT_TRUE(part->raw_call().find("list_skills") != std::string::npos);
    /* Идентификаторы уникальны: два сообщения с одним id слиплись бы при
     * сохранении (И5.5). */
    ASSERT_EQ(ids().last() > 0, true);
}

TEST(agent_loop_gives_the_model_both_the_call_and_its_result) {
    LoopFixture fx;
    fx.host.replies = {call_block("list_skills", ""), long_answer("Готово.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    fx.run(loop, response);

    ASSERT_EQ(fx.host.calls(), (size_t)2);
    const std::string second = fx.host.transcript(1);
    /* И вызов, и результат: раньше цикл заменял содержимое сообщения
     * блоком вызова, и модель на следующем шаге не видела, что
     * вызывала, либо (в другой ветке) не видела результата. */
    ASSERT_TRUE(second.find("list_skills") != std::string::npos);
    ASSERT_TRUE(second.find("RESULT [list_skills]:") != std::string::npos);
    /* И ровно по одному разу каждый. Два одинаковых «RESULT [x]:» означали бы
     * два вызова, которых не было, а два одинаковых блока вызова — протокол
     * в транскрипте дважды: один как текст, другой как вызов. Именно это
     * и было устройством цикла до И5.7, где блок вырезался из текста
     * эвристикой (шесть догадок вместо одного факта от провайдера). */
    ASSERT_EQ(count_occurrences(second, "RESULT [list_skills]:"), (size_t)1);
    ASSERT_EQ(count_occurrences(second, "\"tool\": \"list_skills\""), (size_t)1);
}

TEST(agent_loop_does_not_invent_a_second_copy_of_a_refusal) {
    LoopFixture fx;
    /* Инструмента нет: отказ — самый частый исход, и именно он раньше
     * записывался в историю дважды (ToolRunner писал строку сам, цикл
     * писал вторую). */
    fx.host.replies = {call_block("no_such_tool", ""), long_answer("Инструмента нет.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    ASSERT_TRUE(fx.run(loop, response));

    const std::vector<Message> h = fx.history();
    const MessagePart* part = first_tool_part(h[1]);
    ASSERT_TRUE(part != nullptr);
    ASSERT_EQ(part->state(), ToolState::Error);
    ASSERT_TRUE(part->error().find("неизвестный инструмент") !=
                std::string::npos);
    const std::string second = fx.host.transcript(1);
    ASSERT_EQ(count_occurrences(second, "неизвестный инструмент"), (size_t)1);
}

TEST(agent_loop_reports_a_provider_failure_without_adding_a_turn) {
    LoopFixture fx;
    fx.host.fail = true;
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string& text) {
                       fx.pushed.push_back(text);
                   });
    const bool ok = fx.run(loop, response);

    /* Провал провайдера — это провал задачи, а не «пустой ответ»: цикл
     * возвращает false, иначе run_task отметил бы задачу выполненной. */
    ASSERT_FALSE(ok);
    /* Причина видна пользователю и в ответе, и в логе. */
    ASSERT_TRUE(response.find("сеть недоступна") != std::string::npos);
    bool logged = false;
    for (const std::string& t : fx.pushed) {
        if (t.find("сеть недоступна") != std::string::npos) logged = true;
    }
    ASSERT_TRUE(logged);
    /* Незакрытого вызова в истории не осталось. */
    for (const Message& m : fx.history()) {
        ASSERT_FALSE(m.has_open_tool_part());
    }
}

TEST(agent_loop_keeps_the_tool_state_out_of_the_transcript_as_text) {
    /* Служебные виды частей в транскрипт не попадают: у них нет формата,
     * о котором модель знает. */
    std::vector<Message> history;
    Message m = Message::assistant("msg_0");
    m.parts.push_back(MessagePart::step_start());
    m.parts.push_back(MessagePart::reasoning("думаю"));
    m.parts.push_back(MessagePart::text("ответ"));
    m.parts.push_back(MessagePart::step_finish());
    history.push_back(m);
    const std::vector<ModelMessage> model = to_model_messages(history);
    ASSERT_EQ(model.size(), (size_t)1);
    ASSERT_EQ(model[0].content, std::string("ответ"));
}

TEST(agent_loop_respects_an_abort_before_the_first_call) {
    LoopFixture fx;
    fx.host.replies = {"этот ответ не должен быть получен"};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    engine_state().abort_requested.store(true);

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    fx.run(loop, response);

    ASSERT_EQ(fx.host.calls(), (size_t)0);
    ASSERT_TRUE(response.find("прервано пользователем") != std::string::npos);
}

/* ======================================================================
 * И5.7: разрешение на внешний путь
 * ====================================================================== */

TEST(permission_outcome_reflects_the_users_decision) {
    /* Ответ пользователя — список разрешённых путей. Отдельный флаг был бы
     * вторым местом, где живёт тот же факт, и места разъехались бы. */
    EngineState st;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.project_dir = "/srv/project";
    }
    ASSERT_TRUE((int)permission_outcome(st, "/etc/hosts") ==
                (int)PermissionOutcome::Rejected);

    HostCallbacks cb;
    PermissionGate gate(st, cb, [](AgentEvent::Kind, const std::string&) {});
    gate.allow_once("/etc/hosts");
    ASSERT_TRUE((int)permission_outcome(st, "/etc/hosts") ==
                (int)PermissionOutcome::Granted);
}

TEST(permission_reject_leaves_the_path_forbidden) {
    /* Регрессия: цикл после ожидания безусловно добавлял путь в
     * allowed_external_paths, и после ОТКАЗА повторный вызов проходил
     * уже без вопроса — отказ не означал ничего. Теперь список трогает
     * только гейт, а цикл спрашивает permission_outcome. */
    EngineState st;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.project_dir = "/srv/project";
        st.state = AgentState::Executing;
    }
    HostCallbacks cb;
    PermissionGate gate(st, cb, [](AgentEvent::Kind, const std::string&) {});
    std::string ask = gate.check("/etc/nginx/nginx.conf");
    /* check() возвращает модели предупреждение, а вопрос пользователю
     * уходит отдельным событием; состояние — единственное, что видно
     * тесту снаружи. */
    ASSERT_TRUE(ask.find("Доступ запрещён") != std::string::npos);
    ASSERT_TRUE((int)st.state == (int)AgentState::WaitingPermission);

    gate.reject();
    ASSERT_TRUE((int)permission_outcome(st, "/etc/nginx/nginx.conf") ==
                (int)PermissionOutcome::Rejected);
    /* И повторный вопрос всё ещё задаётся: доступ не открылся. */
    ASSERT_TRUE(!gate.check("/etc/nginx/nginx.conf").empty());
}

/* ======================================================================
 * И5.8: условие завершения в цикле
 * ====================================================================== */

TEST(agent_loop_does_not_finish_a_turn_that_contains_a_tool_call) {
    /* Регрессия на главный смысл И5.8. Хост про блок вызова не знает и
     * присылает finish=stop, а текст хода — длинный. Без приведения finish
     * к «tool-calls» условие выполнилось бы, и задача закрылась бы с
     * невыполненным инструментом: результата никто бы не увидел. */
    LoopFixture fx;
    fx.host.replies = {
        long_answer("Сейчас посмотрю навыки.") + "\n```json\n"
            "{\"tool\": \"list_skills\"}\n```",
        long_answer("Посмотрел, вот итог.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    ASSERT_TRUE(fx.run(loop, response));

    /* Два хода: первый с вызовом, второй с итогом. */
    ASSERT_EQ(fx.host.calls(), (size_t)2);
    const std::vector<Message> h = fx.history();
    const MessagePart* part = first_tool_part(h[1]);
    ASSERT_TRUE(part != nullptr);
    ASSERT_EQ(part->state(), ToolState::Completed);
    ASSERT_TRUE(part->output().output.find("навык") != std::string::npos);
}

TEST(agent_loop_asks_again_when_the_answer_is_too_short) {
    /* Короткий ответ без вызова — не итог. Модель получает напоминание, и
     * цикл продолжает: задача не закрывается ответом «Ок.». */
    LoopFixture fx;
    fx.host.replies = {"Ок.", long_answer("Теперь подробно.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    ASSERT_TRUE(fx.run(loop, response));
    ASSERT_EQ(fx.host.calls(), (size_t)2);

    /* Напоминание легло в историю сообщением пользователя — так его видела
     * модель и раньше. */
    const std::vector<Message> h = fx.history();
    bool reminder = false;
    for (const Message& m : h) {
        if (m.is_user() && m.text().find("[напоминание]") != std::string::npos) {
            reminder = true;
        }
    }
    ASSERT_TRUE(reminder);
    /* И оно ушло модели, а не осталось в UI-логе. */
    ASSERT_TRUE(fx.host.transcript(1).find("[напоминание]") !=
                std::string::npos);
}

TEST(agent_loop_keeps_going_when_the_user_writes_mid_turn) {
    /* Условие 3: ход отвечает на последнюю реплику пользователя. Если
     * человек написал, пока ход шёл, закрывать задачу нельзя — иначе его
     * новое сообщение останется без ответа. */
    LoopFixture fx;
    fx.host.replies = {long_answer("Первый ответ."), long_answer("Второй ответ.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    bool injected = false;
    fx.host.on_chat = [&injected] {
        if (injected) return;   /* один раз, иначе задача не закончится */
        injected = true;
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().session.push_back(Message::user("подожди, ещё вопрос"));
    };
    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    fx.run(loop, response);

    /* Ход, на который пользователь уже ответил новым сообщением, задачу не
     * закрыл: цикл сделал ещё один запрос. */
    ASSERT_TRUE(injected);
    ASSERT_EQ(fx.host.calls(), (size_t)2);
    const std::vector<Message> h = fx.history();
    size_t users = 0;
    for (const Message& m : h) {
        if (m.is_user()) ++users;
    }
    ASSERT_TRUE(users >= 2);
}

/* ======================================================================
 * И5.9: последний шаг — предупреждение, а не конец
 * ====================================================================== */

TEST(agent_loop_warns_on_the_last_step_and_finishes_normally) {
    /* max_steps = 2: на втором шаге модель должна получить напоминание и
     * ответить текстом. Раньше цикл на втором шаге просто прекращался и
     * делал отдельный запрос «прошу итоговый ответ» — то есть модель
     * получала два разных напоминания об одном и том же и последнее
     * слово оставалось за циклом, а не за ней. */
    LoopFixture fx;
    fx.host.replies = {call_block("list_skills", ""), long_answer("Итог.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare(2);

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    ASSERT_TRUE(fx.run(loop, response));

    /* Ровно два запроса: форсированного третьего не было. */
    ASSERT_EQ(fx.host.calls(), (size_t)2);
    ASSERT_TRUE(fx.host.transcript(1).find("последний шаг") != std::string::npos);
    ASSERT_TRUE(response.find("Итог.") != std::string::npos);
    /* Напоминание осталось в истории — следующая задача в той же сессии
     * не должна начинаться с него. */
    bool reminder_in_history = false;
    for (const Message& m : fx.history()) {
        if (m.is_user() && m.text().find("последний шаг") != std::string::npos) {
            reminder_in_history = true;
        }
    }
    ASSERT_TRUE(reminder_in_history);
}

TEST(agent_loop_warns_only_after_the_first_step) {
    /* На первом шаге предупреждать не о чем: шаг первый, и напоминание
     * было бы враньём, от которого модель отказалась бы работать впустую. */
    LoopFixture fx;
    fx.host.replies = {long_answer("Сделано сразу.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare(1);

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    ASSERT_TRUE(fx.run(loop, response));
    ASSERT_EQ(fx.host.calls(), (size_t)1);
    ASSERT_TRUE(fx.host.transcript(0).find("последний шаг") == std::string::npos);
}

TEST(agent_loop_forces_a_summary_when_the_last_step_still_calls_tools) {
    /* Напоминание модель может проигнорировать. Тогда задача не закрывается
     * молча: цикл делает ещё один запрос с тем же напоминанием и
     * записывает ответ в историю. */
    LoopFixture fx;
    fx.host.replies = {call_block("list_skills", ""),
                       call_block("list_skills", ""),
                       long_answer("Итог по факту.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare(2);

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    fx.run(loop, response);

    ASSERT_EQ(fx.host.calls(), (size_t)3);
    ASSERT_TRUE(fx.host.transcript(2).find("последний шаг") != std::string::npos);
    ASSERT_TRUE(response.find("Итог по факту.") != std::string::npos);
    /* И ответ лежит в истории, а не только в результате задачи: вопрос
     * «а что ты сделал?» после задачи иначе получил бы пустоту. */
    bool answered = false;
    for (const Message& m : fx.history()) {
        if (m.is_assistant() &&
            m.text().find("Итог по факту.") != std::string::npos) {
            answered = true;
        }
    }
    ASSERT_TRUE(answered);
}


/* ======================================================================
 * И7.2: цикл отдаёт измерение контекста
 * ====================================================================== */

/* ======================================================================
 * И7.10: цикл действительно сжимает
 * ======================================================================
 *
 * Проверки движка (test_engine.cpp) доказывают, что сжатие работает, когда
 * его позвали. Эта доказывает, что его зовут: мутация «цикл по-прежнему
 * вызывает старую обрезку строк» ломала ровно эту проверку и больше
 * ничего — то есть весь конвейер можно было откатить к обрезке, оставив
 * все тесты зелёными. Класс тот же, что D2: средство есть, о нём никто
 * не сказал.
 *
 * Переполнение устраивается ИЗМЕРЕНИЕМ: провайдер сообщает 15000 входных
 * токенов при окне в 1000. Оценкой это не сделать — context_usage берёт
 * измеренное, когда оно есть (И7.2), и подставленная оценка была бы
 * немедленно спрятана. */

TEST(the_loop_compacts_when_the_provider_reports_a_full_window) {
    LoopFixture fx;
    fx.host.compaction_reply = "## Цель\n- посчитать навыки";
    fx.host.prompt_tokens = 15000;
    /* Первый ход — вызов инструмента, второй — итог: цикл дойдёт до
     * конца, и проверка не будет висеть на числе шагов. */
    fx.host.replies = {call_block("repo_map", ""),
                       long_answer("Навыки: wp, python, devops.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    /* Окно: вход ограничен провайдером отдельно (input = 3000), резерв под
     * сводку равен лимиту ответа (1000), то есть usable = 2000 токенов.
     * Измеренные 15000 переполняют его сразу.
     *
     * Числа выбраны по формуле usable(), а не на глаз: без явного input
     * вычитается ПОЛНЫЙ лимит ответа, и при context = 21000 получается
     * usable = 20000, то есть переполнение не наступает никогда. */
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().model_limits.context = 100000;
        engine_state().model_limits.input = 3000;
        engine_state().model_limits.max_output = 1000;
    }

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    fx.run(loop, response);

    /* Сводщика спросили — и спросили по-портовски: с промптом сводки. */
    ASSERT_TRUE(fx.host.compaction_calls >= 1);
    /* Ровно одно сжатие: провайдер после него сообщает 500 токенов. */
    ASSERT_EQ(fx.host.compaction_calls, 1);

    /* И в истории появилась сжатая форма: сводка впереди, а последняя
     * реплика пользователя — синтетическое продолжение. Без этой проверки
     * «сводку попросили» прошло бы и при «попросили, но в историю не
     * положили». */
    const std::vector<Message> history = fx.history();
    ASSERT_TRUE(!history.empty());
    ASSERT_TRUE(history.front().parts[0].is(PartKind::Compaction));
    ASSERT_EQ(history.front().parts[0].text(),
              std::string("## Цель\n- посчитать навыки"));
    int continuations = 0;
    for (const Message& m : history) {
        for (const MessagePart& p : m.parts) {
            if (p.is(PartKind::CompactionContinue)) ++continuations;
        }
    }
    ASSERT_EQ(continuations, 1);
    /* Последняя реплика пользователя — именно продолжение: на неё
     * отвечает ход, который цикл завершил итогом. Если бы это был ответ
     * человека, условие завершения хода (И5.8) не сошлось бы. */
    const Message* last_user = last_user_message(history);
    ASSERT_TRUE(last_user != nullptr);
    ASSERT_TRUE(last_user->parts[0].is(PartKind::CompactionContinue));
}

TEST(the_loop_does_not_ask_for_a_summary_while_the_window_has_room) {
    /* Обратная сторона: сжатие не должно становиться новым источником
     * расходов. При окне 200000 токенов цикл не спрашивает сводку ни
     * разу, сколько бы шагов ни прошло. */
    LoopFixture fx;
    fx.host.compaction_reply = "## Цель\n- посчитать навыки";
    fx.host.prompt_tokens = 100;
    fx.host.replies = {call_block("repo_map", ""),
                       long_answer("Навыки: wp, python, devops.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().model_limits.context = 200000;
        engine_state().model_limits.max_output = 1000;
    }

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    fx.run(loop, response);

    ASSERT_EQ(fx.host.compaction_calls, 0);
    const std::vector<Message> history = fx.history();
    for (const Message& m : history) {
        for (const MessagePart& p : m.parts) {
            ASSERT_FALSE(p.is(PartKind::Compaction));
            ASSERT_FALSE(p.is(PartKind::CompactionContinue));
        }
    }
}

TEST(loop_records_the_input_tokens_the_host_reported) {
    /* Кто именно измеряет контекст: хост присылает usage.input = 100
     * (FakeHost), цикл обязан положить это в состояние, а не размазать
     * по трём своим счётчикам. Проверяется через эффект, а не вызовом
     * record_turn_usage напрямую: функция внутренняя, и тест, зовущий
     * её вручную, не сказал бы ничего о цикле. */
    LoopFixture fx;
    fx.host.replies = {long_answer("Навыки: wp, python, devops.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    fx.run(loop, response);

    ASSERT_EQ(engine_state().measured_input_tokens, 100LL);
    /* И измерение видно ровно таким, каким его вернёт панели: измеренным,
     * а не оценкой. */
    const compaction::ContextUsage used =
        compaction::context_usage(engine_state().measured_input_tokens,
                                  engine_state().session);
    ASSERT_TRUE(used.measured());
    ASSERT_EQ(used.tokens, 100LL);
}

TEST(loop_leaves_the_measurement_empty_when_the_host_sends_no_usage) {
    /* Хост без метрик (старый хост, провайдер без usage) — это НЕ
     * «контекст занял 0 токенов»: такая ошибка выглядела бы в панели как
     * «контекст не вырос» и надолго отключила бы сжатие. */
    LoopFixture fx;
    fx.host.replies = {long_answer("Навыки: wp, python, devops.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    /* Хост, который не присылает usage. */
    HostCallbacks quiet = cb;
    quiet.llm_chat = [&fx](const std::string& sys,
                           const std::vector<ModelMessage>& msgs,
                           LlmReply& out) {
        fx.host.replies.push_back(long_answer("Навыки: wp, python, devops."));
        LlmReply r;
        const bool ok = fx.host.chat(sys, msgs, r);
        out.content = r.content;
        out.finish_reason = r.finish_reason;
        out.prompt_tokens = 0;      /* провайдер метрики не прислал */
        out.completion_tokens = 0;
        return ok;
    };
    engine().init(quiet);

    std::string response;
    AgentLoop loop(engine_state(), quiet,
                   [&](AgentEvent::Kind, const std::string&) {});
    fx.run(loop, response);

    ASSERT_EQ(engine_state().measured_input_tokens, 0LL);
    /* Тогда панель обязана показать оценку, а не ноль. */
    const compaction::ContextUsage used =
        compaction::context_usage(engine_state().measured_input_tokens,
                                  engine_state().session);
    ASSERT_FALSE(used.measured());
    ASSERT_TRUE(used.tokens > 0);
}

/* ======================================================================
 * И10.1: снимок рабочего каталога на каждом шаге
 * ====================================================================== */

TEST(the_loop_snapshots_the_workspace_on_every_step) {
    /* Точка подключения, а не функция снимка: снимок можно снять где
     * угодно и получить зелёный набор проверок снимка, ни разу не
     * показав, что цикл его берёт. Проверяется ровно это — по одному
     * снимку на шаг, а не «хоть какой-то». */
    LoopFixture fx;
    fx.host.replies = {call_block("list_skills", ""),
                       call_block("list_skills", ""),
                       long_answer("Навыки: wp, python, devops.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    fx.run(loop, response);

    ASSERT_TRUE(engine_state().steps >= 2);
    ASSERT_EQ(engine_state().snapshots_taken, engine_state().steps);
    /* Снимок обязан быть настоящим, а не пустой структурой: иначе
     * счётчик рос бы, а откатываться было бы нечем. */
    ASSERT_TRUE(engine_state().last_snapshot.ok());
    ASSERT_TRUE(engine_state().last_snapshot.hash.size() > 0);
}

TEST(the_step_snapshot_does_not_contain_what_the_agent_wrote) {
    /* Момент снимка — начало шага, а не его результат. Проверяется
     * содержимым копии: если бы снимок брался ПОСЛЕ вызова инструмента,
     * откат вернул бы к состоянию, которое агент уже сделал, то есть
     * ничего бы не откатывал — и откат выглядел бы рабочим.
     */
    LoopFixture fx;
    fx.host.replies = {call_block("write_file",
                                  ",\n \"path\": \"новый-файл.md\""
                                  ",\n \"content\": \"привет\""),
                       long_answer("Файл записан.")};
    HostCallbacks cb = fx.callbacks();
    engine().init(cb);
    fx.prepare();

    /* Снимок первого шага снимается ДО запроса к модели, поэтому он
     * виден на первом же запросе — а к этому моменту write_file ещё не
     * отработал. Проверка содержимого — уже после прогона: каталог
     * снимка с этого момента не меняется, и «в нём нет файла» означает
     * «файл появился после снимка», а не «снимок снят позже». */
    snapshot::Snapshot first_step;
    int calls = 0;
    fx.host.on_chat = [&] {
        ++calls;
        if (calls == 1) {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            first_step = engine_state().last_snapshot;
        }
    };

    std::string response;
    AgentLoop loop(engine_state(), cb,
                   [&](AgentEvent::Kind, const std::string&) {});
    fx.run(loop, response);

    ASSERT_TRUE(fs::exists(fx.project / "новый-файл.md"));
    ASSERT_TRUE(first_step.ok());
    ASSERT_TRUE(first_step.kind == snapshot::Kind::DirCopy);
    ASSERT_FALSE(fs::exists(fs::path(first_step.dir) / "новый-файл.md"));
    /* Снимок второго шага снят уже после записи — значит состояние
     * движка по снимкам идёт, а не повторяет первый. */
    ASSERT_TRUE(engine_state().last_snapshot.ok());
    ASSERT_TRUE(engine_state().last_snapshot.hash != first_step.hash);
    ASSERT_TRUE(
        fs::exists(fs::path(engine_state().last_snapshot.dir) / "новый-файл.md"));
}

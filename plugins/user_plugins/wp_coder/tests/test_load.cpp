/*
 * test_load.cpp — И13.7: нагрузочные проверки.
 *
 * Четыре сценария из критерия готовности: 200 вызовов инструментов,
 * 10-сессийная история, компапшн под нагрузкой, отмена на каждом шаге.
 *
 * ЧТО ЭТИ ПРОВЕРКИ НЕ ДЕЛАЮТ (и это важнее, чем их число):
 *
 *   - Они не проверяют, что «ничего не упало». Проверка «200 вызовов не
 *     уронили программу» зелена на коде, который на двухсотом вызове
 *     возвращает пустую строку. Каждая проверка смотрит на РЕЗУЛЬТАТ
 *     последнего вызова и на ограничители, а не на факт завершения;
 *   - Они не меряют скорость с точностью до единицы. Время печатается и
 *     сравнивается с запасом в разы: критерий «дольше пяти минут — дефект»
 *     не про микросекунды;
 *   - Они не гоняют 200 ХОДОВ цикла. Замерено на живой машине: ход стоит
 *     0,2–0,4 с, и 200 ходов — это 40–73 с, то есть БОЛЬШЕ сторожа (30 с
 *     на проверку, tests/test_framework.h). Такая проверка не «упала бы», а
 *     убила бы весь прогон кодом 3 без внятного вердикта. Поэтому «200
 *     вызовов» — это 200 вызовов ИНСТРУМЕНТОВ, как и написано в задаче, а
 *     ходы цикла проверяются десятками (сценарий отмены);
 *   - Они НЕ передают инструментам относительный ROOT. Замерено: `root: "."`
 *     — это каталог ЗАПУСКА бинарника, а не каталог фикстуры, и нагрузочная
 *     проверка уходила грепать по 231 чужому файлу вместо двенадцати
 *     своих, упираясь в сторож. Ровно тот класс, что и петля симлинков в
 *     /tmp из прогона И13.5: вердикт о мусоре вокруг, а не о коде. Все
 *     корни здесь АБСОЛЮТНЫЕ;
 *   - Они не строят проект по 33 МБ мусора. Замерено: без git снимок шага
 *     копирует каталог целиком, и один ход на большом каталоге стоит
 *     3,9 с — нагрузка мерила бы диск, а не цикл.
 *
 * НАЙДЕНО НА ЖИВОМ МАТЕРИАЛЕ И НЕ ПОЧИНЕНО ЗДЕСЬ (журнал §4, И13.7):
 * число СООБЩЕНИЙ в истории не ограничено. `compress_history` жмёт части
 * (обрезает вывод инструментов), но сообщения не убирает, а бюджет задан
 * в символах. На 200 ходах живого цикла сессия доросла до 401 сообщения при
 * 21 тысяче символов из 60 тысяч: короткие реплики дёшевы по символам и
 * неограниченно дороги по числу запросов к модели. Это решение о том, что
 * терять (какие сообщения можно выкинуть), а не о пределе, поэтому здесь
 * оно только замерено и названо.
 */

#include "test_framework.h"
#include "test_printers.h"
#include "test_support.h"

#include "../core/agent_components.h"
#include "../core/base_tools.h"
#include "../core/compaction.h"
#include "../core/git_tools.h"
#include "../core/id_prefix.h"
#include "../core/message.h"
#include "../core/session_store.h"
#include "../core/snapshot.h"
#include "../core/tools_registry.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace coder;
namespace fs = std::filesystem;

namespace {

/* Предел с запасом: критерий «дольше пяти минут — дефект», а этот должен
 * ловить РЕЗКУЮ деградацию, а не дрожание на загруженной машине. */
constexpr long long kBudgetMs200 = 20000;

fs::path make_load_tree(const std::string& tag) {
    fs::path t = fs::temp_directory_path()
        / ("wp_coder_load_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(t);
    fs::create_directories(t / "src");
    register_tmp_tree(t.string());
    /* Маленький проект — половина смысла фикстуры: без git снимок шага
     * копирует каталог целиком (замерено: 3,9 с на ход на большом
     * каталоге), и проверка мерила бы диск вместо цикла. */
    for (int i = 0; i < 12; ++i) {
        std::ofstream f(t / "src" / ("m" + std::to_string(i) + ".php"),
                        std::ios::binary);
        f << "<?php\nfunction f" << i << "() { return " << i << "; }\n";
    }
    std::ofstream f(t / "notes.md", std::ios::binary);
    f << "заметка\n";
    return t;
}

void init_engine_for(const fs::path& project) {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&,
                         std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    cb.path_data_dir = [&project] { return project.string(); };
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    register_base_tools();
    register_git_tools();
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    engine_state().project_dir = project.string();
    engine_state().allowed_external_paths.clear();
}

std::string call_of(const std::string& tool, const std::string& args = "") {
    return "Смотрю.\n```json\n{\"tool\": \"" + tool + "\"" + args + "}\n```";
}

}  // namespace

/* ======================================================================
 * 1. 200 вызовов инструментов
 * ====================================================================== */

/*
 * Не «200 раз позвонили и не упали», а «после двухсот вызовов пайплайн
 * по-прежнему отдаёт правильный результат»: последний вызов сверяется с
 * содержимым файла. Подмена на пустую строку, обрезанный вывод или
 * сдвинутое смещение её уронят.
 *
 * Инструменты чередуются, чтобы по пайплайну прошли разные ветки: чтение,
 * поиск, обход дерева, обзор проекта.
 */
TEST(two_hundred_tool_calls_still_return_the_right_answer) {
    const fs::path t = make_load_tree("calls");
    init_engine_for(t);

    const auto started = std::chrono::steady_clock::now();
    size_t empty_outputs = 0;
    for (int i = 0; i < 200; ++i) {
        const int phase = i % 4;
        json::JsonValue a = json::JsonValue::object();
        const char* tool = "read_file";
        if (phase == 0) {
            a.set("path", json::JsonValue("notes.md"));
            tool = "read_file";
        } else if (phase == 1) {
            a.set("root", json::JsonValue(t.string()));
            a.set("pattern", json::JsonValue("function f[0-9]"));
            tool = "grep_search";
        } else if (phase == 2) {
            a.set("path", json::JsonValue(t.string()));
            a.set("pattern", json::JsonValue("*.php"));
            tool = "glob";
        } else {
            a.set("root", json::JsonValue(t.string()));
            tool = "repo_map";
        }
        /* Кэш задачи у repo_map сбрасывается: иначе 49 вызовов из 50
         * попали бы в кэш первого и измеряли бы не обход, а сравнение
         * строк. Кэш — законное поведение, но нагрузка меряет инструмент. */
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().repo_map_cache_root.clear();
            engine_state().repo_map_cache_result.clear();
        }
        const ToolOutput o =
            ToolsRegistry::instance().run_output(tool, a);
        if (o.output.empty()) ++empty_outputs;
    }
    const long long spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();

    if (empty_outputs > 0) {
        std::cerr << "  из 200 вызовов пустой вывод у " << empty_outputs
                  << std::endl;
    }
    ASSERT_EQ(empty_outputs, size_t(0));

    /* Последний вызов — чтение notes.md: сверяем СОДЕРЖИМОЕ, а не то, что
     * вызов вернулся. */
    json::JsonValue a = json::JsonValue::object();
    a.set("path", json::JsonValue("notes.md"));
    const ToolOutput last = ToolsRegistry::instance().run_output("read_file", a);
    ASSERT_TRUE(last.output.find("заметка") != std::string::npos);

    std::cerr << "  200 вызовов инструментов: " << spent << " мс" << std::endl;
    if (spent > kBudgetMs200) {
        std::cerr << "  дольше " << kBudgetMs200
                  << " мс — по критерию нагрузки это дефект" << std::endl;
    }
    ASSERT_TRUE(spent <= kBudgetMs200);
}

/* ======================================================================
 * 2. 10-сессийная история
 * ====================================================================== */

/*
 * Десять сессий подряд через настоящий файл: запись, чтение, правило
 * resume («самый свежий файл»). Проверяется содержимое каждой, а не то,
 * что save вернул true: круговое «сохранили — прочитали то, что сами же
 * и положили» не увидит, что файл сессии двух соседних прогонов
 * перетирает друг друга.
 *
 * Идентификаторы соседние и по времени, и по номеру — именно тот случай,
 * ради которого `current_file` развязывает их по номеру (точность времени
 * файла бывает в секунду, а ses_…009 не «свежее» ses_…010 по строкам).
 */
TEST(ten_sessions_round_trip_through_the_store_and_resume_picks_the_newest) {
    const fs::path data = fs::temp_directory_path()
        / ("wp_coder_load_ses_" + std::to_string(::getpid()));
    fs::remove_all(data);
    fs::create_directories(data);
    register_tmp_tree(data.string());

    const auto started = std::chrono::steady_clock::now();
    std::vector<std::string> ids;
    IdSequence ids_seq;
    for (int n = 1; n <= 10; ++n) {
        /* Идентификатор — НАСТОЯЩИМ генератором (IdSequence), а не
         * собранным руками: `is_session_id`/`id_number` сверяют длину
         * хвоста с kIdDigits, и ручная сборка «ses_%09d» дала бы не
         * идентификатор, а имя — развязка по номеру молча выключилась бы,
         * и current_file вернул бы произвольную сессию. Первая версия
         * проверки на этом и падала. */
        const std::string id = ids_seq.next_session();
        ids.push_back(id);

        SessionFile s;
        s.session_id = id;
        /* Разный объём истории: иначе проверка была бы зелёной на коде,
         * который хранит только последнее сообщение. */
        for (int k = 0; k <= n; ++k) {
            s.messages.push_back(Message::user("реплика " + std::to_string(k)
                                               + " сессии " + std::to_string(n)));
        }
        s.messages.push_back(Message::assistant("ответ сессии " + std::to_string(n)));

        std::string err;
        if (!SessionArchive::save(SessionArchive::file_path(data.string(), id),
                                 s, &err)) {
            std::cerr << "  сессия " << id << " не записана: " << err << std::endl;
            ASSERT_TRUE(false);
        }
    }

    for (size_t i = 0; i < ids.size(); ++i) {
        SessionFile back;
        std::string err;
        std::vector<std::string> warnings;
        if (!SessionArchive::load(
                SessionArchive::file_path(data.string(), ids[i]), back, &err,
                &warnings)) {
            std::cerr << "  сессия " << ids[i] << " не прочитана: " << err
                      << std::endl;
            ASSERT_TRUE(false);
            continue;
        }
        ASSERT_EQ(back.session_id, ids[i]);
        /* Сообщений на одно больше, чем реплик: n+1 реплик и ответ. */
        if (back.messages.size() != i + 3) {
            std::cerr << "  сессия " << ids[i] << ": сообщений "
                      << back.messages.size() << ", ожидалось " << (i + 3)
                      << std::endl;
        }
        ASSERT_EQ(back.messages.size(), i + 3);
    }

    /* Правило resume: открыться должна ПОСЛЕДНЯЯ сессия, а не любая. */
    const std::string current = SessionArchive::current_file(data.string());
    ASSERT_EQ(current,
              SessionArchive::file_path(data.string(), ids.back()));
    const long long spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    std::cerr << "  10 сессий через диск: " << spent << " мс" << std::endl;
    ASSERT_TRUE(spent <= kBudgetMs200);
}

/* ======================================================================
 * 3. Компакшн под нагрузкой
 * ====================================================================== */

/*
 * Тяжёлая история ПЕРЕПОЛНЯЕТ окно, и цикл обязан дойти до сводки. Проверяется
 * не «сводка позвана», а три вещи, которые ломаются по отдельности: история
 * уменьшилась, ЦЕЛЬ задачи дошла до модели и ПОСЛЕДНИЙ обсуждаемый ход уцелел.
 *
 * Про цель важно не перепутать: по замыслу compaction ЗАМЕНЯЕТ голову истории
 * сводкой, а не сохраняет первое сообщение (core/compaction.cpp,
 * compacted_history). Поэтому первое сообщение после сжатия — это обёртка
 * сводки, и проверять надо не «сообщение «ЗАДАЧА» осталось», а «модель по-
 * прежнему знает, зачем она здесь». Первая версия проверки требовала первое
 * и падала на коде, который устроен правильно.
 */
TEST(compaction_of_a_loaded_history_shrinks_it_and_keeps_the_goal_and_the_tail) {
    const fs::path t = make_load_tree("compact");
    init_engine_for(t);

    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().session.clear();
        engine_state().session.push_back(Message::user("ЗАДАЧА: починить парсер"));
        for (int i = 0; i < 60; ++i) {
            engine_state().session.push_back(
                Message::user("ход " + std::to_string(i) + ": разбираю исходник, "
                              "строк очень много, вот они: " +
                              std::string(400, 'x')));
        }
        /* Message::assistant() принимает parent_id, а НЕ текст: подпись
         * параметра выглядит как взаимозаменяемая с user(text), но
         * assistant("текст") молча создаёт пустое сообщение с этим
         * текстом в parent_id. Текст кладётся частью, как в остальных
         * проверках. */
        Message tail_turn = Message::assistant("msg_user");
        tail_turn.parts.push_back(MessagePart::text("текущий ход обсуждается"));
        engine_state().session.push_back(tail_turn);
        engine_state().session_budget = 60000;
        engine_state().model_limits = compaction::ModelLimits();
        engine_state().model_limits.context = 4000;
        engine_state().model_limits.max_output = 1000;
        engine_state().measured_input_tokens = 3800;
        engine_state().abort_requested.store(false);
    }

    const size_t before_size = engine_state().session.size();
    const size_t before_chars = model_history_chars(engine_state().session);

    /* Свёртка: отдаём короткую сводку и запоминаем, что её звали. */
    std::atomic<int> turns{0};
    const compaction::SummaryTurn turn =
        [&](const std::string&, const std::vector<ModelMessage>&,
            std::string& text, std::string&) {
            ++turns;
            text = "## Цель\n- починить парсер";
            return true;
        };
    engine().compact_history_if_needed(turn);

    const size_t after_size = engine_state().session.size();
    const size_t after_chars = model_history_chars(engine_state().session);

    std::cerr << "  история под компакшном: " << before_size << " сообщений / "
              << before_chars << " символов → " << after_size << " / "
              << after_chars << std::endl;

    if (turns.load() == 0) {
        std::cerr << "  сводку не звали: переполнение не наступило" << std::endl;
    }
    ASSERT_TRUE(turns.load() > 0);
    if (after_size >= before_size) {
        std::cerr << "  история не уменьшилась: " << before_size << " → "
                  << after_size << std::endl;
    }
    ASSERT_TRUE(after_size < before_size);

    /* ЦЕЛЬ: после сжатия голова — это сводка, и модель должна по-прежнему
     * знать, что делает. Проверка ищет цель ВО ВСЕЙ истории, а не в первом
     * сообщении: место, где она живёт, — следствие замысла сжатия, а
     * «знает ли модель» — это и есть инвариант. */
    std::string whole;
    for (const Message& m : engine_state().session)
        for (const MessagePart& p : m.parts) whole += p.text() + "\n";
    if (whole.find("починить парсер") == std::string::npos) {
        std::cerr << "  цель задачи не дошла до модели после сжатия: ["
                  << whole.substr(0, 300) << "]" << std::endl;
    }
    ASSERT_TRUE(whole.find("починить парсер") != std::string::npos);

    /* Последний обсуждаемый ход: стирать его нельзя. Искать надо ПО ВСЕЙ
     * истории, а не в последнем сообщении: по замыслу (core/compaction.cpp,
     * compacted_history) собранное после сжатия — это [обёртка сводки,
     * хвост, реплика-продолжение], то есть последним лежит автопродолжение,
     * а не то, что обсуждалось. Первая версия проверки смотрела на back()
     * и падала на коде, который устроен правильно. */
    if (whole.find("текущий ход обсуждается") == std::string::npos) {
        std::cerr << "  последний обсуждаемый ход не уцелел после сжатия"
                  << std::endl;
    }
    ASSERT_TRUE(whole.find("текущий ход обсуждается") != std::string::npos);
}

/* ======================================================================
 * 4. Отмена на каждом шаге
 * ====================================================================== */

/*
 * Отмена на КАЖДОМ шаге по очереди: первый, второй, третий… Каждый раз —
 * свежее состояние, отмена приходит изнутри хода (хост ставит фlag перед
 * N-м запросом к модели), и цикл обязан вернуться, сказать «прервано
 * пользователем» и НЕ дойти до следующего шага.
 *
 * Проверка «отмена на первом шаге» уже есть (agent_loop_respects_an_abort_
 * before_the_first_call). Здесь важно другое: отмена ПОСЛЕ выполненного
 * шага — там, где цикл держит состояние между шагами (счётчики, история,
 * снимок) и где отмена легко теряется. Плюс бюджет времени: отмена, которая
 * возвращает управление через полминуты, — это зависание, а не отмена.
 *
 * И ещё одно, что видно только в цикле: отмена не должна оставлять вопрос
 * разрешения висеть навечно (`pending_`), иначе панель показала бы вопрос,
 * на который уже никто не придёт.
 */
TEST(cancelling_at_every_step_returns_the_loop_promptly_and_cleanly) {
    const fs::path t = make_load_tree("cancel");
    init_engine_for(t);

    const int kSteps = 6;
    for (int target = 1; target <= kSteps; ++target) {
        HostCallbacks cb;
        std::atomic<int> chats{0};
        cb.llm_chat = [&chats, target, &t](const std::string&,
                                       const std::vector<ModelMessage>&,
                                       LlmReply& out) {
            const int n = ++chats;
            /* Отмена приходит перед N-м запросом к модели. */
            if (n == target) engine_state().abort_requested.store(true);
            /* Каждый ответ — ВЫЗОВ инструмента, а не итог задачи. Итог на
             * чётном шаге обрывал бы ход раньше, чем цикл дошёл бы до
             * границы следующего шага, где и стоит проверка отмены, — и
             * отмена на чётных шагах не проверялась бы вовсе, а прошла бы
             * на коде, который отмену игнорирует. Инструменты чередуются,
             * иначе три одинаковых вызова подряд ловят детектор
             * зацикливания, и проверка измеряла бы его, а не отмену. */
            out.content = (n % 2 == 1)
                              ? call_of("list_skills")
                              : call_of("repo_map",
                                        ", \"root\": \"" + t.string() + "\"");
            out.finish_reason = "stop";
            out.prompt_tokens = 100;
            out.completion_tokens = 20;
            return true;
        };
        cb.llm_is_connected = []() { return true; };
        cb.chat_event = [](const std::string&) {};
        cb.path_data_dir = [&t] { return t.string(); };
        engine().init(cb);
        test_support::approve_all_permissions();
        register_base_tools();

        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().session.clear();
            engine_state().session.push_back(Message::user("посчитай навыки"));
            engine_state().project_dir = t.string();
            engine_state().mode = 0;
            engine_state().plan_mode = false;
            engine_state().use_planning = false;
            engine_state().max_steps = 12;
            engine_state().session_budget = 60000;
            engine_state().todos.clear();
            engine_state().recent_calls.clear();
            engine_state().allowed_external_paths.clear();
            engine_state().measured_input_tokens = 0;
            engine_state().model_limits = compaction::ModelLimits();
            engine_state().compaction_config = compaction::CompactionConfig();
            engine_state().abort_requested.store(false);
            engine_state().shutting_down = false;
            engine_state().state = AgentState::Executing;
            engine_state().steps = 0;
            engine_state().last_snapshot = snapshot::Snapshot();
            engine_state().snapshots_taken = 0;
        }

        const auto started = std::chrono::steady_clock::now();
        std::string response;
        AgentLoop loop(engine_state(), cb,
                       [](AgentEvent::Kind, const std::string&) {});
        loop.run("системный промпт", response);
        const long long spent =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();

        if (response.find("прервано пользователем") == std::string::npos) {
            std::cerr << "  отмена на шаге " << target
                      << ": цикл не сказал «прервано пользователем», ответ ["
                      << response.substr(0, 160) << "]" << std::endl;
        }
        ASSERT_TRUE(response.find("прервано пользователем") != std::string::npos);
        std::cerr << "  отмена на шаге " << target << ": " << spent << " мс"
                  << std::endl;
        if (spent > 5000) {
            std::cerr << "  это зависание, а не отмена" << std::endl;
        }
        ASSERT_TRUE(spent <= 5000);
        /* После отмены вопрос разрешения не должен висеть. */
        ASSERT_EQ(engine().permissions().pending_count(), size_t(0));
    }
}
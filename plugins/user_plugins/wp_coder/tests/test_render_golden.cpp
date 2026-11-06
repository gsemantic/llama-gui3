/*
 * test_render_golden.cpp — И11.13: снапшоты отрисовки (golden) и отсутствие
 * дедлока при активном агенте.
 *
 * ---------------------------------------------------------------------------
 * ЧТО ТАКОЕ «СНАПШОТ ОТРИСОВКИ» ЗДЕСЬ И ЧЕГО ОН НЕ МОЖЕТ
 * ---------------------------------------------------------------------------
 *
 * ImGui тестам НЕ ПОДКЛЮЧЕНА: в `plugins/user_plugins/wp_coder/CMakeLists.txt`
 * у `wp_coder_tests` нет ни include-пути `external/imgui`, ни линковки
 * `imgui_lib`, и прецедента offscreen-контекста (`ImGui::CreateContext`
 * без бэкенда) в репозитории нет ни одного — единственный вызов стоит в
 * `src/ui/main_window.cpp`. Заводить рендерер ради двух панелей — это новая
 * зависимость и новая поверхность отказа, а правило запрещает расширять
 * зависимости.
 *
 * Поэтому снимок снимается с ТОГО ЖЕ кода, которым рисует окно, — со
 * строковых функций core. Это ровно тот приём, который И11.3 уже назвал
 * в заготовке golden-проверки для diff (tests/test_diff_view.cpp:621-622:
 * «Именно её 11.13 будет сверять как golden-снапшот»).
 *
 * ЧЕГО ТАКОЙ СНИМОК НЕ ВИДИТ, И ЭТО НАЗВАНО, А НЕ ПРОПУЩЕНО:
 *   - раскладку и геометрию ImGui (позиции, шрифт, перенос по краю окна);
 *   - цвет как ВИДИМЫЙ пиксель — но числовой цвет текста и полосы в снимок
 *     входит, потому что решение о нём принимает core, а не окно;
 *   - что окно нарисовало именно эту строку, а не другую (это отдельная
 *     граница «окно рисует, но не решает»; её проверяют сканированием
 *     исходника — test_window_locks.cpp, test_permission_panel.cpp).
 * Иными словами: снимок ловит СОДЕРЖАНИЕ и ВИД решения, а не раскладку.
 *
 * ---------------------------------------------------------------------------
 * ПОЧЕМУ В СНИМОК ВХОДИТ ЦВЕТ
 * ---------------------------------------------------------------------------
 *
 * Виджет diff различает замену от удаления и рисует полосу «до/после»
 * ПОД строкой — решение о виде принимает `core/diff.cpp`, а окно только
 * переводит четыре числа в `ImVec4`. Снимок только текста пропустил бы
 * правку цвета целиком: полоса стала бы другой, а эталон остался бы
 * зелёным, потому что текст не изменился. Поэтому строка снимка — это
 * `роль | цвет текста | непрозрачность полосы | строка показа`.
 *
 * ---------------------------------------------------------------------------
 * ЧТО ЛОВИТ СНИМОК ТАЙМЛАЙНА, ЧЕГО НЕ ЛОВИЛИ ПРОВЕРКИ ПО ОДНОЙ СТРОКЕ
 * ---------------------------------------------------------------------------
 *
 * До И11.13 подписи таймлайна сверялись ПО ОДНОЙ (`turn_label`, `call_label`,
 * `details_line` — tests/test_timeline.cpp). Это ловит правку текста, но не
 * ловит: ПОРЯДОК блоков, повтор строки, пропавшую строку целиком и
 * перестановку ходов. Снимок склеивает панель целиком в том порядке, в
 * котором её рисует окно, — и расходится с эталоном на любом из этих
 * случаев, называя переставленную строку, а не «число не то».
 *
 * ---------------------------------------------------------------------------
 * ВРЕМЯ В ЭТАЛОНЕ ПОДСТАВЛЕНО, А НЕ ИЗМЕРЕНО
 * ---------------------------------------------------------------------------
 *
 * Эталон со «0.5 с» из настоящих двухсот миллисекунд был бы нестабилен и
 * проверял бы скорость машины. Метки времени подставляются напрямую
 * (`restore_duration_ms`), как и в tests/test_timeline.cpp; измерение
 * времени проверяется отдельно (tests/test_message.cpp) и здесь не
 * подменяется — тот же довод, что записан в шапке того файла.
 */

#include "core/agent_components.h"
#include "core/compaction.h"
#include "core/diff.h"
#include "core/engine.h"
#include "core/timeline.h"
#include "test_framework.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace coder;

namespace {

/* ======================================================================
 * Сборка строки показа diff — те же четыре шага и в том же порядке, что
 * зовёт `render_diff_rows` в окне: разобрать, срезать отступ, снять вид,
 * напечатать. Именно эта склейка ломается первой при ошибке в любом шаге.
 * ====================================================================== */
std::string picture_of(const diff::FileDiff& fd) {
    std::vector<diff::DiffRow> rows = diff::parse_unified(fd.patch);
    diff::trim_diff(rows);
    const diff::DiffGutter gutter = diff::diff_number_gutter(rows);
    const std::vector<diff::DiffChangeBlock> blocks =
        diff::diff_change_blocks(rows);

    std::string out;
    size_t block = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        /* Подпись блока рисуется один раз — на первой строке блока, и в
         * снимок она попадает на своё место, а не «в конце блока». */
        if (block < blocks.size() && blocks[block].first == i) {
            if (!out.empty()) out += "\n";
            out += "[" + diff::diff_change_label(blocks[block]) + "]";
            ++block;
        }
        if (!out.empty()) out += "\n";
        const diff::DiffColor text = diff::diff_role_color(rows[i].role);
        const diff::DiffChangeKind kind =
            diff::diff_row_change(blocks, i);
        const diff::DiffColor band = diff::diff_change_color(kind, rows[i].role);
        char head[96];
        std::snprintf(head, sizeof(head), "%d %03d/%03d ",
                      static_cast<int>(rows[i].role),
                      static_cast<int>(text.r * 100.0f),
                      static_cast<int>(band.a * 100.0f));
        out += head;
        out += diff::format_diff_row(rows[i], gutter);
    }
    return out;
}

/* ======================================================================
 * Сборка панели таймлайна — в том порядке, в каком её рисует окно:
 * заголовок, подпись режима, затем ходы, внутри хода — вызовы.
 * ====================================================================== */
std::string timeline_picture(const timeline::Timeline& tl,
                             timeline::Details details) {
    std::string out = "head: " + timeline::timeline_head(tl) + "\n";
    out += "mode: " + timeline::details_line(tl, details);
    for (const timeline::Turn& turn : tl.turns) {
        out += "\nturn: " + timeline::turn_label(turn);
        for (const timeline::Call& c : turn.calls) {
            const timeline::CallShow show = timeline::call_show(c, details);
            out += "\n  call: " + timeline::call_label(c);
            out += (show == timeline::CallShow::Row) ? " [row]"
                   : (show == timeline::CallShow::Body) ? " [body]"
                                                        : " [branch]";
        }
    }
    return out;
}

/* --- Фикстура таймлайна (та же, что у tests/test_timeline.cpp) --- */
std::string msg_id(int n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "msg_0000000000%02d", n);
    return buf;
}

Message user_message(const char* id, const std::string& text) {
    Message m = Message::user(text);
    m.id = id;
    return m;
}

MessagePart call_part(const char* call_id, const char* tool, int ms,
                      bool ok) {
    MessagePart p = MessagePart::tool(call_id, tool);
    p.set_running();
    MessagePart done = ok ? p.set_result(ToolOutput())
                          : p.set_error("сбой инструмента");
    /* Время ПОДСТАВЛЕНО: эталон обязан быть стабилен, а измерение
     * проверяется отдельно (шапка файла). */
    done.restore_duration_ms(ms);
    return done;
}

Message assistant_message(const char* id, const char* parent_id,
                          std::vector<MessagePart> parts) {
    Message m = Message::assistant(parent_id);
    m.id = id;
    for (MessagePart& p : parts) m.parts.push_back(std::move(p));
    return m;
}

}  // namespace

/* ======================================================================
 * 1. Снапшот отрисовки diff
 * ====================================================================== */

TEST(golden_diff_замена_одной_строки) {
    /* Замена — самый частый вид правки и самый наглядный: у неё и полоса
     * «до/после» сильнее, чем у вставки и удаления, и вид блока свой. */
    const diff::FileDiff fd = diff::make_file_diff(
        "src/a.txt", "alpha\nbravo\ncharlie\n", "alpha\nBRAVO\ncharlie\n");
    /* Значения сняты с ЖИВОГО вывода и сверены с `core/diff.cpp`:
     * Header 0.45→045, Context 0.80→080, Removed 0.90→090, Added
     * 0.42→042; полоса 0.30 у замены и 0.18 у вставки/удаления — «замена
     * читается как одно изменение, у вставки второй стороны нет». Эталон,
     * выдуманный вместо снятого, проверял бы не код, а моё представление
     * о нём — и падал бы ровно на правильном поведении. */
    ASSERT_EQ(picture_of(fd), std::string(
        "0 045/000           @@ -1,3 +1,3 @@\n"
        "2 080/000   1   1   alpha\n"
        "[замена 1 → 1]\n"
        "4 090/030   2     - bravo\n"
        "3 042/030       2 + BRAVO\n"
        "2 080/000   3   3   charlie"));
}

TEST(golden_diff_чистая_вставка) {
    /* Вставка — блок своего вида и полоса слабее, чем у замены: человек
     * не должен читать «дописанное» как «заменённое». */
    const diff::FileDiff fd = diff::make_file_diff(
        "src/a.txt", "alpha\ncharlie\n", "alpha\nbravo\ndelta\ncharlie\n");
    ASSERT_EQ(picture_of(fd), std::string(
        "0 045/000           @@ -1,2 +1,4 @@\n"
        "2 080/000   1   1   alpha\n"
        "[добавлено 2]\n"
        "3 042/018       2 + bravo\n"
        "3 042/018       3 + delta\n"
        "2 080/000   2   4   charlie"));
}

TEST(golden_diff_чистое_удаление) {
    /* Удаление — зеркало вставки: полоса по другой стороне строки. */
    const diff::FileDiff fd = diff::make_file_diff(
        "src/a.txt", "alpha\nbravo\ndelta\ncharlie\n", "alpha\ncharlie\n");
    ASSERT_EQ(picture_of(fd), std::string(
        "0 045/000           @@ -1,4 +1,2 @@\n"
        "2 080/000   1   1   alpha\n"
        "[удалено 2]\n"
        "4 090/018   2     - bravo\n"
        "4 090/018   3     - delta\n"
        "2 080/000   4   2   charlie"));
}

TEST(golden_diff_два_хука_не_сливаются_в_один_блок) {
    /* Два изменения в разных местах файла — самый частый реальный патч.
     * Снимок фиксирует, что между ними стоит `@@`: слипшиеся блоки
     * выглядели бы как один правый блок и человек прочитал бы две правки
     * как одну. */
    const diff::FileDiff fd = diff::make_file_diff(
        "src/a.txt", "a\nb\nc\nd\ne\nf\ng\nh\n", "A\nb\nc\nd\ne\nf\ng\nH\n");
    const std::string pic = picture_of(fd);

    /* Хуков ровно ДВА. Считаются строки заголовка (роль Header, номер 0),
     * а не вхождения «@@»: в заголовке их ПАРА (`@@ -1,4 +1,4 @@`), и счёт
     * по вхождениям дал бы четыре на двух хуках — то есть проверка прошла
     * бы на неверном числе и проверяла бы не то. */
    size_t hunks = 0;
    for (std::size_t i = 0; i < pic.size(); ++i) {
        if (pic[i] == '0' && pic.compare(i, 6, "0 045/") == 0) ++hunks;
    }
    ASSERT_EQ(hunks, size_t(2));
    /* Блоков подписи ровно ДВА — по одному на хук. */
    size_t labels = 0;
    for (std::size_t i = 0; i < pic.size(); ++i) {
        if (pic[i] == '[') ++labels;
    }
    ASSERT_EQ(labels, size_t(2));
    ASSERT_TRUE(pic.find("A") != std::string::npos);
    ASSERT_TRUE(pic.find("H") != std::string::npos);
}

TEST(golden_diff_пустой_патч_даёт_пустую_картинку_а_не_мусор) {
    /* Файл не изменился — патча нет. Виджет не должен рисовать ни одной
     * строки: снимок пуст, и пустая картинка не выглядит «сломанной
     * разметкой». */
    const diff::FileDiff fd =
        diff::make_file_diff("src/a.txt", "alpha\n", "alpha\n");
    ASSERT_EQ(picture_of(fd), std::string());
}

/* ======================================================================
 * 2. Снапшот таймлайна
 * ====================================================================== */

namespace {

/* Один ход с успешным и ошибочным вызовами — материал обоих снимков. */
std::vector<Message> turn_with_mixed_calls() {
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "прочитай конфиг"));
    h.push_back(assistant_message(
        "msg_000000000002", "msg_000000000001",
        {call_part("call_0", "read_file", 120, true),
         call_part("call_1", "grep_search", 340, false)}));
    return h;
}

}  // namespace

TEST(golden_таймлайн_целиком_при_всех_деталях) {
    /* Полная склейка панели в режиме по умолчанию. Именно её человек видит
     * обычно, и именно она ломается при перестановке ходов, при пропавшем
     * вызове и при смене порядка внутри хода.
     *
     * Подпись хода заканчивается ТЕКСТОМ РЕПЛИКИ ЧЕЛОВЕКА, а не ответом
     * агента: `turn_label` берёт `reply_to`, то есть то, на что ход
     * ОТВЕЧАЕТ, — иначе подпись повторяла бы первую строку ответа, которая
     * и так нарисована панелью стриминга (11.6). */
    const timeline::Timeline tl = timeline::build_timeline(
        timeline::timeline_source(turn_with_mixed_calls(),
                                  limits::kMaxTimelineTurns));
    ASSERT_EQ(timeline_picture(tl, timeline::Details::All), std::string(
        "head: Таймлайн: ходов 1, вызовов 2, время вызовов: 460 мс\n"
        "mode: детали: тела всех вызовов"
        "\nturn: Ход 1: с ошибкой, вызовов: 2, время вызовов: 460 мс — "
        "прочитай конфиг"
        "\n  call: read_file: готов, 120 мс [branch]"
        "\n  call: grep_search: ошибка, 340 мс [branch]"));
}

TEST(golden_таймлайн_в_режиме_только_проблемы_меняет_вид_а_не_порядок) {
    /* Второй режим: успешный вызов схлопывается в одну строку, ошибочный
     * остаётся телом. Порядок при этом МЕНЯТЬСЯ НЕ ДОЛЖЕН, и снимок
     * требует и перестроения `branch`→`row`/`body`, и неизменного текста.
     *
     * В режиме «все детали» ЛЮБОЙ вызов рисуется веткой — решение
     * `call_show` отдаёт `Branch` на весь режим целиком, а не по статусу.
     * Снимок фиксирует и это: иначе «ошибочный всегда телом» осталось бы
     * правдой только по памяти. */
    const timeline::Timeline tl = timeline::build_timeline(
        timeline::timeline_source(turn_with_mixed_calls(),
                                  limits::kMaxTimelineTurns));
    ASSERT_EQ(timeline_picture(tl, timeline::Details::OnlyProblems), std::string(
        "head: Таймлайн: ходов 1, вызовов 2, время вызовов: 460 мс\n"
        "mode: детали: тела только неуспешных вызовов; успешных скрыто: 1"
        "\nturn: Ход 1: с ошибкой, вызовов: 2, время вызовов: 460 мс — "
        "прочитай конфиг"
        "\n  call: read_file: готов, 120 мс [row]"
        "\n  call: grep_search: ошибка, 340 мс [body]"));
}

TEST(golden_таймлайн_различает_ходы_по_порядку) {
    /* Два хода с РАЗНЫМИ вызовами: снимок обязан различать их по порядку.
     * Проверки по одной строке такой случай пропустили бы — обе подписи
     * остались бы «теми же самыми», если бы ходы переставили.
     *
     * Ходы взяты С ВЫЗОВАМИ намеренно: `turn_label` для хода без вызовов
     * возвращается РАНЬШЕ текста реплики («Ход N: только текст, вызовов
     * нет»), и два таких хода дали бы две одинаковые строки — снимок
     * различал бы их только по номеру, а перестановка осталась бы незаметной. */
    std::vector<Message> h;
    h.push_back(user_message("msg_000000000001", "первая задача"));
    h.push_back(assistant_message(
        "msg_000000000002", "msg_000000000001",
        {call_part("call_0", "read_file", 120, true)}));
    h.push_back(user_message("msg_000000000003", "вторая задача"));
    h.push_back(assistant_message(
        "msg_000000000004", "msg_000000000003",
        {call_part("call_1", "bash", 2500, true)}));

    const timeline::Timeline tl = timeline::build_timeline(
        timeline::timeline_source(h, limits::kMaxTimelineTurns));
    ASSERT_EQ(timeline_picture(tl, timeline::Details::All), std::string(
        "head: Таймлайн: ходов 2, вызовов 2, время вызовов: 2.6 с\n"
        "mode: детали: тела всех вызовов"
        "\nturn: Ход 1: готов, вызовов: 1, время вызовов: 120 мс — первая задача"
        "\n  call: read_file: готов, 120 мс [branch]"
        "\nturn: Ход 2: готов, вызовов: 1, время вызовов: 2.5 с — вторая задача"
        "\n  call: bash: готов, 2.5 с [branch]"));
}

/* ======================================================================
 * 3. Отсутствие дедлока при активном агенте
 *
 * Модель — `history_compression_does_not_block_ui_reader`
 * (tests/test_engine.cpp): «UI-поток» берёт `state_.mtx` каждый кадр, а
 * рядом работает агент; если агент держит лок, UI застревает.
 *
 * Агент здесь — НАСТОЯЩИЙ цикл (`AgentLoop::run` в своём потоке на
 * живом состоянии движка), а не имитация: имитация не брала бы тот самый
 * лок, и проверка прошла бы на коде, который вешает настоящий агент.
 * Разметка — как у `stop_during_a_turn_breaks_the_request_and_not_just_
 * the_wait` (tests/test_agent_loop.cpp): хост, на котором запрос начался,
 * но ответа не будет.
 *
 * ПОЧЕМУ НЕ `engine().start()`. Перезапустить worker движка нельзя: `Engine::stop()`
 * выставляет `shutting_down` навсегда, и второй `start()` поднимает поток,
 * который сразу выходит. Проверка на этом падала бы не из-за дедлока, а
 * из-за того, что агент не работал вовсе, — то есть проверяла бы пустоту.
 * Цикл в своём потоке даёт то же самое без общего ресурса на весь процесс.
 * ====================================================================== */

namespace {

/* Хост, на котором ход ВИСИТ: запрос начат, ответа не будет. */
struct HangingHost {
    std::atomic<bool> entered{false};
    std::atomic<int> cancel_calls{0};
};

HostCallbacks hanging_callbacks(HangingHost& host) {
    HostCallbacks cb;
    HangingHost* h = &host;
    cb.llm_chat_stream =
        [h](const std::string&, const std::vector<ModelMessage>&,
            const std::string&, std::function<void(void*)> on_started,
            std::function<void(const char*, int)>,
            std::function<void(const char*, const char*, const char*)>,
            std::function<void(const std::string&)>) -> bool {
            static int handle = 1;
            if (on_started) on_started(&handle);
            h->entered.store(true);
            return true;   /* on_done не будет — ход висит до отмены */
        };
    cb.llm_chat_cancel = [h](void*) { h->cancel_calls.fetch_add(1); };
    cb.llm_is_connected = []() { return true; };
    cb.settings_set = [](const std::string&, const std::string&) {};
    cb.settings_get = [](const std::string&, const std::string& d) { return d; };
    cb.chat_event = [](const std::string&) {};
    return cb;
}

/* Держатель цикла агента.
 *
 * Деструктор ОБЯЗАТЕЛЬНО останавливает и джойнит поток: `std::thread`,
 * уничтоженный в присоединённом состоянии, зовёт `std::terminate` и роняет
 * ВЕСЬ прогон. Поэтому даже когда проверка упала на `ASSERT` и дошла до
 * `throw`, поток агента остановлен — иначе падение одной проверки убивало
 * бы все остальные (тот же довод, что у `Joiner` в test_permission_engine.cpp
 * и у `worker.detach()` в test_engine.cpp). */
struct RunningAgent {
    HangingHost host;
    HostCallbacks cb;
    std::string response;   /* живёт вместе с потоком, см. ниже */
    std::thread runner;

    RunningAgent() : cb(hanging_callbacks(host)) {
        engine().init(cb);
        {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().session.clear();
            engine_state().session.push_back(Message::user("работаю"));
            engine_state().abort_requested.store(false);
            /* СВОЙ токен отмены на ход — ровно как делает `Engine::submit`
             * (engine.cpp). Без него цикл берёт `state_.turn_abort`, а он
             * после ПРОШЛОГО теста уже отменён: `LlmClient::fetch` проверяет
             * токен ДО зова и возвращается, не позвав хоста вовсе. Проверка
             * падала бы «агент не начал ход», а читалось бы как «агент завис»
             * — то есть проверяла бы не дедлок, а чужое состояние синглона. */
            engine_state().turn_abort = std::make_shared<AbortToken>();
            engine_state().state = AgentState::Executing;
            engine_state().mode = 0;
            engine_state().plan_mode = false;
            engine_state().use_planning = false;
            engine_state().max_steps = 3;
            engine_state().steps = 0;
            engine_state().todos.clear();
            engine_state().measured_input_tokens = 0;
            engine_state().model_limits = compaction::ModelLimits();
            engine_state().compaction_config = compaction::CompactionConfig();
        }
        AgentLoop* loop = new AgentLoop(engine_state(), cb,
                                        [](AgentEvent::Kind, const std::string&) {});
        /* `response` — ЧЛЕН структуры, а не локальная переменная
         * конструктора. Локал, захваченный в поток ПО ССЫЛКЕ, умирает
         * вместе с конструктором, а поток живёт дольше: цикл писал бы в
         * освобождённый `std::string`. На `-O0` это проходит — освобождённый
         * кадр ещё цел, — а на `-O2` его немедленно занимает кто-то другой,
         * и `append` падает с `std::bad_alloc`. Нашлось контрольной
         * пересборкой (правило 3), то есть ровно тем, для чего она и есть:
         * один и тот же код ведёт себя по-разному в двух сборках. */
        runner = std::thread([this, loop] {
            loop->run("системный промпт", response);
            delete loop;
        });
        for (int i = 0; i < 300 && !host.entered.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~RunningAgent() {
        engine().request_abort();
        if (runner.joinable()) runner.join();
    }

    bool entered() const { return host.entered.load(); }
};

/* Держатель «окна»: поток-читатель, который гарантированно джойнится.
 *
 * `join()` без границы — ловушка: если метод заклинил, прогон повис бы до
 * сторожа на 30 с и упал с rc=3, то есть умер бы от таймаута молча, не
 * сказав, что именно заклинило. Поэтому ждём ОГРАНИЧЕННО, а при неудаче
 * отсоединяем: незаджойненный поток зовёт terminate и роняет прогон. */
struct UiReader {
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    std::atomic<long> rounds{0};
    std::thread t;

    /* body — что «окно» делает за кадр. */
    template <typename Body>
    void start(Body body) {
        t = std::thread([this, body] {
            long local = 0;
            while (!stop.load()) {
                body();
                ++local;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            rounds = local;
            done = true;
        });
    }

    /* Прогнать N секунд работы и остановить. */
    void run_for(int seconds) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        stop = true;
    }

    bool join_within(std::chrono::milliseconds limit) {
        const auto t0 = std::chrono::steady_clock::now();
        while (!done.load() &&
               std::chrono::steady_clock::now() - t0 < limit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!done.load()) {
            t.detach();
            return false;
        }
        t.join();
        return true;
    }
};

}  // namespace

TEST(окно_читает_состояние_пока_агент_работает_и_не_застревает) {
    /* ГЛАВНОЕ ИЗ ЗАДАЧИ. Агент жив (ход висит на провайдере), а «окно»
     * делает ровно то, что после И11.12 делает настоящее: короткий лок,
     * копия, отпуск. Если агент держит `state_.mtx` дольше, чем может
     * ждать кадр, — «окно» застревает, и это тот самый висящий GUI. */
    RunningAgent agent;
    ASSERT_TRUE(agent.entered());

    UiReader ui;
    ui.start([] {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        const size_t n = engine_state().session.size();
        const int state = static_cast<int>(engine_state().state);
        (void)n;
        (void)state;
    });
    ui.run_for(2);
    const bool joined = ui.join_within(std::chrono::milliseconds(5000));
    const long got = ui.rounds.load();

    ASSERT_TRUE(joined);
    /* Кадров должно быть МНОГО: агент жив, лок отпускается, «окно» рисует.
     * Малое число означало бы, что чтения блокировались почти всё время. */
    ASSERT_TRUE(got >= 100);
}

TEST(окно_зовёт_метод_движка_берущий_лок_пока_агент_работает) {
    /* Второй путь, который И11.12 закрыл: `session_profile` берёт
     * `state_.mtx` САМ. До И11.12 он звался ИЗ-ПОД лока окна, и это был
     * дедлок — мьютекс нерекурсивный, второй захват из-под первого не
     * вернулся бы. Здесь метод зовётся БЕЗ лока, пока агент работает, и
     * обязан возвращаться. Много раз: одиночный вызов прошёл бы и при
     * агенте, заклинившем на втором захвате. */
    RunningAgent agent;
    ASSERT_TRUE(agent.entered());

    UiReader ui;
    ui.start([] {
        harness::Profile p;
        std::string why;
        engine().session_profile(&p, &why);
    });
    ui.run_for(2);
    const bool joined = ui.join_within(std::chrono::milliseconds(5000));
    const long got = ui.rounds.load();

    ASSERT_TRUE(joined);
    ASSERT_TRUE(got >= 100);
}

TEST(агент_и_окно_не_встают_в_тупик_вместе) {
    /* Оба пути сразу: «окно» берёт лок напрямую И зовёт метод, берущий
     * лок сам. Именно так выглядел бы И11.12, если бы правка закрыла
     * дедлок в одном месте, а второй путь остался. */
    RunningAgent agent;
    ASSERT_TRUE(agent.entered());

    UiReader ui;
    ui.start([] {
        harness::Profile p;
        std::string why;
        engine().session_profile(&p, &why);
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        (void)engine_state().todos.size();
    });
    ui.run_for(2);
    const bool joined = ui.join_within(std::chrono::milliseconds(5000));
    const long got = ui.rounds.load();

    ASSERT_TRUE(joined);
    ASSERT_TRUE(got >= 100);
}

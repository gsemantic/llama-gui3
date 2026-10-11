/*
 * test_token_panel.cpp — панель токенов и стоимости (И11.9).
 *
 * Проверяется не «числа нарисованы», а то, что человек получит от панели
 * и чего он НЕ получит: стоимость не превращается в «$0.00» при
 * незаданной цене, оценка chars/4 не выдаётся за измеренную скорость, а
 * кэш и размышление не исчезают из итога.
 *
 * Отдельная проверка — инвариант счётчиков на живом движке: три новых
 * счётчика копятся тем же `record_turn_usage`, что и input/output, и
 * сумма трёх обязана равняться `total_prompt_tokens` (шапка engine.h).
 */

#include "core/token_panel.h"

#include "core/engine.h"
#include "core/limits.h"
#include "test_framework.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

using namespace coder;
using namespace coder::token_panel;

namespace {

/* Неразрывный пробел в разрядах — тот же символ, что и в панели. */
const std::string kSep = "\xC2\xA0";

/* Разбор без UI: панель собирается из снимка, поэтому проверка может
 * говорить о тексте, а не о пикселях. */
bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/* Типичная задача: вход с кэшем, выход с размышлением, скорость из
 * usage, цены заданы, окно известно. */
Snapshot measured_task() {
    Snapshot s;
    s.input = 12000;
    s.output = 3400;
    s.reasoning = 900;
    s.cache_read = 5000;
    s.cache_write = 1500;
    s.tokens_per_second = 42.5;
    s.response_seconds = 30.0;
    s.llm_seconds = 20.0;
    s.steps = 4;
    s.input_price_per_1m = 2.0;
    s.output_price_per_1m = 8.0;
    s.context.tokens = 9500;
    s.context.source = compaction::TokenSource::Measured;
    s.context_room = 32000;
    return s;
}

/* Снимок, в котором задача шла, но провайдер не прислал usage. */
Snapshot without_usage() {
    Snapshot s = measured_task();
    s.input = 0;
    s.output = 0;
    s.reasoning = 0;
    s.cache_read = 0;
    s.cache_write = 0;
    s.tokens_per_second = 42.5;
    s.tokens_estimated = true;
    s.context.tokens = 0;
    s.context.source = compaction::TokenSource::Estimated;
    return s;
}

/* Возврат метрик задачи. Движок — синглтон, состояние протекает между
 * тестами (реентерабельность, правило 5 SESSION_START), а проверки ниже
 * пишут в него напрямую. Объявлено здесь, а не внутри теста, по двум
 * причинам сразу: лок в деструкторе всё равно нужен, а объявление внутри
 * функции теста инициализирует поля в порядке ОБЪЯВЛЕНИЯ — ссылка на
 * владельца, объявленная последней, оказалась бы мусором (так и было). */
struct TokenMetricsGuard {
    explicit TokenMetricsGuard(EngineState& s) : s(&s) {
        p = s.total_prompt_tokens;
        c = s.total_completion_tokens;
        r = s.total_reasoning_tokens;
        cr = s.total_cache_read_tokens;
        cw = s.total_cache_write_tokens;
        mi = s.measured_input_tokens;
        est = s.tokens_estimated;
        tps = s.last_tokens_per_second;
        tg = s.last_tokens_generated;
    }
    ~TokenMetricsGuard() {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->total_prompt_tokens = p;
        s->total_completion_tokens = c;
        s->total_reasoning_tokens = r;
        s->total_cache_read_tokens = cr;
        s->total_cache_write_tokens = cw;
        s->measured_input_tokens = mi;
        s->tokens_estimated = est;
        s->last_tokens_per_second = tps;
        /* last_tokens_generated — ТОЖЕ восстанавливается: apply_task_speed
         * пишет и его, а забытый в возврате счётчик утекал в следующий
         * тест синглтона. Первая версия проверки «пустой ответ» упала
         * ровно на этом (получила 85 токенов от предыдущего теста). */
        s->last_tokens_generated = tg;
    }
    long long p, c, r, cr, cw, mi;
    double tps;
    int tg;
    bool est;
    EngineState* s;
};

}  // namespace

/* --- Видимость --- */

TEST(пустая_панель_не_показывается) {
    /* Ноль токенов, ноль шагов, ноль времени — это «задача не идёт».
     * Рамка с заголовком и нулями выглядела бы как сломанный счётчик. */
    const Panel p = build(Snapshot());
    ASSERT_FALSE(p.visible);
    ASSERT_TRUE(p.input_line.empty());
    ASSERT_TRUE(p.cost_line.empty());
    ASSERT_TRUE(p.steps_line.empty());
}

TEST(пустой_выход_но_измеренный_вход_уже_показывает_панель) {
    Snapshot s = measured_task();
    s.output = 0;
    s.reasoning = 0;
    s.tokens_per_second = 0.0;
    /* Ход, на котором модель позвала инструмент и не ответила текстом:
     * вход измерен, выхода нет. Панель обязана появиться — иначе человек
     * не увидит, что запрос к модели стоил денег. */
    ASSERT_TRUE(build(s).visible);
}

TEST(панель_показывается_по_времени_даже_без_usage) {
    Snapshot s = without_usage();
    s.llm_seconds = 5.0;
    ASSERT_TRUE(build(s).visible);
}

/* --- Счётчики --- */

TEST(вход_показывает_кэш_раздельно) {
    const Panel p = build(measured_task());
    ASSERT_TRUE(has(p.input_line, "12" + kSep + "000"));
    ASSERT_TRUE(has(p.input_line, "чтение 5" + kSep + "000"));
    ASSERT_TRUE(has(p.input_line, "запись 1" + kSep + "500"));
}

TEST(выход_показывает_размышление) {
    const Panel p = build(measured_task());
    ASSERT_TRUE(has(p.output_line, "3" + kSep + "400"));
    ASSERT_TRUE(has(p.output_line, "размышление: 900"));
}

TEST(нулевой_кэш_не_рисует_разбивку) {
    /* «кэш: чтение 0, запись 0» читалось бы как «провайдер сообщил, что
     * кэша нет», а это разные вещи с тем же видом на экране. */
    Snapshot s = measured_task();
    s.cache_read = 0;
    s.cache_write = 0;
    const Panel p = build(s);
    ASSERT_TRUE(has(p.input_line, "12" + kSep + "000"));
    ASSERT_FALSE(has(p.input_line, "кэш"));
}

TEST(нулевое_размышление_не_рисует_разбивку) {
    Snapshot s = measured_task();
    s.reasoning = 0;
    const Panel p = build(s);
    ASSERT_TRUE(has(p.output_line, "3" + kSep + "400"));
    ASSERT_FALSE(has(p.output_line, "размышление"));
}

TEST(кэш_и_размышление_не_складываются_с_входом_и_выходом) {
    /* Кэш ВНУТРИ input (инвариант Usage), размышление ВНУТРИ output.
     * Складывать их во второй раз — значит показать вход 18500 при
     * измеренных 12000. */
    Snapshot s = measured_task();
    const Panel p = build(s);
    ASSERT_FALSE(has(p.input_line, "18" + kSep + "500"));
    ASSERT_FALSE(has(p.output_line, "4" + kSep + "300"));
}

/* --- Формат числа --- */

TEST(число_разбивается_на_разряды) {
    ASSERT_EQ(format_count(0), std::string("0"));
    ASSERT_EQ(format_count(7), std::string("7"));
    ASSERT_EQ(format_count(999), std::string("999"));
    ASSERT_EQ(format_count(1000), std::string("1" + kSep + "000"));
    ASSERT_EQ(format_count(12345), std::string("12" + kSep + "345"));
    ASSERT_EQ(format_count(1000000), std::string("1" + kSep + "000" + kSep + "000"));
}

TEST(разряды_не_переносят_знак_минус_в_группу) {
    /* Ошибка была бы не в цифрах, а в виде: «-1 234» и «1 -234» читаются
     * по-разному, и второе выглядит как положительное число. */
    const std::string s = format_count(-1234);
    ASSERT_EQ(s.front(), '-');
    ASSERT_TRUE(has(s, "1" + kSep + "234"));
}

/* --- Стоимость --- */

TEST(стоимость_считается_по_обеим_ставкам) {
    const Snapshot s = measured_task();
    /* 12000/1e6*2 + 3400/1e6*8 = 0.024 + 0.0272 = 0.0512 */
    const double want = 12000.0 / 1e6 * 2.0 + 3400.0 / 1e6 * 8.0;
    /* Сравнение с допуском, а не ASSERT_EQ: сумма считается в double, и
     * печатное представление здесь было бы проверкой округления, а не
     * арифметики. */
    ASSERT_TRUE(std::fabs(cost_for(s) - want) < 1e-9);
}

TEST(стоимость_в_подписи_печатается_числом) {
    const Panel p = build(measured_task());
    ASSERT_TRUE(has(p.cost_line, "стоимость: $"));
    ASSERT_FALSE(has(p.cost_line, "не посчитана"));
}

TEST(незаданная_цена_не_показывается_как_ноль) {
    /* Главное. «$0.00» утверждало бы, что работа была бесплатной, а
     * человек просто не задал цену. */
    Snapshot s = measured_task();
    s.input_price_per_1m = 0.0;
    s.output_price_per_1m = 0.0;
    const Panel p = build(s);
    ASSERT_FALSE(has(p.cost_line, "$"));
    ASSERT_TRUE(has(p.cost_line, "цена не задана"));
    ASSERT_TRUE(has(p.cost_line, "wp_coder.price_input_per_1m"));
    ASSERT_TRUE(has(p.cost_line, "wp_coder.price_output_per_1m"));
}

TEST(одна_ставка_без_второй_сумму_не_считает) {
    /* Половина от суммы выглядела бы как итог: человек увидел бы «$0.024»
     * и не узнал бы, что выход не посчитан. */
    Snapshot only_in = measured_task();
    only_in.output_price_per_1m = 0.0;
    ASSERT_TRUE(has(build(only_in).cost_line, "не задана цена выхода"));
    ASSERT_FALSE(has(build(only_in).cost_line, "$"));

    Snapshot only_out = measured_task();
    only_out.input_price_per_1m = 0.0;
    ASSERT_TRUE(has(build(only_out).cost_line, "не задана цена входа"));
    ASSERT_FALSE(has(build(only_out).cost_line, "$"));
}

TEST(сама_арифметика_тоже_не_считает_по_одной_ставке) {
    /* Проверка подписи проверяет текст, а не сумму: сняв её, можно было бы
     * оставить `cost_for` считающим по одной ставке, и подпись всё равно
     * сказала бы «не посчитана» — то есть проверка подписи осталась бы
     * зелёной при коде, который считает неверно. Мутация «одна ставка
     * считается как итог» именно это и пропустила, пока не появилась
     * вот эта. */
    Snapshot only_in = measured_task();
    only_in.output_price_per_1m = 0.0;
    ASSERT_TRUE(cost_for(only_in) == 0.0);
    Snapshot only_out = measured_task();
    only_out.input_price_per_1m = 0.0;
    ASSERT_TRUE(cost_for(only_out) == 0.0);
}

TEST(ненулевая_сумма_не_печатается_как_нулевая) {
    /* 500 токенов по $0.01 за 1M — это $0.000005. «$0.0000» читается как
     * «бесплатно», то есть ровно то заключение, которого панель обязана
     * не допускать. */
    Snapshot s;
    s.input = 500;
    s.steps = 1;
    s.input_price_per_1m = 0.01;
    s.output_price_per_1m = 0.01;
    const std::string line = cost_line(s);
    ASSERT_FALSE(has(line, "$0.0000"));
    ASSERT_TRUE(has(line, "< $0.0001"));
}

TEST(кэш_считается_по_цене_входа_и_это_сказано) {
    /* Кэш не складывается со входом второй раз: он внутри него. Но у
     * части провайдеров кэш дешевле, поэтому сумма — верхняя оценка, и
     * панель обязана об этом сказать, а не выдать число как точный счёт. */
    const Panel p = build(measured_task());
    ASSERT_TRUE(has(p.cost_line, "верхняя оценка"));
}

TEST(без_кеша_пометки_об_оценке_нет) {
    Snapshot s = measured_task();
    s.cache_read = 0;
    s.cache_write = 0;
    ASSERT_FALSE(has(build(s).cost_line, "верхняя оценка"));
}

/* --- Скорость --- */

TEST(измеренная_скорость_показывается_числом) {
    const std::string line = speed_line(measured_task());
    ASSERT_TRUE(has(line, "42.5 ток/с"));
    ASSERT_TRUE(has(line, "20.0 с"));
}

TEST(оценка_скорости_не_показывается_числом) {
    /* run_task подставляет chars/4, когда usage не пришёл, и скорость
     * считается уже по оценке. Показанное здесь число выглядело бы как
     * измерение — тем же шрифтом, что честная скорость. */
    const std::string line = speed_line(without_usage());
    ASSERT_FALSE(has(line, "42.5"));
    ASSERT_TRUE(has(line, "оценка"));
    ASSERT_TRUE(has(line, "провайдер не прислал usage"));
}

TEST(оценка_скорости_помечает_себя_раньше_чем_число_о_времени) {
    /* Порядок частей важен для читателя: сначала ЧТО это, потом число. */
    const std::string line = speed_line(without_usage());
    ASSERT_TRUE(line.find("оценка") < line.find("скорость") + 100);
}

TEST(без_скорости_названа_причина) {
    Snapshot s = measured_task();
    s.tokens_per_second = 0.0;
    const std::string line = speed_line(s);
    ASSERT_FALSE(has(line, "0.0 ток/с"));
    ASSERT_TRUE(has(line, "нет данных"));
}

/* --- Контекст --- */

TEST(контекст_показывает_долю_от_окна) {
    bool tight = false;
    const std::string line = context_line(measured_task(), &tight);
    ASSERT_TRUE(has(line, "9" + kSep + "500"));
    ASSERT_TRUE(has(line, "32" + kSep + "000"));
    /* 9500 / 32000 = 29.6875% */
    ASSERT_TRUE(has(line, "29.7%"));
    ASSERT_TRUE(has(line, "измерено"));
    ASSERT_FALSE(tight);
}

TEST(незаданное_окно_называет_себя_а_не_показывает_ноль_процентов) {
    /* Ноль процентов читался бы как «контекст пуст». */
    Snapshot s = measured_task();
    s.context_room = 0;
    bool tight = false;
    const std::string line = context_line(s, &tight);
    ASSERT_FALSE(has(line, "%"));
    ASSERT_TRUE(has(line, "лимит окна не задан"));
    ASSERT_FALSE(tight);
}

TEST(оценка_контекста_помечается_словом) {
    /* И7.2: «измерено» и «оценка» — разные достоверности, и выглядеть
     * они должны по-разному. */
    Snapshot s = measured_task();
    s.context.source = compaction::TokenSource::Estimated;
    ASSERT_TRUE(has(context_line(s, nullptr), "оценка"));
}

TEST(порог_помечается_когда_контекст_до_него_дошёл) {
    Snapshot s = measured_task();
    s.context.tokens = s.context_room;
    bool tight = false;
    context_line(s, &tight);
    ASSERT_TRUE(tight);
}

TEST(чуть_меньше_порога_не_помечается) {
    /* Равенство — да, на единицу меньше — нет: иначе «упирается»
     * означало бы «в любой момент упрётся», и предупреждение перестало
     * бы значить что-нибудь. */
    Snapshot s = measured_task();
    s.context.tokens = s.context_room - 1;
    bool tight = false;
    context_line(s, &tight);
    ASSERT_FALSE(tight);
}

/* --- Шаги и время --- */

TEST(шаги_и_время_показываются) {
    const Panel p = build(measured_task());
    ASSERT_TRUE(has(p.steps_line, "шагов: 4"));
    ASSERT_TRUE(has(p.steps_line, "30.0 с"));
    ASSERT_TRUE(has(p.steps_line, "20.0 с"));
}

TEST(нулевое_время_печатается_тире_а_не_нулём) {
    /* «0.0 с у модели» читается как «модель не отвечала», что при
     * отсутствии usage ещё и правда, но выглядит как измерение. */
    Snapshot s = measured_task();
    s.llm_seconds = 0.0;
    s.response_seconds = 0.0;
    const Panel p = build(s);
    ASSERT_FALSE(has(p.steps_line, "0.0 с"));
    ASSERT_TRUE(has(p.steps_line, "—"));
}

/* ======================================================================
 * Живой движок: счётчики и настройки
 * ====================================================================== */

TEST(session_usage_counts_match_the_usage_invariants) {
    /* Три новых счётчика копятся тем же `record_turn_usage`, что и
     * input/output. Инвариант Usage: non_cached + cache_read +
     * cache_write == input. Нарушение заметил бы только человек,
     * сравнивающий панель с журналом провайдера.
     *
     * Лок НЕ держится: функция берёт `state_.mtx` сама, а мьютекс
     * нерекурсивный — вызов под локом повесил бы прогон (первая версия
     * проверки именно так и висела, сторож поймал). */
    EngineState& st = engine_state();
    TokenMetricsGuard guard(st);

    Usage u = Usage::from_provider(12000, 3400, 5000, 1500,
                                   /*cache_counted_in_prompt=*/true);
    u.set_reasoning(900);
    ASSERT_TRUE(u.invariants_hold());

    record_turn_usage(st, u);
    record_turn_usage(st, u);

    std::lock_guard<std::mutex> lk(st.mtx);
    ASSERT_EQ(st.total_prompt_tokens, guard.p + 24000);
    ASSERT_EQ(st.total_completion_tokens, guard.c + 6800);
    ASSERT_EQ(st.total_reasoning_tokens, guard.r + 1800);
    ASSERT_EQ(st.total_cache_read_tokens, guard.cr + 10000);
    ASSERT_EQ(st.total_cache_write_tokens, guard.cw + 3000);
    /* Ключевое: сумма трёх равна входу, а не «входу плюс кэш». */
    ASSERT_EQ(st.total_cache_read_tokens + st.total_cache_write_tokens +
                  (st.total_prompt_tokens - st.total_cache_read_tokens -
                   st.total_cache_write_tokens),
              st.total_prompt_tokens);
}

TEST(сброс_метрик_задачи_обнуляет_и_новые_счётчики) {
    /* Забытый здесь счётчик жил бы в панели следующей задачи как «дешёвый
     * запрос, которого не делал». */
    EngineState& st = engine_state();
    TokenMetricsGuard guard(st);

    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.total_reasoning_tokens = 7;
        st.total_cache_read_tokens = 30;
        st.total_cache_write_tokens = 10;
        st.tokens_estimated = true;
        st.last_tokens_per_second = 5.0;
        reset_task_metrics(st);
    }
    std::lock_guard<std::mutex> lk(st.mtx);
    ASSERT_EQ(st.total_reasoning_tokens, 0LL);
    ASSERT_EQ(st.total_cache_read_tokens, 0LL);
    ASSERT_EQ(st.total_cache_write_tokens, 0LL);
    ASSERT_FALSE(st.tokens_estimated);
    ASSERT_EQ(st.last_tokens_per_second, 0.0);
}

/* --- Скорость: единственный её писатель --- */

TEST(измеренная_скорость_не_помечается_оценкой) {
    /* apply_task_speed — единственное место, где ставится флаг оценки.
     * Проверка на панели без этой ничего не значила бы: панель честно
     * говорила бы «оценка», а флаг никто бы не ставил, и человек видел бы
     * «42 ток/с» как измерение. Мутация «флаг не ставится» именно это и
     * пропустила, пока проверки на живом движке не было. */
    EngineState& st = engine_state();
    TokenMetricsGuard guard(st);
    /* Счётчики обнуляются: движок — синглтон, и к прошлому тесту в нём
     * могли остаться токены (первая версия проверки складывала свои 3400 с
     * чужими и получала не 1700 ток/с). */
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.total_prompt_tokens = 0;
        st.total_completion_tokens = 0;
    }
    Usage u = Usage::from_provider(12000, 3400);
    record_turn_usage(st, u);
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.last_response = "ответ модели";
        apply_task_speed(st, "ответ модели", 2.0);
        ASSERT_FALSE(st.tokens_estimated);
        ASSERT_EQ(st.last_tokens_per_second, 1700.0);
        ASSERT_EQ(st.last_tokens_generated, 3400);
    }
}

TEST(оценка_скорости_помечается_флагом) {
    /* usage не пришёл — выходных токенов нет, и скорость посчитана по
     * chars/4. Флаг обязан встать, иначе панель покажет оценку как
     * измерение. */
    EngineState& st = engine_state();
    TokenMetricsGuard guard(st);
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.total_completion_tokens = 0;
        apply_task_speed(st, std::string(400, 'x'), 2.0);
        ASSERT_TRUE(st.tokens_estimated);
        /* 400 символов / 4 = 100 токенов оценки, 100 / 2 с = 50 ток/с */
        ASSERT_EQ(st.last_tokens_generated, 100);
        ASSERT_EQ(st.last_tokens_per_second, 50.0);
    }
}

TEST(пустой_ответ_оценкой_не_считается) {
    /* «(пустой ответ)» — это подстановка run_task, а ответ модели;
     * оценивать по нему нечего, и оценка 1 токен означала бы скорость
     * 0.5 ток/с, то есть выдуманное число. */
    EngineState& st = engine_state();
    TokenMetricsGuard guard(st);
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.total_completion_tokens = 0;
        apply_task_speed(st, std::string(), 2.0);
        ASSERT_FALSE(st.tokens_estimated);
        ASSERT_EQ(st.last_tokens_generated, 0);
        ASSERT_EQ(st.last_tokens_per_second, 0.0);
    }
}

/* --- Граница с окном --- */

TEST(окно_берёт_решение_панели_из_core) {
    /* Окно ImGui юнит-тестом не проверяется, поэтому сверяем по исходнику:
     * решение пришло из core, а окно только рисует. Правка решения обратно
     * в окно расходилась бы с проверками панели молча — они бы
     * продолжали зелёными проверять функцию, которую никто не зовёт. */
    std::ifstream f(fs::path(__FILE__).parent_path().parent_path() /
                        "ui/coder_window.cpp",
                    std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string ui = ss.str();
    ASSERT_FALSE(ui.empty());

    /* Подписи и решения — в core. */
    ASSERT_TRUE(ui.find("token_panel::build(") != std::string::npos);
    /* Лок отпускается ДО отрисовки: держать state_.mtx под ImGui — висящий
     * UI (D1, правило 3 SESSION_START). */
    const std::size_t snap_at = ui.find("take_token_panel() {");
    ASSERT_TRUE(snap_at != std::string::npos);
    const std::size_t snap_end = ui.find("\nstatic ", snap_at + 10);
    const std::string snap_body = ui.substr(
        snap_at, snap_end == std::string::npos ? std::string::npos : snap_end - snap_at);
    ASSERT_TRUE(snap_body.find("lock_guard") != std::string::npos);
    /* Возвращается готовая панель, а не ссылка на состояние. */
    ASSERT_TRUE(snap_body.find("return coder::token_panel::build(snap);") !=
                std::string::npos);
    ASSERT_TRUE(snap_body.find("EngineState&") == std::string::npos);

    /* Подписи не собираются в окне: окно читает готовые поля. */
    const std::size_t draw_at = ui.find("static void draw_token_lines(");
    ASSERT_TRUE(draw_at != std::string::npos);
    const std::size_t draw_end = ui.find("\nstatic ", draw_at + 10);
    const std::string draw_body = ui.substr(
        draw_at, draw_end == std::string::npos ? std::string::npos : draw_end - draw_at);
    ASSERT_TRUE(draw_body.find("lock_guard") == std::string::npos);
    ASSERT_TRUE(draw_body.find("format_count") == std::string::npos);
    ASSERT_TRUE(draw_body.find("cost_line(") == std::string::npos);
    ASSERT_TRUE(draw_body.find("speed_line(") == std::string::npos);
    ASSERT_TRUE(draw_body.find("context_line(") == std::string::npos);
    ASSERT_TRUE(draw_body.find("\"стоимость") == std::string::npos);
    ASSERT_TRUE(draw_body.find("\"вход") == std::string::npos);
    ASSERT_TRUE(draw_body.find("\"контекст") == std::string::npos);
    /* Панель рисуется в обоих местах из ОДНОГО снимка: две копии полей
     * разошлись бы при первой правке счётчика. */
    ASSERT_TRUE(ui.find("render_token_panel();") != std::string::npos);
    const std::size_t extras_at = ui.find("void render_extras()");
    ASSERT_TRUE(extras_at != std::string::npos);
    const std::string extras =
        ui.substr(extras_at, ui.find("\n}", extras_at) - extras_at);
    ASSERT_TRUE(extras.find("take_token_panel()") != std::string::npos);
}
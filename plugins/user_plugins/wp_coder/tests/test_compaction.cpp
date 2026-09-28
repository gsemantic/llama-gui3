/*
 * test_compaction.cpp — математика переполнения контекста (И7.1).
 *
 * Здесь нет ни цикла, ни модели, ни сети: только арифметика, решающая,
 * пора ли сжимать историю. Проверять её дорого в другом месте (проверка
 * «агент сжимает» требовала бы сети и целой сессии), поэтому она и
 * вынесена в чистые функции — и именно поэтому каждая проверка ниже
 * табличная: не «посчиталось число», а «каждый из этих случаев даёт
 * такой-то порог», потому что решение принимается по одному числу, и
 * ошибка в нём означает либо вечный compaction, либо переполнение.
 *
 * Каждая проверка проверена мутацией (правка формулы роняет именно её,
 * а не соседнюю) — см. коммит задачи 7.1.
 */

#include "core/compaction.h"
#include "core/engine.h"
#include "core/json_utils.h"   /* text::utf8_prefix/is_valid_utf8 */
#include "core/prompts.h"   /* kCompactionSystemPrompt: строки формата */

#include "test_framework.h"
#include "test_support.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace coder;
using namespace coder::compaction;

namespace {

/* Сборка лимитов покороче: в таблице преобладают случаи «окно + ответ». */
ModelLimits limits_of(long long context, long long max_output,
                      long long input = 0) {
    ModelLimits l;
    l.context = context;
    l.max_output = max_output;
    l.input = input;
    return l;
}

struct Case {
    long long context;
    long long input;
    long long max_output;
    long long expected;
    const char* what;
};

/* Таблица «окно → сколько токенов истории помещается». Ветка по окну:
 * вычитается ПОЛНЫЙ лимит ответа, а не буфер сводки. Последний случай
 * (32000) — тот, ради которого ветка и написана: модель с ответом больше
 * буфера, и история, выросшая до context - 20000, увела бы запрос за
 * окно. Формула плана «min(20000, max_output)» дала бы здесь 12768 и
 * промолчала (отклонение №57). */
const Case kWindowBranch[] = {
    {32768, 0, 8192, 24576, "типичная локальная модель"},
    {32768, 0, 4096, 28672, "короткий ответ"},
    {32768, 0, 32000, 768,  "ответ длиннее буфера сводки"},
    {32768, 0, 0,     32768, "лимит ответа неизвестен"},
    {8192,  0, 8192,  0,     "окно равно ответу: места нет"},
    {4096,  0, 8192,  0,     "ответ больше окна: места нет"},
};

} // anonymous namespace

TEST(usable_takes_the_whole_output_limit_out_of_the_context) {
    for (const Case& c : kWindowBranch) {
        const CompactionConfig cfg;
        const long long got = usable(limits_of(c.context, c.max_output, c.input), cfg);
        if (got != c.expected) {
            std::cerr << "  " << c.what << ": контекст " << c.context
                      << ", ответ " << c.max_output << " → порог " << got
                      << ", ожидалось " << c.expected << std::endl;
        }
        ASSERT_EQ(got, c.expected);
    }
}

TEST(usable_uses_the_input_limit_when_the_provider_declares_one) {
    const CompactionConfig cfg;
    /* Вход ограничен отдельно → резерв под сводку, а не весь ответ. */
    ASSERT_EQ(usable(limits_of(32768, 8192, 20000), cfg), 11808);
    /* Ответ длиннее буфера, но вход отдельный: буфер режет резерв до
     * 20000, то есть до предела, при котором вход ещё помещается. */
    ASSERT_EQ(usable(limits_of(32768, 32000, 20000), cfg), 0);
    /* Лимит входа меньше окна — вход и есть порог, а не окно. */
    ASSERT_EQ(usable(limits_of(32768, 1000, 8000), cfg), 7000);
}

TEST(usable_is_zero_when_the_window_is_unknown) {
    const CompactionConfig cfg;
    /* Окно не объявлено — считать нечем, даже если известен лимит входа:
     * порог ставится на окно, а не на вход. */
    ASSERT_EQ(usable(limits_of(0, 8192), cfg), 0);
    ASSERT_EQ(usable(limits_of(0, 8192, 20000), cfg), 0);
}

TEST(reserve_is_the_buffer_capped_by_the_output_limit) {
    const CompactionConfig cfg;
    ModelLimits m;
    m.context = 32768;

    m.max_output = 0;
    ASSERT_EQ(reserve_for(m, cfg), 0);
    m.max_output = 15000;
    ASSERT_EQ(reserve_for(m, cfg), 15000);
    m.max_output = 20000;
    ASSERT_EQ(reserve_for(m, cfg), 20000);
    /* Длиннее буфера — режется, иначе резерв съел бы место, которого
     * ждёт не сводка, а ответ. */
    m.max_output = 64000;
    ASSERT_EQ(reserve_for(m, cfg), 20000);
}

TEST(reserved_from_settings_replaces_the_default_buffer) {
    ModelLimits m = limits_of(32768, 8192, 20000);
    CompactionConfig cfg;
    cfg.reserved = 4000;
    ASSERT_EQ(reserve_for(m, cfg), 4000);
    ASSERT_EQ(usable(m, cfg), 16000);
    /* Резерв больше умолчания тоже выигрывает — иначе настройка была бы
     * «работает, только если меньше». */
    cfg.reserved = 40000;
    ASSERT_EQ(reserve_for(m, cfg), 40000);
    ASSERT_EQ(usable(m, cfg), 0);
}

TEST(reserved_only_reaches_the_input_branch) {
    /* Асимметрия порта (overflow.ts: reserved вычитается только там, где
     * есть limit.input). Закреплена, чтобы «сделать одинаково» было
     * решением с обоснованием, а не случайностью. */
    CompactionConfig cfg;
    cfg.reserved = 40000;
    const ModelLimits with_input = limits_of(32768, 8192, 20000);
    const ModelLimits window_only = limits_of(32768, 8192);
    ASSERT_EQ(usable(with_input, cfg), 0);
    ASSERT_EQ(usable(window_only, cfg), 24576);
}

TEST(overflow_starts_exactly_at_the_threshold) {
    const CompactionConfig cfg;
    const ModelLimits m = limits_of(32768, 8192);
    ASSERT_EQ(usable(m, cfg), 24576);
    /* На границе — уже переполнение: следующий запрос не поместится, и
     * ждать, пока история перестанет расти, нельзя. */
    ASSERT_TRUE(is_overflow(m, cfg, 24576));
    ASSERT_FALSE(is_overflow(m, cfg, 24575));
    ASSERT_TRUE(is_overflow(m, cfg, 30000));
    ASSERT_FALSE(is_overflow(m, cfg, 0));
}

TEST(overflow_is_off_when_auto_compaction_is_disabled) {
    CompactionConfig cfg;
    cfg.auto_compact = false;
    const ModelLimits m = limits_of(32768, 8192);
    ASSERT_FALSE(is_overflow(m, cfg, 24576));
    ASSERT_FALSE(is_overflow(m, cfg, 1000000000));
}

TEST(overflow_is_off_when_the_limits_are_unknown) {
    /* Окно неизвестно — порога не существует. Самая опасная форма
     * проверки: без неё пустая история выглядела бы «мест нет» и
     * агент сжимал бы по кругу. */
    const CompactionConfig cfg;
    const ModelLimits unknown = limits_of(0, 8192);
    ASSERT_FALSE(is_overflow(unknown, cfg, 0));
    ASSERT_FALSE(is_overflow(unknown, cfg, 1000000000));
}

TEST(overflow_is_not_claimed_when_there_is_no_room_at_all) {
    /* Окно равно лимиту ответа: сжимать нечем, и повторное сжатие ничего
     * не освободит. Объявлять переполнение здесь — значит заставить
     * вызывающего сжимать историю снова и снова. */
    const CompactionConfig cfg;
    const ModelLimits m = limits_of(8192, 8192);
    ASSERT_EQ(usable(m, cfg), 0);
    ASSERT_FALSE(is_overflow(m, cfg, 1));
    ASSERT_FALSE(is_overflow(m, cfg, 1000000000));
}

/* ======================================================================
 * И7.2 — откуда берётся число, которое сравнивают с порогом
 * ====================================================================== */

TEST(token_estimate_is_chars_over_four_with_port_rounding) {
    /* Портовское Math.round(n/4), а не целочисленное деление: на 7
     * символах порт даёт 2, деление дало бы 1. */
    ASSERT_EQ(estimate_tokens(""), 0LL);
    ASSERT_EQ(estimate_tokens("a"), 0LL);
    ASSERT_EQ(estimate_tokens("ab"), 1LL);
    ASSERT_EQ(estimate_tokens("abcd"), 1LL);
    ASSERT_EQ(estimate_tokens("abcde"), 1LL);
    ASSERT_EQ(estimate_tokens("abcdef"), 2LL);
    ASSERT_EQ(estimate_tokens("abcdefg"), 2LL);
    ASSERT_EQ(estimate_tokens(std::string(400, 'x')), 100LL);
    ASSERT_EQ(estimate_tokens(std::string(1000, 'x')), 250LL);
    /* Отрицательное число символов — не оценка, а ошибка вызывающего;
     * вернуть ноль безопаснее, чем отрицательные токены в панели. */
    ASSERT_EQ(tokens_from_chars(-5), 0LL);
}

TEST(history_estimate_counts_only_what_the_model_actually_sees) {
    /* Реплика пользователя из 400 символов уходит в модель как есть. */
    std::vector<Message> history;
    history.push_back(Message::user(std::string(400, 'x')));
    ASSERT_EQ(estimate_history_tokens(history), 100LL);

    /* Размышление в транскрипт НЕ попадает (core/message.h): оценка по
     * сырым частям считала бы в контексте то, что модели не уходит, и
     * завышала бы объём вдвое-втрое на ходах с длинным размышлением. */
    Message turn = Message::assistant(history.front().id);
    turn.parts.push_back(MessagePart::reasoning(std::string(4000, 'r')));
    history.push_back(turn);
    ASSERT_EQ(estimate_history_tokens(history), 100LL);

    /* А вот текст хода попадает — и оценка обязана это увидеть.
     * Правка части — в КОПИИ внутри истории: push_back выше уже скопировал
     * ход, и добавление части в локальный turn не изменило бы историю
     * (ошибка, которую поймало первое же прогонание теста). */
    history.back().parts.push_back(MessagePart::text(std::string(400, 'y')));
    ASSERT_EQ(estimate_history_tokens(history), 200LL);
}

TEST(measured_input_wins_over_the_estimate) {
    std::vector<Message> history;
    history.push_back(Message::user(std::string(400, 'x')));
    const ContextUsage u = context_usage(1234, history);
    ASSERT_EQ(u.tokens, 1234LL);
    ASSERT_TRUE(u.measured());
    ASSERT_EQ(std::string(token_source_name(u.source)), std::string("измерено"));
}

TEST(no_measurement_falls_back_to_the_estimate) {
    /* Ноль у провайдера — это «usage не пришёл» (первый запрос, старый
     * хост), а НЕ «контекст пуст»: иначе панель показала бы ноль на
     * полной истории, а панель токенов (И11.9) наследовала бы эту
     * ошибку. */
    std::vector<Message> history;
    history.push_back(Message::user(std::string(400, 'x')));
    const ContextUsage u = context_usage(0, history);
    ASSERT_EQ(u.tokens, 100LL);
    ASSERT_FALSE(u.measured());
    ASSERT_EQ(std::string(token_source_name(u.source)), std::string("оценка"));
}

/* --- Лимиты из настроек ---
 *
 * Проверка идёт через load_settings, а не через присваивание полей:
 * значение приходит строкой из настроек хоста, и весь смысл задачи — в
 * том, как строка превращается в число. Прямое присваивание проверило
 * бы структуру, а не разбор. */

namespace {

/* Хост, отдающий заданные настройки. */
HostCallbacks settings_host(const std::map<std::string, std::string>& values) {
    HostCallbacks cb;
    const auto* v = &values;
    cb.settings_get = [v](const std::string& key, const std::string& def) {
        auto it = v->find(key);
        return it == v->end() ? def : it->second;
    };
    return cb;
}

/* Движок — синглтон, и правило реентерабельности (SESSION_START.md,
 * ограничение 5) требует восстанавливать то, что тест изменил. */
struct LimitsGuard {
    LimitsGuard()
        : limits(engine_state().model_limits),
          config(engine_state().compaction_config),
          budget(engine_state().session_budget),
          steps(engine_state().max_steps) {}
    ~LimitsGuard() {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().model_limits = limits;
        engine_state().compaction_config = config;
        engine_state().session_budget = budget;
        engine_state().max_steps = steps;
    }
    compaction::ModelLimits limits;
    compaction::CompactionConfig config;
    size_t budget;
    int steps;
};

} // anonymous namespace

TEST(limits_are_read_from_settings) {
    LimitsGuard guard;
    {
        std::map<std::string, std::string> v;
        v["wp_coder.context_limit"] = "32768";
        v["wp_coder.input_limit"] = "20000";
        v["wp_coder.max_output_tokens"] = "8192";
        v["wp_coder.compaction_auto"] = "false";
        v["wp_coder.compaction_reserved"] = "4000";
        HostCallbacks cb = settings_host(v);
        engine().init(cb);
        engine().load_settings();
    }
    ASSERT_EQ(engine_state().model_limits.context, 32768LL);
    ASSERT_EQ(engine_state().model_limits.input, 20000LL);
    ASSERT_EQ(engine_state().model_limits.max_output, 8192LL);
    ASSERT_FALSE(engine_state().compaction_config.auto_compact);
    ASSERT_EQ(engine_state().compaction_config.reserved, 4000LL);
    /* Настройки должны влиять на порог, а не просто лежать в состоянии. */
    ASSERT_EQ(usable(engine_state().model_limits,
                     engine_state().compaction_config), 16000LL);
}

TEST(garbage_in_limits_means_unknown_not_a_guess) {
    LimitsGuard guard;
    {
        std::map<std::string, std::string> v;
        v["wp_coder.context_limit"] = "много";
        v["wp_coder.input_limit"] = "-1";
        v["wp_coder.max_output_tokens"] = "0x4000";
        v["wp_coder.compaction_reserved"] = "-500";
        HostCallbacks cb = settings_host(v);
        engine().init(cb);
        engine().load_settings();
    }
    /* Мусор → «не задано» (0), а не дефолт: стойкое неверное значение
     * лимита хуже отсутствия, потому что отсутствие видно, а неверное
     * число принимается за правду. */
    ASSERT_EQ(engine_state().model_limits.context, 0LL);
    ASSERT_EQ(engine_state().model_limits.input, 0LL);
    ASSERT_EQ(engine_state().model_limits.max_output, 0LL);
    ASSERT_EQ(engine_state().compaction_config.reserved, 0LL);
    /* И главное: без лимитов переполнение не объявляется никогда, то
     * есть агент работает как раньше, а не сжимает историю наугад. */
    ASSERT_FALSE(is_overflow(engine_state().model_limits,
                             engine_state().compaction_config, 1000000000LL));
}

TEST(negative_limits_are_treated_as_unknown) {
    /* Отрицательное число в настройке — не «лимит», а ошибка ввода.
     * Держать его в состоянии нельзя: значение уехало бы в порог, и
     * панель показала бы выдуманное число как настоящее. */
    LimitsGuard guard;
    {
        std::map<std::string, std::string> v;
        v["wp_coder.context_limit"] = "-1";
        v["wp_coder.input_limit"] = "-1";
        v["wp_coder.max_output_tokens"] = "-32000";
        HostCallbacks cb = settings_host(v);
        engine().init(cb);
        engine().load_settings();
    }
    ASSERT_EQ(engine_state().model_limits.context, 0LL);
    ASSERT_EQ(engine_state().model_limits.input, 0LL);
    ASSERT_EQ(engine_state().model_limits.max_output, 0LL);
}

TEST(task_metrics_start_from_zero_whatever_the_previous_task_measured) {
    /* Измерение контекста относится к ПОСЛЕДНЕМУ запросу. Новая задача
     * начинается с другой (часто пустой) истории, и оставленное измерение
     * означало бы «измерено 40 000 токенов» для пустого контекста — а
     * позже компакшн сравнил бы чужое число с новой историей.
     *
     * Проверяется reset_task_metrics(), а не run_task: тот достижим
     * только через worker синглона, и тест на нём зависает на общей
     * очереди (И6.8 — ровно эта причина, по которой правило и вынесено). */
    EngineState& st = engine_state();
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.llm_total_time = 12.5;
        st.total_prompt_tokens = 4000;
        st.total_completion_tokens = 300;
        st.last_tokens_per_second = 25.0;
        st.steps = 7;
        st.measured_input_tokens = 4000;
    }
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        reset_task_metrics(st);
    }
    std::lock_guard<std::mutex> lk(st.mtx);
    ASSERT_EQ(st.total_prompt_tokens, 0);
    ASSERT_EQ(st.total_completion_tokens, 0);
    ASSERT_EQ(st.steps, 0);
    ASSERT_EQ(st.measured_input_tokens, 0LL);
    ASSERT_EQ(st.llm_total_time, 0.0);
    ASSERT_EQ(st.last_tokens_per_second, 0.0);
}

/* ======================================================================
 * И7.3 — выбор хвоста
 * ======================================================================
 *
 * ВСЕ размеры ниже — в ТОКЕНАХ, а не в символах. Оценка — это chars/4
 * (И7.2), и первые прогоны этих тестов были посчитаны в символах: ход
 * «на 4000 символов» оказался не четырьмя токенами, а тысячей, и девять
 * проверок падали не из-за кода. Размер в символах = размер в токенах ×
 * limits::kCharsPerToken, и пересчёт живёт в фикстурах ниже.
 */

namespace {

/* Сколько символов в заданном числе токенов. */
size_t chars_of(long long tokens) {
    return static_cast<size_t>(tokens * limits::kCharsPerToken);
}

/* Реплика пользователя заданного размера (в токенах). */
Message user_turn(const std::string& tag, long long tokens) {
    Message m = Message::user(std::string(chars_of(tokens), 'u'));
    m.id = "msg_user_" + tag;
    return m;
}

/* Ответ ассистента заданного размера (в токенах). */
Message answer_turn(const std::string& parent, const std::string& tag,
                    long long tokens) {
    Message m = Message::assistant(parent);
    m.id = "msg_assist_" + tag;
    m.parts.push_back(MessagePart::text(std::string(chars_of(tokens), 'a')));
    return m;
}

/* Ход целиком: реплика пользователя (1 токен) + ответ (tokens-1).
 * Пользовательская часть крошечная намеренно: так у хода почти весь
 * объём — в ответе, и разрез внутри хода (splitTurn) не может «спасти»
 * ход целиком там, где тест этого не ждёт. */
std::vector<Message> turn_of(const std::string& tag, long long tokens) {
    std::vector<Message> h;
    h.push_back(user_turn(tag, 1));
    h.push_back(answer_turn("msg_user_" + tag, tag, tokens - 1));
    return h;
}

void append_turn(std::vector<Message>& h, const std::string& tag,
                 long long tokens) {
    const std::vector<Message> t = turn_of(tag, tokens);
    h.insert(h.end(), t.begin(), t.end());
}

/* Сколько ходов. */
std::vector<Message> history_of_turns(int count, long long tokens_per_turn) {
    std::vector<Message> h;
    for (int i = 0; i < count; ++i) {
        append_turn(h, std::to_string(i), tokens_per_turn);
    }
    return h;
}

/* Модель с порогом usable ровно в tokens. */
ModelLimits limits_with_usable(long long usable_tokens) {
    ModelLimits l;
    /* Лимита входа нет, поэтому usable = context - max_output. */
    l.context = usable_tokens + 1000;
    l.max_output = 1000;
    return l;
}

/* Настройка «хвост ровно на N токенов», лимиты такие, что сжимать есть
 * что: usable заведомо больше хвоста. */
CompactionConfig keep_n_tokens(long long tokens) {
    CompactionConfig cfg;
    cfg.preserve_recent_tokens = tokens;
    return cfg;
}

const char* first_id(const std::vector<Message>& v) {
    return v.empty() ? "" : v.front().id.c_str();
}

const char* last_id(const std::vector<Message>& v) {
    return v.empty() ? "" : v.back().id.c_str();
}

} // anonymous namespace

TEST(preserve_recent_budget_is_a_quarter_of_usable_within_bounds) {
    CompactionConfig cfg;
    /* Четверть, но не меньше 2000 и не больше 15000 (порт). */
    ASSERT_EQ(preserve_recent_budget(limits_with_usable(8000), cfg), 2000LL);
    ASSERT_EQ(preserve_recent_budget(limits_with_usable(32000), cfg), 8000LL);
    ASSERT_EQ(preserve_recent_budget(limits_with_usable(100000), cfg), 15000LL);
    /* Лимиты неизвестны — usable = 0, и берётся нижняя граница: хвост
     * всё равно нужен, иначе модель получит сводку без единой своей
     * мысли. */
    ASSERT_EQ(preserve_recent_budget(ModelLimits(), cfg), 2000LL);
    /* Настройка важнее умолчания и берётся как есть, даже вне границ:
     * clamp в порте относится к умолчанию, а не к решению пользователя. */
    cfg.preserve_recent_tokens = 500;
    ASSERT_EQ(preserve_recent_budget(limits_with_usable(100000), cfg), 500LL);
    cfg.preserve_recent_tokens = 40000;
    ASSERT_EQ(preserve_recent_budget(limits_with_usable(32000), cfg), 40000LL);
}

TEST(a_history_that_fits_needs_no_compaction) {
    /* Всё умещается — сжимать нечего, и ответ обязан быть «нечего», а не
     * «удалите всё, пустое у меня». Ровно это отличает пустой хвост от
     * «хвост не поместился»: в первом случае вызывающий не должен
     * суммировать ничего, во втором — обязан всё. */
    const std::vector<Message> h = history_of_turns(3, 40);
    const CompactionConfig cfg;
    const Selection sel = select_to_compact(h, limits_with_usable(100000), cfg);
    ASSERT_TRUE(sel.tail.empty());
    ASSERT_EQ(sel.head.size(), h.size());
    ASSERT_EQ(sel.tail_tokens, 0LL);
}

TEST(recent_turns_are_kept_whole_while_they_fit) {
    /* Порог на два последних хода: свежие уцелеют целиком, всё
     * остальное уйдёт в сводку. Хвост обязан быть НЕПРЕРЫВНЫМ и
     * приходить к финалу истории. */
    const std::vector<Message> h = history_of_turns(5, 1000);
    const Selection sel =
        select_to_compact(h, limits_with_usable(200000), keep_n_tokens(2500));
    ASSERT_EQ(sel.tail.size(), (size_t)4);             /* ходы 3 и 4 */
    ASSERT_EQ(std::string(first_id(sel.tail)), std::string("msg_user_3"));
    ASSERT_EQ(std::string(last_id(sel.tail)), std::string("msg_assist_4"));
    ASSERT_EQ(sel.head.size(), (size_t)6);             /* ходы 0..2 */
    ASSERT_EQ(std::string(last_id(sel.head)), std::string("msg_assist_2"));
    ASSERT_EQ(sel.tail_tokens, 2000LL);
}

TEST(an_oversized_turn_goes_to_the_summary_and_the_newer_ones_survive) {
    /* Ход 1 велик, ходы 0 и 2 малы, порог на один малый ход. Обход от
     * новых к старым: ход 2 влез, ход 1 — нет (и его собственный хвост
     * не влезает тоже), в сводку уходит всё до хода 2.
     *
     * Именно поэтому перебор обрывается на первом неудачном ходе, а не
     * идёт дальше: иначе в хвост попал бы СТАРЫЙ малый ход 0, а свежий
     * ход 1 между ним и началом — нет, и хвост перестал бы быть хвостом
     * (в нём был бы пропуск). */
    std::vector<Message> h;
    append_turn(h, "0", 40);
    append_turn(h, "1", 4000);
    append_turn(h, "2", 40);
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(100));
    ASSERT_EQ(sel.tail.size(), (size_t)2);
    ASSERT_EQ(std::string(first_id(sel.tail)), std::string("msg_user_2"));
    ASSERT_EQ(sel.head.size(), (size_t)4);
    ASSERT_EQ(std::string(last_id(sel.head)), std::string("msg_assist_1"));
}

TEST(an_oversized_turn_keeps_its_own_suffix_when_it_fits) {
    /* Ход не влезает целиком, но его ответ влезает: уцелеет кусок ХОДА,
     * а не только более новые ходы. Порт зовёт это splitTurn, и без
     * него модель теряла бы последнее, что делала. */
    std::vector<Message> h;
    h.push_back(user_turn("0", 1));
    h.push_back(answer_turn("msg_user_0", "0", 50));
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(50));
    ASSERT_EQ(sel.tail.size(), (size_t)1);
    ASSERT_EQ(std::string(first_id(sel.tail)), std::string("msg_assist_0"));
    ASSERT_EQ(sel.head.size(), (size_t)1);
    ASSERT_EQ(std::string(first_id(sel.head)), std::string("msg_user_0"));
}

TEST(the_split_keeps_the_longest_piece_that_fits) {
    /* Ход из трёх сообщений, порог — ровно на два последних. Уцелеть
     * должен самый ДЛИННЫЙ подошедший кусок: перебор идёт от начала хода
     * и возвращается на первом подошедшем — если бы возвращался на
     * последнем, от хода остался бы огрызок вместо сути. */
    std::vector<Message> h;
    h.push_back(user_turn("0", 1));
    h.push_back(answer_turn("msg_user_0", "mid", 50));
    h.push_back(answer_turn("msg_user_0", "last", 10));
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(60));
    ASSERT_EQ(sel.tail.size(), (size_t)2);
    ASSERT_EQ(std::string(first_id(sel.tail)), std::string("msg_assist_mid"));
    ASSERT_EQ(std::string(last_id(sel.tail)), std::string("msg_assist_last"));
}

TEST(nothing_fitting_means_the_whole_history_is_compacted) {
    /* Даже свежий ход не влезает — тогда хвоста нет и сжимается ВСЁ.
     * Это не аварийный путь: оставить хвост и сжать голову — значит не
     * сделать ничего, то есть переполнение осталось бы тем же. */
    const std::vector<Message> h = history_of_turns(2, 4000);
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(10));
    ASSERT_TRUE(sel.tail.empty());
    ASSERT_EQ(sel.head.size(), h.size());
    /* Порог в Selection остаётся названным даже при пустом хвосте:
     * вызывающий показывает его в панели, и «хвост пуст» не должно
     * выглядеть как «порога нет». */
    ASSERT_EQ(sel.budget, 10LL);
}

TEST(tail_turns_limits_how_many_turns_may_survive) {
    const std::vector<Message> h = history_of_turns(4, 40);
    CompactionConfig cfg = keep_n_tokens(100000);
    cfg.tail_turns = 1;
    /* Ограничение важнее порога: ходов могло уцелеть все четыре, но
     * пользователь сказал «последний». */
    const Selection one = select_to_compact(h, limits_with_usable(200000), cfg);
    ASSERT_EQ(one.tail.size(), (size_t)2);
    ASSERT_EQ(std::string(first_id(one.tail)), std::string("msg_user_3"));
    cfg.tail_turns = 2;
    const Selection two = select_to_compact(h, limits_with_usable(200000), cfg);
    ASSERT_EQ(two.tail.size(), (size_t)4);
    ASSERT_EQ(std::string(first_id(two.tail)), std::string("msg_user_2"));
}

TEST(tail_turns_zero_means_no_tail_at_all) {
    /* Порог УМЕРЕННО: если бы он влезал целиком, «хвоста нет» и «уцелело
     * всё» давали бы одинаковый ответ, и проверка ничего не различала
     * бы (на этом споткнулась первая версия — мутация удаления раннего
     * выхода не ловилась ничем). Здесь в хвост при строгом чтении
     * поместился бы последний ход, и «не поместился ни один» доказывает
     * именно правило tail_turns = 0, а не случайность. */
    const std::vector<Message> h = history_of_turns(3, 4000);
    CompactionConfig cfg = keep_n_tokens(5000);
    cfg.tail_turns = 0;
    const Selection sel = select_to_compact(h, limits_with_usable(100000), cfg);
    ASSERT_TRUE(sel.tail.empty());
    ASSERT_EQ(sel.head.size(), h.size());
    /* Тот же порог без tail_turns = 0 оставил бы последний ход. */
    cfg.tail_turns = -1;
    const Selection without = select_to_compact(h, limits_with_usable(100000), cfg);
    ASSERT_EQ(without.tail.size(), (size_t)2);
    ASSERT_EQ(std::string(first_id(without.tail)), std::string("msg_user_2"));
}

TEST(only_a_user_reply_starts_a_turn) {
    /* История без реплик пользователя (или состоящая из одних ответов
     * агента) не имеет ходов, и жать в ней нечего: попытка «сжать
     * всё» стёрла бы ответы, не оставив ни хвоста, ни смысла. */
    std::vector<Message> h;
    h.push_back(answer_turn("msg_unknown", "a", 40));
    h.push_back(answer_turn("msg_unknown", "b", 40));
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(10));
    ASSERT_TRUE(sel.tail.empty());
    ASSERT_EQ(sel.head.size(), h.size());
}

TEST(a_compaction_wrapper_does_not_start_a_turn) {
    /* Реплика с частью compaction — это искусственная обёртка сводки от
     * прошлого сжатия (И7.8). Считать её началом хода нельзя: она и
     * есть результат прошлого сжатия, и второй раз её «сжатие» значило
     * бы пересобирать сводку из сводки. */
    std::vector<Message> h;
    Message wrapper = Message::user("сводка предыдущей части сессии");
    wrapper.id = "msg_user_wrapper";
    wrapper.parts.push_back(MessagePart::compaction("о чём шла речь", {}));
    h.push_back(wrapper);
    h.push_back(answer_turn("msg_user_wrapper", "w", 40));
    append_turn(h, "0", 40);
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(100));
    /* Обёртка уходит в сводку вместе со своим ответом, уцелел свежий ход. */
    ASSERT_EQ(sel.tail.size(), (size_t)2);
    ASSERT_EQ(std::string(first_id(sel.tail)), std::string("msg_user_0"));
    ASSERT_EQ(sel.head.size(), (size_t)2);
    ASSERT_EQ(std::string(first_id(sel.head)), std::string("msg_user_wrapper"));
}

TEST(a_reasoning_only_turn_costs_nothing) {
    /* Хвост измеряется по ТРАНСКРИПТУ, а размышление в него не входит.
     * Оценка по сырым частям объявила бы такой ход тяжёлым и выбросила
     * бы в сводку последнюю мысль модели — ровно то, чего хвост и нужен,
     * чтобы не допустить. */
    std::vector<Message> h;
    append_turn(h, "0", 2000);
    h.push_back(user_turn("1", 1));
    Message thinking = Message::assistant("msg_user_1");
    thinking.id = "msg_assist_thinking";
    thinking.parts.push_back(
        MessagePart::reasoning(std::string(chars_of(5000), 'r')));
    h.push_back(thinking);
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(500));
    ASSERT_EQ(sel.tail.size(), (size_t)2);
    ASSERT_EQ(std::string(first_id(sel.tail)), std::string("msg_user_1"));
    /* 5 000 токенов размышления в хвост не попали: по транскрипту этот
     * ход занимает один токен, и он уместился. */
    ASSERT_TRUE(sel.tail_tokens < 100);
    ASSERT_EQ(sel.head.size(), (size_t)2);
}

/* ======================================================================
 * И7.4 — плоский транскрипт для агента-сводщика
 * ======================================================================
 *
 * Проверяется не «строки получились», а то, что сводщик получит всё
 * нужное и не получит лишнего: каждый вид части на своей строке, вызов
 * инструмента СОБСТВЕННЫМ текстом модели, результат усечён по границе
 * символа, служебные части молчат. */
namespace {

Message user_with_text(const std::string& tag, const std::string& body) {
    Message m = Message::user(body);
    m.id = "msg_user_" + tag;
    return m;
}

Message assistant_with(const std::string& tag, MessagePart part) {
    Message m = Message::assistant("msg_user_prev");
    m.id = "msg_assist_" + tag;
    m.parts.push_back(std::move(part));
    return m;
}

MessagePart completed_tool(const std::string& name, const std::string& body) {
    MessagePart p = MessagePart::tool("call_1", name);
    ToolOutput out;
    out.title = name;
    out.output = body;
    p.set_result(out);
    return p;
}

bool has_line(const std::string& text, const std::string& line) {
    return text.find("\n" + line + "\n") != std::string::npos ||
           text.rfind(line, 0) == 0 || text.find("\n" + line) != std::string::npos;
}

size_t count_lines(const std::string& text) {
    size_t n = text.empty() ? 0 : 1;
    for (char c : text) {
        if (c == '\n') ++n;
    }
    return n;
}

} // anonymous namespace

TEST(summary_transcript_names_every_kind_of_line) {
    std::vector<Message> h;
    h.push_back(user_with_text("u", "почини тест"));
    h.push_back(assistant_with("t", MessagePart::text("Смотрю.")));
    h.push_back(assistant_with("r", MessagePart::reasoning("Кажется, дело в замке.")));
    h.push_back(assistant_with("c", completed_tool("read_file", "содержимое")));
    MessagePart failed = MessagePart::tool("call_2", "bash");
    failed.set_error("sudo: отказано в доступе");
    h.push_back(assistant_with("e", failed));
    const std::string text = serialize_for_summary(h);

    ASSERT_TRUE(has_line(text, "[User]: почини тест"));
    ASSERT_TRUE(has_line(text, "[Assistant]: Смотрю."));
    /* Размышление в сводке ОБЯЗАТЕЛЬНО: без него теряется ход
     * рассуждений. В транскрипте для модели его нет (И5.4) — и здесь его
     * тоже легко потерять по ошибке, копируя тудашний код. */
    ASSERT_TRUE(has_line(text, "[Assistant reasoning]: Кажется, дело в замке."));
    ASSERT_TRUE(has_line(text, "[Tool result]: содержимое"));
    ASSERT_TRUE(has_line(text, "[Tool error]: sudo: отказано в доступе"));
    /* Структура: пять сообщений — пять блоков, внутри них семь строк
     * (вызов и его исход считаются двумя). Проверяются ОБА счёта: один
     * без другого не говорит, ни где кончился блок, ни что внутри него. */
    size_t blocks = 1;
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == '\n' && text[i + 1] == '\n') ++blocks;
    }
    ASSERT_EQ(blocks, (size_t)5);
    size_t content_lines = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string line =
            text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty()) ++content_lines;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    ASSERT_EQ(content_lines, (size_t)7);
}

TEST(summary_transcript_keeps_the_models_own_tool_call) {
    /* Блок вызова — это текст самой модели; собирать его заново из
     * аргументов здесь нельзя (это второй протокол вызова, класс D2). */
    std::vector<Message> h;
    MessagePart raw = MessagePart::tool(
        "call_1", "read_file", json::JsonValue::object(),
        "```json\n{\"tool\": \"read_file\", \"path\": \"a.cpp\"}\n```");
    h.push_back(assistant_with("c", raw));
    const std::string text = serialize_for_summary(h);
    ASSERT_TRUE(text.find("[Assistant tool call]: ```json") != std::string::npos);
    ASSERT_TRUE(text.find("a.cpp") != std::string::npos);
}

TEST(summary_transcript_caps_a_long_tool_result) {
    std::vector<Message> h;
    h.push_back(assistant_with("c", completed_tool("bash", std::string(5000, 'x'))));
    const std::string text = serialize_for_summary(h);
    /* Метка обрезания обязана быть: без неё сводщик принял бы огрызок за
     * весь вывод и записал в сводку выводы о том, чего не было. */
    ASSERT_TRUE(text.find("[обрезано, всего 5000 символов]") != std::string::npos);
    ASSERT_TRUE(text.find("x") != std::string::npos);
    /* Обрезанный результат — меньше 2600 символов строки, а не 5000. */
    ASSERT_TRUE(text.size() < (size_t)2600);
}

TEST(summary_transcript_never_splits_a_codepoint) {
    /* Кириллица: символ три байта. Режем посередине — в промпте сводщика
     * появится битый байт, который портит текст тихо и без сообщения. */
    std::vector<Message> h;
    h.push_back(assistant_with("c",
        completed_tool("read_file", std::string(3000, 'я'))));
    /* Переменная названа out, а не text: локальный text:: закрыл бы
     * одноимённое пространство имён. */
    const std::string out = serialize_for_summary(h);
    ASSERT_TRUE(coder::text::is_valid_utf8(out));
}

TEST(summary_transcript_skips_service_parts) {
    /* Служебные виды не несут содержания, а формат для них не описан ни
     * здесь, ни в промпте сводщика: молчащая строка честнее строки,
     * которую тот не знает. */
    std::vector<Message> h;
    h.push_back(assistant_with("s", MessagePart::step_start("план")));
    h.push_back(assistant_with("f", MessagePart::step_finish("план")));
    h.push_back(assistant_with("r", MessagePart::retry(2, 1500)));
    h.push_back(assistant_with("p", MessagePart::patch("abc123")));
    h.push_back(assistant_with("t", MessagePart::subtask("ses_1", "wp_explore")));
    h.push_back(assistant_with("x", MessagePart::text("Видимое.")));
    const std::string text = serialize_for_summary(h);
    ASSERT_EQ(count_lines(text), (size_t)1);
    ASSERT_TRUE(has_line(text, "[Assistant]: Видимое."));
}

TEST(summary_transcript_marker_follows_the_role) {
    /* Один и тот же текст в реплике пользователя и в ответе ассистента —
     * разные строки: перепутать их — значит приписать модели слова
     * пользователя, и сводка запишет их как решение агента. */
    std::vector<Message> h;
    h.push_back(user_with_text("u", "одно и то же"));
    h.push_back(assistant_with("a", MessagePart::text("одно и то же")));
    const std::string text = serialize_for_summary(h);
    ASSERT_TRUE(has_line(text, "[User]: одно и то же"));
    ASSERT_TRUE(has_line(text, "[Assistant]: одно и то же"));
    ASSERT_TRUE(text.find("[User]: [Assistant]:") == std::string::npos);
}

TEST(summary_transcript_carries_the_previous_summary) {
    /* Обёртка сводки от прошлого сжатия обязана попасть в новую сводку:
     * иначе повторное сжатие забыло бы всё, что было до него, а именно
     * ради этого сжатие и делается. */
    std::vector<Message> h;
    Message wrapper = Message::user("сводка прошлого раза");
    wrapper.parts.push_back(
        MessagePart::compaction("искали причину падения теста", {}));
    h.push_back(wrapper);
    h.push_back(assistant_with("a", MessagePart::text("нашёл")));
    const std::string text = serialize_for_summary(h);
    ASSERT_TRUE(has_line(text, "[User]: искали причину падения теста"));
    ASSERT_TRUE(has_line(text, "[Assistant]: нашёл"));
}

TEST(summary_transcript_has_no_holes_for_empty_messages) {
    /* Пустое сообщение (реплика без частей) не должно оставлять ни строки,
     * ни пустого разделителя: дыра в промпте сводщика выглядит как
     * «здесь был ход, о котором забыли».
     *
     * Пустое сообщение стоит В СЕРЕДИНЕ, а не в начале: в начале оно не
     * оставляет следка даже при сломанной проверке (разделитель ставится
     * только перед непустым блоком, а блока нет) — и проверка проходила бы
     * при удалённой `if (lines.empty()) continue;`. Именно так она и
     * выглядела в первой версии. */
    std::vector<Message> h;
    Message blank;
    blank.role = kRoleUser;
    blank.id = "msg_user_blank";
    h.push_back(user_with_text("a", "первая задача"));
    h.push_back(blank);
    h.push_back(user_with_text("b", "вторая задача"));
    const std::string text = serialize_for_summary(h);
    ASSERT_EQ(text, std::string("[User]: первая задача\n\n[User]: вторая задача"));
}

/* ======================================================================
 * И7.5 — агент-сводщик
 * ======================================================================
 *
 * Главная проверка здесь механическая и ровно та, ради которой писался
 * 7.4: КАЖДАЯ строка формата транскрипта названа в промпте сводщика. Сводщик
 * получает только этот текст, поэтому необъявленная строка для него —
 * шум, а названная строка, которой нет, — обещание. Проверка сравнивает
 * списки, а не ищет подстроку: иначе одна объявленная метка удовлетворила
 * бы проверку за все шесть. */

/* Строки, которые даёт serialize_for_summary (И7.4). Список — тот же
 * источник, что и сам транскрипт, и он проверяется таблицей, а не
 * глазами. */
const char* kFormatMarkers[] = {
    "[User]:", "[Assistant]:", "[Assistant reasoning]:",
    "[Assistant tool call]:", "[Tool result]:", "[Tool error]:",
};

TEST(the_summarizer_prompt_names_every_line_format) {
    const std::string sys = kCompactionSystemPrompt;
    for (const char* marker : kFormatMarkers) {
        if (sys.find(marker) == std::string::npos) {
            std::cerr << "  строка формата " << marker << " не названа в промпте"
                      << " сводщика. Сводщик получает только этот текст, и"
                      << " необъявленная строка для него — шум. Добавь её в"
                      << " kCompactionSystemPrompt (core/prompts.h)."
                      << std::endl;
        }
        ASSERT_TRUE(sys.find(marker) != std::string::npos);
    }
    /* Обратная проверка: сводщику нельзя объяснять протокол вызовов, его
     * потом пришлось бы отклонять, а токены на попытку тратятся зря. */
    ASSERT_TRUE(sys.find("ПРОТОКОЛ ВЫЗОВА") == std::string::npos);
    ASSERT_TRUE(sys.find("НЕ вызываешь инструменты") != std::string::npos);
    /* Слово-выход объявлено вместе со смыслом. */
    ASSERT_TRUE(sys.find("compact") != std::string::npos);
}

TEST(the_summarizer_prompt_separates_the_previous_summary) {
    const std::string first = compaction_user_prompt("[User]: задача", "");
    ASSERT_TRUE(first.find("<conversation>") != std::string::npos);
    ASSERT_TRUE(first.find("задача") != std::string::npos);
    /* Без прошлой сводки тега быть не должно: пустой тег читается как
     * «сводка была, но потерялась». */
    ASSERT_TRUE(first.find("<prior-summary>") == std::string::npos);

    const std::string again = compaction_user_prompt("[User]: продолжение",
                                                     "## Цель\n- было");
    /* Именно ОБЁРТКА, а не «тег встречается в тексте»: слово
     * <prior-summary> есть и в инструкции («переноси из <prior-summary>
     * цели...»), и проверка на вхождение удовлетворялась инструкцией —
     * то есть проходила бы и с выкинутой обёрткой. Проверяется, что
     * сводка лежит МЕЖДУ открывающим и закрывающим тегом. */
    const size_t open = again.find("<prior-summary>\n");
    const size_t close = again.find("\n</prior-summary>");
    ASSERT_TRUE(open != std::string::npos);
    ASSERT_TRUE(close != std::string::npos);
    ASSERT_TRUE(close > open);
    const size_t summary_at = again.find("## Цель\n- было", open);
    ASSERT_TRUE(summary_at != std::string::npos);
    ASSERT_TRUE(summary_at > open);
    ASSERT_TRUE(summary_at < close);
    /* Порядок закреплён: разговор новее сводки, и об этом сказано словами,
     * потому что именно на этом сводщик и спотыкается. */
    ASSERT_TRUE(again.find("<conversation>") < open);
}

TEST(the_answer_becomes_the_summary) {
    SummaryTurn turn = [](const std::string&, const std::vector<ModelMessage>&,
                          std::string& text, std::string&) {
        text = "## Цель\n- починить тест";
        return true;
    };
    const CompactionResult r = summarize("[User]: задача", "", turn);
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Continue);
    ASSERT_EQ(r.summary, std::string("## Цель\n- починить тест"));
    ASSERT_EQ(r.reason, std::string(""));
    ASSERT_EQ((int)r.failure, (int)FailureKind::None);
    ASSERT_FALSE(r.summary_truncated);
    /* Сводщик получает ровно одну реплику и промпт сводщика — не
     * системный промпт агента. */
    ASSERT_EQ(std::string(compaction_outcome_name(r.outcome)),
              std::string("продолжаем"));
}

TEST(answering_compact_means_the_context_is_exhausted) {
    /* Слово-выход: место кончилось даже под сводку. Это НЕ сбой
     * провайдера — сеть работает, запрос ушёл, — и повторять попытку
     * бессмысленно, поэтому вид сбоя свой. */
    for (const char* word : {"compact", "COMPACT", "  compact  ", "Compact\n"}) {
        SummaryTurn turn = [word](const std::string&,
                                  const std::vector<ModelMessage>&, std::string& t,
                                  std::string&) {
            t = word;
            return true;
        };
        const CompactionResult r = summarize("[User]: задача", "", turn);
        ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Compact);
        ASSERT_EQ((int)r.failure, (int)FailureKind::ContextOverflow);
        ASSERT_TRUE(r.summary.empty());
    }
    /* Названный иначе, чем «провайдер»: иначе UI показал бы «ошибка
     * сети» там, где сеть работает. */
    ASSERT_TRUE(std::string(failure_kind_name(FailureKind::ContextOverflow)) !=
                std::string(failure_kind_name(FailureKind::Provider)));
}

TEST(a_failed_turn_stops_without_touching_the_history) {
    SummaryTurn turn = [](const std::string&, const std::vector<ModelMessage>&,
                          std::string&, std::string& error) {
        error = "провайдер недоступен";
        return false;
    };
    const CompactionResult r = summarize("[User]: задача", "", turn);
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Stop);
    ASSERT_EQ((int)r.failure, (int)FailureKind::Provider);
    /* Причина обязана быть видна: молчаливый отказ от сжатия выглядит как
     * «сжатия не было вовсе», и переполнение приходит снова. */
    ASSERT_TRUE(r.reason.find("провайдер недоступен") != std::string::npos);
    ASSERT_TRUE(r.summary.empty());
}

TEST(a_summarizer_calling_a_tool_is_denied_by_name) {
    /* Запрет на инструменты механический: у сводщика нет запускателя, и
     * блок вызова останется в тексте, то есть попадёт в сводку и станет
     * выдумкой, будто инструмент работал. */
    SummaryTurn turn = [](const std::string&, const std::vector<ModelMessage>&,
                          std::string& t, std::string&) {
        t = "смотрю файл\n```json\n{\"tool\": \"read_file\", \"path\": \"a.cpp\"}\n```";
        return true;
    };
    const CompactionResult r = summarize("[User]: задача", "", turn);
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Stop);
    ASSERT_EQ((int)r.failure, (int)FailureKind::Tool);
    ASSERT_TRUE(r.reason.find("read_file") != std::string::npos);
    ASSERT_TRUE(r.reason.find("нет инструментов") != std::string::npos);
    ASSERT_TRUE(r.summary.empty());
}

TEST(an_empty_answer_stops_instead_of_becoming_a_summary) {
    for (const char* answer : {"", "   ", "\n\n"}) {
        SummaryTurn turn = [answer](const std::string&,
                                    const std::vector<ModelMessage>&, std::string& t,
                                    std::string&) {
            t = answer;
            return true;
        };
        const CompactionResult r = summarize("[User]: задача", "", turn);
        ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Stop);
        ASSERT_TRUE(r.reason.find("пустой") != std::string::npos);
        ASSERT_TRUE(r.summary.empty());
    }
}

TEST(a_too_long_summary_is_cut_at_a_line_and_flagged) {
    /* Предел длины держится, потому что сводка длиннее неё съела бы
     * ровно то место, ради которого её составили. Обрезка — по границе
     * строки, и флаг обязателен: молчаливая обрезка выглядит как целая
     * сводка. */
    const std::string line = std::string(500, 'a');
    std::string huge;
    while (huge.size() < static_cast<size_t>(limits::kCompactionSummaryMaxChars) + 5000) {
        huge += line + "\n";
    }
    SummaryTurn turn = [&huge](const std::string&,
                               const std::vector<ModelMessage>&, std::string& t,
                               std::string&) {
        t = huge;
        return true;
    };
    const CompactionResult r = summarize("[User]: задача", "", turn);
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Continue);
    ASSERT_TRUE(r.summary_truncated);
    ASSERT_TRUE(!r.reason.empty());
    ASSERT_TRUE(r.summary.size() <= static_cast<size_t>(limits::kCompactionSummaryMaxChars));
    /* Обрезано по границе строки, а не посреди неё. */
    ASSERT_TRUE(huge.compare(0, r.summary.size(), r.summary) == 0);
    ASSERT_TRUE(coder::text::is_valid_utf8(r.summary));
}

TEST(no_summarizer_means_stop_not_a_crash) {
    /* Пустая функция — не «сработает по умолчанию», а отказ с причиной:
     * вызывающий обязан узнать, что сводщика нечем кормить, а не упасть. */
    const CompactionResult r = summarize("[User]: задача", "", nullptr);
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Stop);
    ASSERT_TRUE(r.reason.find("недоступен") != std::string::npos);
}

TEST(nothing_to_summarize_is_a_stop_with_a_reason) {
    /* Пустой разговор: Continue означал бы, что работа продолжается ни на
     * чём, а пустая сводка хуже её отсутствия. */
    SummaryTurn turn = [](const std::string&, const std::vector<ModelMessage>&,
                          std::string& t, std::string&) {
        t = "сводка";
        return true;
    };
    const CompactionResult r = summarize("", "", turn);
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Stop);
    ASSERT_TRUE(r.reason.find("нечего сжимать") != std::string::npos);
    ASSERT_TRUE(r.summary.empty());
}

TEST(compaction_selection_reads_its_settings) {
    LimitsGuard guard;
    {
        std::map<std::string, std::string> v;
        v["wp_coder.context_limit"] = "128000";
        v["wp_coder.compaction_tail_turns"] = "2";
        v["wp_coder.compaction_preserve_recent_tokens"] = "6000";
        HostCallbacks cb = settings_host(v);
        engine().init(cb);
        engine().load_settings();
    }
    ASSERT_EQ(engine_state().compaction_config.tail_turns, 2LL);
    ASSERT_EQ(engine_state().compaction_config.preserve_recent_tokens, 6000LL);
    /* tail_turns = 0 обязан пережить разбор настроек: это «сжимать всё»,
     * и если бы ноль читался как «не задано», настройка была бы
     * недоступна вовсе. */
    {
        std::map<std::string, std::string> v;
        v["wp_coder.compaction_tail_turns"] = "0";
        HostCallbacks cb = settings_host(v);
        engine().init(cb);
        engine().load_settings();
    }
    ASSERT_EQ(engine_state().compaction_config.tail_turns, 0LL);
    /* Мусор → «не задано» (-1), а не 0: иначе мусор в настройке молча
     * означал бы «сжимать всё». */
    {
        std::map<std::string, std::string> v;
        v["wp_coder.compaction_tail_turns"] = "сколько угодно";
        HostCallbacks cb = settings_host(v);
        engine().init(cb);
        engine().load_settings();
    }
    ASSERT_EQ(engine_state().compaction_config.tail_turns, -1LL);
}

TEST(no_limits_at_all_mean_no_compaction_and_a_named_panel) {
    /* Чистое состояние: настроек нет. Панель обязана говорить об этом
     * словами, а не показывать выдуманный порог. */
    const ModelLimits none;
    CompactionConfig cfg;
    ASSERT_EQ(usable(none, cfg), 0LL);
    ASSERT_FALSE(is_overflow(none, cfg, 1000000000LL));
    ASSERT_EQ(std::string(token_source_name(TokenSource::Estimated)),
              std::string("оценка"));
}

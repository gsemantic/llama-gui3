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
#include "core/session_store.h"   /* round-trip порядка (И7.8) */

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

/* Обёртка сводки от прошлого сжатия — во что сжимается старая голова
 * истории. Живёт здесь, а не рядом с И7.6: пользуются и транскрипт
 * (эта задача), и сводщик (следующая). */
Message summary_wrapper(const std::string& tag, const std::string& summary) {
    Message m = Message::user(std::string());
    m.id = "msg_user_" + tag;
    m.parts.push_back(MessagePart::compaction(summary, {}));
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

TEST(the_previous_summary_is_not_part_of_the_conversation) {
    /* И7.4 сначала требовала обратного: строка `[User]: <сводка>` в
     * транскрипте. И7.6 завела прошлой сводке ОТДЕЛЬНУЮ полосу
     * (<prior-summary>), и держать её ещё и в разговоре нельзя: там она
     * читалась бы словами человека, а промпт одновременно просит считать
     * разговор новее сводки. Два утверждения об одном тексте, и
     * выигрывает более свежее — то есть предписывающее отбросить старое.
     *
     * Проверка именно ОТСУТСТВИЯ строки, а не «сводки нет в выводе вообще»:
     * сам текст обязан дойти до сводщика, и проверяет это уже тест
     * the_previous_summary_reaches_the_new_summarizer (И7.6). */
    std::vector<Message> h;
    h.push_back(summary_wrapper("c", "искали причину падения теста"));
    h.push_back(assistant_with("a", MessagePart::text("нашёл")));
    const std::string text = serialize_for_summary(h);
    ASSERT_TRUE(text.find("искали причину падения теста") == std::string::npos);
    /* Сообщение-обёртка не оставляет и пустой дыры: блок целиком пуст, и
     * разделитель между сообщениями не ставится. */
    ASSERT_EQ(text, std::string("[Assistant]: нашёл"));
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

/* Содержимое между двумя метками. Пустая строка — метки нет либо текста
 * между ними нет, и различать это проверка обязана сама (иначе
 * «тег есть, но сводка не доехала» прошло бы как успех). */
std::string between(const std::string& text, const std::string& open,
                    const std::string& close) {
    const size_t a = text.find(open + "\n");
    if (a == std::string::npos) return std::string();
    const size_t b = text.find("\n" + close, a);
    if (b == std::string::npos) return std::string();
    return text.substr(a + open.size() + 1, b - (a + open.size() + 1));
}

/* Сводщик-заглушка, который ЗАПОМИНАЕТ то, что ему передали. Проверять
 * доставку прошлой сводки иначе нечем: наружу summarize() отдаёт только
 * итог, и «сводщик получил» — это утверждение о том, чего в результате
 * нет. */
struct CapturingTurn {
    std::string system;
    std::string user_prompt;
    int replies = 0;
    int messages = 0;
    /* Была ли прошлая сводка и какая (И7.11: итеративность целиком). */
    bool calls_had_prior = false;
    std::string prior_text;
    std::string answer = "## Цель\n- починить тест";

    SummaryTurn fn() {
        CapturingTurn* self = this;
        return [self](const std::string& sys, const std::vector<ModelMessage>& m,
                      std::string& text, std::string&) {
            ++self->replies;
            self->messages = static_cast<int>(m.size());
            self->system = sys;
            self->prior_text = between(self->user_prompt + "\n\n" +
                                           (m.empty() ? "" : m[0].content),
                                       "<prior-summary>", "</prior-summary>");
            self->calls_had_prior = m.size() == 1 &&
                                    m[0].content.find("<prior-summary>") !=
                                        std::string::npos;
            self->user_prompt.clear();
            for (const ModelMessage& mm : m) {
                if (!self->user_prompt.empty()) self->user_prompt += "\n\n";
                self->user_prompt += mm.content;
            }
            text = self->answer;
            return true;
        };
    }
};

/* Разговор без следов прошлого сжатия. */
std::vector<Message> plain_history() {
    std::vector<Message> h;
    h.push_back(user_with_text("u", "почини тест"));
    h.push_back(assistant_with("a", MessagePart::text("смотрю")));
    return h;
}


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

/* ======================================================================
 * И7.6 — итеративность: прошлая сводка доезжает до нового сводщика
 * ======================================================================
 *
 * Главная проверка здесь смотрит на ТО, ЧТО ПЕРЕДАНО СВОДЩИКУ, а не на
 * результат summarize(): результат одинаков — сводка — независимо от
 * того, видел сводщик прошлую сводку или нет. Проверка по результату
 * была бы зелёной ровно на том дефекте, ради которого задача и
 * существует: второе сжатие выкинуло бы всю работу до первого, сводка
 * получилась бы правдоподобной, и заметить это было бы негде. */

TEST(the_previous_summary_reaches_the_new_summarizer) {
    /* Повторное сжатие: в голове истории лежит обёртка сводки от
     * прошлого раза, а разговор — только то, что было после неё. */
    std::vector<Message> h;
    h.push_back(summary_wrapper("c", "искали причину падения теста"));
    h.push_back(user_with_text("u", "тест всё ещё падает"));
    h.push_back(assistant_with("a", MessagePart::text("смотрю git log")));

    CapturingTurn cap;
    const CompactionResult r = summarize(h, cap.fn());
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Continue);
    ASSERT_EQ(cap.replies, 1);
    /* Промпт сводщика — не системный промпт агента, и реплика ровно одна.
     * Обещание из комментария к the_answer_becomes_the_summary («сводщик
     * получает ровно одну реплику и промпт сводщика») там проверялось
     * ничем: тест смотрел на исход, а не на то, что ушло в модель. */
    ASSERT_EQ(cap.system, std::string(kCompactionSystemPrompt));
    ASSERT_EQ(cap.messages, 1);
    /* Прошлая сводка лежит МЕЖДУ метками, а не «где-то в промпте»: слово
     * <prior-summary> есть и в инструкции, и проверка на вхождение
     * удовлетворялась бы инструкцией. */
    ASSERT_EQ(between(cap.user_prompt, "<prior-summary>", "</prior-summary>"),
              std::string("искали причину падения теста"));
    /* И — главное — в разговоре её НЕТ: дважды отданная сводка с
     * разными пометками («реплика человека» против «сводка прошлого
     * разговора») дают сводщику два утверждения об одном тексте. */
    ASSERT_EQ(between(cap.user_prompt, "<conversation>", "</conversation>"),
              std::string("[User]: тест всё ещё падает\n"
                          "\n"
                          "[Assistant]: смотрю git log"));
    /* Разговор новее сводки, и это сказано словами: именно на порядке
     * сводщик спотыкается. */
    ASSERT_TRUE(cap.user_prompt.find("<conversation>") <
                cap.user_prompt.find("<prior-summary>"));
}

TEST(the_first_compaction_has_no_prior_summary_tag) {
    /* Пустой тег читается как «сводка была, но потерялась», поэтому его
     * не должно быть вовсе — и тем более в виде пары меток с пустым
     * содержимым. */
    CapturingTurn cap;
    const CompactionResult r = summarize(plain_history(), cap.fn());
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Continue);
    ASSERT_TRUE(cap.user_prompt.find("<prior-summary>") == std::string::npos);
    ASSERT_TRUE(cap.user_prompt.find("<conversation>") != std::string::npos);
}

TEST(the_newest_summary_replaces_the_older_one) {
    /* Две сводки подряд: вторая составлена с первой в <prior-summary> и
     * по объявленному правилу слияния содержит её целиком. Отдать сводщику
     * ПЕРВУЮ = выбросить всё, сделанное между сжатиями, причём молча:
     * промпт отработал бы и выдал правдоподобную сводку. */
    std::vector<Message> h;
    h.push_back(summary_wrapper("first", "чинили дедлок в SessionStore"));
    h.push_back(summary_wrapper("second",
        "## Цель\n- починить тест\n\n## Сделано\n- дедлок устранён"));
    h.push_back(user_with_text("u", "теперь падает другое"));

    CapturingTurn cap;
    const CompactionResult r = summarize(h, cap.fn());
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Continue);
    const std::string prior =
        between(cap.user_prompt, "<prior-summary>", "</prior-summary>");
    ASSERT_TRUE(prior.find("теперь падает другое") == std::string::npos);
    ASSERT_TRUE(prior.find("дедлок устранён") != std::string::npos);
    /* Старая сводка не всплывает и в разговоре — иначе модель получила бы
     * «человека, который говорил про дедлок» впереди собственной цели. */
    ASSERT_TRUE(cap.user_prompt.find("чинили дедлок") == std::string::npos);
}

TEST(an_empty_summary_wrapper_does_not_hide_the_older_one) {
    /* Пустая обёртка получается только из правки файла сессии руками.
     * Считать её сводкой значило бы объявить «прошлого сжатия не было»,
     * то есть тихо потерять всё, что до него; всё сказанное после неё и
     * так лежит в разговоре. */
    std::vector<Message> h;
    h.push_back(summary_wrapper("first", "чинили дедлок"));
    h.push_back(summary_wrapper("broken", ""));
    h.push_back(user_with_text("u", "продолжаем"));

    CapturingTurn cap;
    const CompactionResult r = summarize(h, cap.fn());
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Continue);
    ASSERT_EQ(between(cap.user_prompt, "<prior-summary>", "</prior-summary>"),
              std::string("чинили дедлок"));
}

TEST(a_summary_left_alone_stops_and_says_why) {
    /* Переполнение, а новых ходов нет: история уже сведена до обёртки.
     * Повторное сжатие тут нечем делать, и объявлять Continue значило бы
     * «работа продолжается ни на чём». Отказ обязан быть С ДВУМЯ разными
     * причинами: «пустой разговор» и «после сжатия новых ходов нет»
     * выглядят для пользователя одинаково, а приводят к разному — во
     * втором случае переполнение вернётся на следующем шаге. */
    std::vector<Message> h;
    h.push_back(summary_wrapper("c", "всё, что было, уже в сводке"));
    CapturingTurn cap;
    const CompactionResult r = summarize(h, cap.fn());
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Stop);
    ASSERT_TRUE(r.reason.find("новых ходов нет") != std::string::npos);
    ASSERT_TRUE(r.summary.empty());
    /* Сводщик не зовётся вовсе: незачем платить за запрос, результат
     * которого всё равно пришлось бы выбросить. */
    ASSERT_EQ(cap.replies, 0);

    CapturingTurn empty;
    const CompactionResult e = summarize({}, empty.fn());
    ASSERT_TRUE(e.reason.find("разговор пуст") != std::string::npos);
}

TEST(the_previous_summary_is_read_from_the_history_not_the_caller) {
    /* Функция доступна и сама по себе — ею пользуется и summarize(), и
     * тот, кто будет показывать сводку в UI (И11). Значит, её поведение
     * проверяется самостоятельно, а не только через промпт. */
    std::vector<Message> h;
    h.push_back(summary_wrapper("a", "первая"));
    h.push_back(user_with_text("u", "между"));
    h.push_back(summary_wrapper("b", "вторая"));
    ASSERT_EQ(previous_summary(h), std::string("вторая"));
    ASSERT_EQ(previous_summary(plain_history()), std::string(""));
    ASSERT_EQ(previous_summary({}), std::string(""));
}

/* ======================================================================
 * И7.7 — автопродолжение
 * ======================================================================
 *
 * Реплика «Continue if you have next steps…» обязана попасть в ДВА места
 * и не попасть в третье, и каждое требование проверяется отдельно,
 * потому что все три выглядели бы одинаково, пока всё работает:
 *
 *   - транскрипт для МОДЕЛИ: обязана уйти, иначе после сжатия работа
 *     останавливается, а выглядит это как «сжатие прошло, задача
 *     закрыта» — потеря работы выглядит выполненной работой;
 *   - сводка для следующего сжатия: обязана НЕ уйти, иначе сводщик
 *     прочитает «человек попросил продолжать» (там `[User]` — это
 *     человек, и промпт прямо запрещает приписывать ему лишнего);
 *   - файл сессии: обязана сохраниться, иначе после перезагрузки она
 *     станет обычной репликой пользователя (проверяет
 *     session_file_roundtrip_keeps_every_part).
 *
 * Проверка «ушла» и «не ушла» в одном тесте — не экономия, а защита от
 * подмены: правка, убирающая часть из обоих мест разом, оставила бы
 * зелёным любой из двух тестов по отдельности. */

TEST(the_continuation_reaches_the_model_and_not_the_summarizer) {
    const Message resume = Message::compaction_continue();
    /* Роль пользователя — сознательное решение, а не деталь: от неё
     * зависят условие завершения хода (И5.8) и выбор хвоста при
     * следующем сжатии. Отдельная роль «от плагина» потребовала бы
     * учить её быть пользователем во всех местах, где пользователь
     * значит «начало хода». */
    ASSERT_TRUE(resume.is_user());
    ASSERT_EQ(resume.parts.size(), size_t(1));
    ASSERT_TRUE(resume.parts[0].is(PartKind::CompactionContinue));

    std::vector<Message> h;
    h.push_back(summary_wrapper("c", "сводка прошлого раза"));
    h.push_back(assistant_with("a", MessagePart::text("продолжаю")));
    h.push_back(resume);

    /* Модели уходит ровно текст константы: одна реплика пользователя,
     * дословно, без обёрток.
     *
     * Три реплики, а не две: обёртка сводки тоже уходит модели, отдельной
     * репликой ассистента — это работа 7.8, и здесь важно лишь, что она
     * стоит ДО продолжения, а не после. */
    const std::vector<ModelMessage> msgs = to_model_messages(h);
    ASSERT_EQ(msgs.size(), size_t(3));
    ASSERT_EQ(msgs[0].role, std::string(kRoleAssistant));
    ASSERT_EQ(msgs[0].content, std::string("сводка прошлого раза"));
    ASSERT_EQ(msgs[1].role, std::string(kRoleAssistant));
    ASSERT_EQ(msgs[2].role, std::string(kRoleUser));
    ASSERT_EQ(msgs[2].content, std::string(kCompactionContinueText));

    /* В сводку она не попадает: там `[User]` — это человек, а это слова
     * плагина. Проверка именно ОТСУТСТВИЯ, а не «сводка не испортилась»:
     * иначе правка, убирающая часть и из сводки, и из транскрипта,
     * прошла бы здесь, и потерялась бы работа после сжатия. */
    const std::string transcript = serialize_for_summary(h);
    ASSERT_TRUE(transcript.find("Continue if you have next steps") ==
                std::string::npos);
    ASSERT_TRUE(has_line(transcript, "[Assistant]: продолжаю"));
}

TEST(after_compaction_the_last_user_message_is_the_continuation) {
    /* От этого равенства зависит условие завершения хода: модель отвечает
     * на последнюю реплику пользователя, и без реплики-продолжения ход не
     * отвечает ни на что — задача не закрылась бы, но и не продолжилась,
     * а просто остановилась бы без внятной причины. */
    std::vector<Message> h;
    h.push_back(summary_wrapper("c", "сводка"));
    h.push_back(user_with_text("u", "старая задача"));
    const Message resume = Message::compaction_continue();
    h.push_back(resume);
    h.push_back(assistant_with("z", MessagePart::text("взялся")));
    const Message* last = last_user_message(h);
    ASSERT_TRUE(last != nullptr);
    ASSERT_EQ(last->id, resume.id);
}

TEST(the_tail_starts_where_the_work_resumed) {
    /* Хвост обязан начинаться с реплики-продолжения: всё, что после неё,
     * агент делает уже в сжатой сессии, и обрезать хвост раньше значило бы
     * оставить его работать без того, с чего он продолжил. Размеры — в
     * токенах (оценка chars/4), как и во всём блоке И7.3.
     *
     * Проверка не пустая ровно настолько, насколько не пуст порог: старый
     * ход на 8000 токенов в 1000 не влезает даже своим хвостом, поэтому
     * границу может задать только начало следующего хода.
     *
     * Проверка держит ГРАНИЦУ хвоста, а не «считается ли реплика
     * продолжения ходом»: мутация на второе не ломает ничего, потому что
     * разрез хода приходит к той же границе (см. комментарий в
     * starts_a_turn). Хвост, начинающийся с реплики продолжения, —
     * свойство, за которое отвечает выбор хвоста, и оно проверяемо. */
    std::vector<Message> h;
    h.push_back(summary_wrapper("c", "сводка"));
    h.push_back(user_turn("a", 4000));
    h.push_back(answer_turn("msg_user_a", "a", 4000));
    const Message resume = Message::compaction_continue();
    h.push_back(resume);
    h.push_back(answer_turn("msg_continue", "z", 40));

    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(1000));
    ASSERT_EQ(sel.tail.size(), size_t(2));
    ASSERT_EQ(std::string(first_id(sel.tail)), resume.id);
    ASSERT_EQ(std::string(last_id(sel.tail)), std::string("msg_assist_z"));
    /* Голова уходит в сводку вместе со старым ходом, а реплика
     * продолжения — нет: уйди она в сводку, работа возобновилась бы с
     * голой сводки и без повода продолжать. */
    ASSERT_EQ(sel.head.size(), size_t(3));
    ASSERT_EQ(std::string(first_id(sel.head)), std::string("msg_user_c"));
}

TEST(a_lone_continuation_is_not_a_conversation_to_summarize) {
    /* Обёртка прошлого сжатия плюс реплика «продолжай», а новых ходов
     * нет. Сводить нечего, и сводщик зовёться не должен: его ответ всё
     * равно пришлось бы выбросить, а место под запрос платилось бы
     * впустую. Причина — та, что видит пользователь, а не «нечего
     * сжимать» вообще: переполнение после этого вернётся на
     * следующем же шаге. */
    std::vector<Message> h;
    h.push_back(summary_wrapper("c", "всё, что было, уже в сводке"));
    h.push_back(Message::compaction_continue());
    CapturingTurn cap;
    const CompactionResult r = summarize(h, cap.fn());
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Stop);
    ASSERT_TRUE(r.reason.find("новых ходов нет") != std::string::npos);
    ASSERT_EQ(cap.replies, 0);
}

TEST(the_answer_becomes_the_summary) {
    SummaryTurn turn = [](const std::string&, const std::vector<ModelMessage>&,
                          std::string& text, std::string&) {
        text = "## Цель\n- починить тест";
        return true;
    };
    const CompactionResult r = summarize(plain_history(), turn);
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
        const CompactionResult r = summarize(plain_history(), turn);
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
    const CompactionResult r = summarize(plain_history(), turn);
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
    const CompactionResult r = summarize(plain_history(), turn);
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
        const CompactionResult r = summarize(plain_history(), turn);
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
    const CompactionResult r = summarize(plain_history(), turn);
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
    const CompactionResult r = summarize(plain_history(), nullptr);
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
    const CompactionResult r = summarize({}, turn);
    ASSERT_EQ((int)r.outcome, (int)CompactionOutcome::Stop);
    ASSERT_TRUE(r.reason.find("нечего сжимать") != std::string::npos);
    ASSERT_TRUE(r.summary.empty());
}

/* ======================================================================
 * И7.8 — что история становится после сжатия
 * ======================================================================
 *
 * Проверяется не «функция вернула что-то», а форма получившейся истории,
 * потому что форма здесь и есть смысл: [обёртка(сводка), …хвост…,
 * продолжение]. Ошибка в любом из трёх мест выглядит одинаково — агент
 * что-то делает, — но означает разное: без сводки он не помнит, что
 * было; без хвоста он не помнит, над чем работает; без продолжения он
 * останавливается на сводке и выглядит закончившим. */

namespace {

/* Готовая «сжатая» история: три хода, порог на один, чтобы хвостом
 * уцелел только последний. */
Selection selection_with_tail() {
    std::vector<Message> h = history_of_turns(3, 40);
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(100));
    return sel;
}

/* Идентификаторы одной строкой: ASSERT_EQ печатает значения через
 * operator<<, а для вектора строк его нет, и ошибка выводилась бы
 * «no match for operator<<» вместо того, что отличалось. */
std::string joined(const std::vector<std::string>& ids) {
    std::string out;
    for (const std::string& id : ids) {
        if (!out.empty()) out += ",";
        out += id;
    }
    return out;
}

std::string ids_of(const std::vector<Message>& v) {
    std::vector<std::string> out;
    for (const Message& m : v) out.push_back(m.id);
    return joined(out);
}

} // anonymous namespace

TEST(a_compacted_history_is_summary_then_tail_then_continuation) {
    const Selection sel = selection_with_tail();
    ASSERT_TRUE(sel.head.size() > 1 && sel.tail.size() > 0);   /* фикстура */

    const CompactionConfig cfg;
    const std::vector<Message> h =
        compacted_history(sel, "## Цель\n- починить тест", cfg);

    /* Обёртка первая, хвост следом, продолжение последним. */
    ASSERT_EQ(h.size(), sel.tail.size() + 2);
    ASSERT_TRUE(h.front().is_user());
    ASSERT_EQ(h.front().parts.size(), size_t(1));
    ASSERT_TRUE(h.front().parts[0].is(PartKind::Compaction));
    ASSERT_EQ(h.front().parts[0].text(), std::string("## Цель\n- починить тест"));
    for (size_t i = 0; i < sel.tail.size(); ++i) {
        ASSERT_EQ(h[i + 1].id, sel.tail[i].id);
    }
    ASSERT_TRUE(h.back().parts[0].is(PartKind::CompactionContinue));

    /* Головы в результате нет НИКАК: «сжатая история» с остатками старой
     * головы выглядела бы сжатой, а модель платила бы за оба куска. */
    /* Обёртка — единственное новое сообщение, и идентификатор у неё
     * свой; совпадение с головой означало бы, что хвост или обёртка
     * притащили старое сообщение. */
    for (const Message& m : h) {
        ASSERT_TRUE(m.id == h.front().id || m.id == h.back().id ||
                    ids_of(sel.tail).find(m.id) != std::string::npos);
    }
    /* Что именно свёрнуто — перечислено в части сводки, и перечислено
     * САМО из головы: ручной список разошёлся бы с головой при первом же
     * изменении, и UI показывал бы «свёрнуто вот это» мимо. */
    const std::vector<std::string>& replaced = h.front().parts[0].replaced_ids();
    ASSERT_EQ(joined(replaced), ids_of(sel.head));
}

TEST(the_order_after_compaction_is_not_by_id_and_that_is_fine) {
    /* Порядок массива после сжатия НЕ хронологический: у обёртки
     * идентификатор свежее всех (её создали последней), а стоит она
     * первой. Это не небрежность, а единственный способ поставить сводку
     * перед хвостом; проверяется, чтобы правка «на всякий случай
     * отсортируем» не выглядела безобидной. */
    const Selection sel = selection_with_tail();
    const CompactionConfig cfg;
    const std::vector<Message> h =
        compacted_history(sel, "сводка", cfg);
    ASSERT_TRUE(sel.tail.size() > 1);

    /* Обёртка создана ПОЗЖЕ хвоста (её идентификатор больше) и стоит
     * РАНЬШЕ. Это и есть «порядок не хронологический». */
    ASSERT_TRUE(id_number(h.front().id) > id_number(h[1].id));
    /* Реплика продолжения — самая свежая из всех и стоит в конце: с неё
     * агент продолжает, и по времени она последняя, то есть здесь
     * порядок всё-таки хронологический. */
    ASSERT_TRUE(id_number(h.back().id) > id_number(h.front().id));
    /* Массив НЕ отсортирован по идентификатору — и именно поэтому
     * сортировка «для надёжности» при загрузке сломала бы историю (см.
     * следующий тест). Проверка отрицательная намеренно: показан порядок,
     * а не запрет на будущую правку. */
    bool sorted_by_id = true;
    for (size_t i = 1; i < h.size(); ++i) {
        if (id_number(h[i - 1].id) > id_number(h[i].id)) sorted_by_id = false;
    }
    ASSERT_FALSE(sorted_by_id);
}

TEST(a_compacted_history_survives_the_session_file) {
    /* ЛОВУШКА, на которую задача и наведена: идентификаторы у нас
     * монотонны и сортируемы (id_prefix.h), и до сжатия порядок массива
     * совпадал с порядком по id. Стоит кому-то отсортировать сообщения при
     * загрузке — «для надёжности», — сводка уедет в КОНЕЦ, и агент после
     * перезагрузки получит сначала хвост, потом сводку: работа продолжится,
     * но память окажется перепутана, и ход будет отвечать не на ту
     * реплику. Загрузчик порядок ФАЙЛА сохраняет, и это держится здесь. */
    const Selection sel = selection_with_tail();
    const CompactionConfig cfg;
    const std::vector<Message> h =
        compacted_history(sel, "## Цель\n- починить тест", cfg);

    SessionFile file;
    file.session_id = "ses_000000000001";
    file.messages = h;
    const json::JsonValue js = SessionArchive::to_json(file);
    SessionFile loaded;
    std::string error;
    std::vector<std::string> warnings;
    ASSERT_TRUE(SessionArchive::from_json(js, loaded, &error, &warnings));
    ASSERT_TRUE(warnings.empty());
    ASSERT_EQ(ids_of(loaded.messages), ids_of(h));
    /* И то, что модель увидит после перезагрузки, — то же самое: сводка
     * впереди, продолжение последним. */
    const std::vector<ModelMessage> before = to_model_messages(h);
    const std::vector<ModelMessage> after = to_model_messages(loaded.messages);
    ASSERT_EQ(after.size(), before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        ASSERT_EQ(after[i].role, before[i].role);
        ASSERT_EQ(after[i].content, before[i].content);
    }
    ASSERT_TRUE(after.front().content.find("починить тест") != std::string::npos);
}

TEST(the_summary_reaches_the_model_as_an_assistant_turn) {
    /* Сводка по происхождению написана агентом-сводщиком, и модель должна
     * видеть её как ответ ассистента. Склеенная с пользовательской
     * репликой сводка означала бы «человек попросил составить сводку», а
     * на такое модель отвечает новым заданием, то есть агент после
     * сжатия начал бы с нуля. */
    const Selection sel = selection_with_tail();
    const CompactionConfig cfg;
    const std::vector<Message> h =
        compacted_history(sel, "## Цель\n- починить тест", cfg);

    const std::vector<ModelMessage> msgs = to_model_messages(h);
    ASSERT_TRUE(msgs.size() >= 3);
    ASSERT_EQ(msgs[0].role, std::string(kRoleAssistant));
    ASSERT_EQ(msgs[0].content, std::string("## Цель\n- починить тест"));
    /* Хвост после сводки, продолжение — последним. */
    ASSERT_EQ(msgs[1].role, std::string(kRoleUser));
    ASSERT_EQ(msgs.back().role, std::string(kRoleUser));
    ASSERT_EQ(msgs.back().content, std::string(kCompactionContinueText));
    /* Обёртка осталась пользовательским сообщением: роль — про
     * происхождение в ИСТОРИИ, а текст сводки уходит отдельной репликой
     * ассистента. Проверяется явно, потому что «сводка пришла
     * пользователю» — ровно та правка, которая всё ломает, и выглядит
     * она вполне разумно. */
    ASSERT_TRUE(h.front().is_user());
}

TEST(an_empty_summary_is_not_compacted_at_all) {
    /* Сжатие без сводки не применяется: обёртка с пустым текстом заняла бы
     * место головы, а головы нигде больше нет — работа до первого сжатия
     * исчезла бы целиком, и заметить это можно было бы только по тому,
     * что агент перестал помнить, зачем он здесь. */
    const Selection sel = selection_with_tail();
    const CompactionConfig cfg;
    const std::vector<Message> h = compacted_history(sel, "", cfg);
    ASSERT_EQ(ids_of(h), ids_of(sel.head) + "," + ids_of(sel.tail));
    ASSERT_EQ(h.size(), sel.head.size() + sel.tail.size());
    for (size_t i = 0; i < sel.tail.size(); ++i) {
        ASSERT_EQ(h[sel.head.size() + i].id, sel.tail[i].id);
    }
    /* Сводки в истории нет вообще: пустой обёртки тоже быть не должно. */
    for (const Message& m : h) {
        for (const MessagePart& p : m.parts) {
            ASSERT_FALSE(p.is(PartKind::Compaction));
        }
    }
}

TEST(an_empty_tail_still_leaves_a_usable_history) {
    /* Хвост может не уцелеть — например, если не влезает даже свежий
     * ход (решение 4 в compaction.h). Тогда история состоит из сводки и
     * продолжения, и это рабочая история, а не пустая. */
    const std::vector<Message> h = history_of_turns(2, 4000);
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(10));
    ASSERT_TRUE(sel.tail.empty());

    const CompactionConfig cfg;
    const std::vector<Message> out =
        compacted_history(sel, "## Цель\n- всё в сводке", cfg);
    ASSERT_EQ(out.size(), size_t(2));
    ASSERT_TRUE(out.front().parts[0].is(PartKind::Compaction));
    ASSERT_TRUE(out.back().parts[0].is(PartKind::CompactionContinue));
    ASSERT_EQ(to_model_messages(out).size(), size_t(2));
}

TEST(turning_auto_compaction_off_means_no_continuation) {
    /* Настройка «никогда не сжимать» означает и «не продолжать
     * автоматически»: реплика-продолжение существует ради того, чтобы
     * работа не встала после сжатия, и при выключенном сжатии её в
     * истории быть не должно. */
    const Selection sel = selection_with_tail();
    CompactionConfig cfg;
    cfg.auto_compact = false;
    const std::vector<Message> h = compacted_history(sel, "сводка", cfg);
    ASSERT_EQ(h.size(), sel.tail.size() + 1);
    for (const Message& m : h) {
        for (const MessagePart& p : m.parts) {
            ASSERT_FALSE(p.is(PartKind::CompactionContinue));
        }
    }
}

/* ======================================================================
 * И7.9 — прореживание вывода инструментов
 * ======================================================================
 *
 * Проверяется политика (что можно очистить) и отдельно — что очищенное
 * видно модели меткой, а не пустотой. Разделение существенное: политика
 * ничего не меняет, поэтому проверка «список пуст» ничего не сказала бы
 * о том, что модель увидит, а правка рендера не изменила бы ни одной
 * политики.
 *
 * Про устройство фикстур. Очистить можно только то, что стоит ДО
 * второго с конца хода: два последних хода (текущий и предыдущий)
 * защищены счётом реплик пользователя независимо от размера. Значит,
 * чтобы что-то очистить, нужны минимум три хода. Первая версия этих
 * проверок строила историю из двух ходов и «находила», что прореживать
 * нечего, — то есть проверяла бы фикстуру, а не код. И вторая ошибка
 * того же рода: общий completed_tool() даёт всем частям call_id
 * «call_1», и по идентификатору старый вызов от свежего не отличить. */

namespace {

/* Завершённый вызов заданного размера (в токенах) со СВОИМ
 * идентификатором. */
MessagePart big_tool(const std::string& call_id, const std::string& name,
                     long long tokens) {
    MessagePart p = MessagePart::tool(call_id, name);
    ToolOutput out;
    out.title = name;
    out.output = std::string(chars_of(tokens), 'o');
    p.set_result(out);
    return p;
}

Message tool_turn(const std::string& tag, const std::string& call_id,
                  const std::string& name, long long tokens) {
    Message m = Message::assistant("msg_user_prev");
    m.id = "msg_assist_" + tag;
    m.parts.push_back(big_tool(call_id, name, tokens));
    return m;
}

void append_turn_with_tool(std::vector<Message>& h, const std::string& tag,
                           const std::string& call_id,
                           const std::string& name, long long tokens) {
    h.push_back(user_turn(tag, 1));
    h.push_back(tool_turn(tag, call_id, name, tokens));
}

/* Идентификаторы очищенных вызовов одной строкой, в порядке истории. */
std::string cleared_calls(const std::vector<Message>& h) {
    std::string out;
    for (const Message& m : h) {
        for (const MessagePart& p : m.parts) {
            if (!p.output_cleared()) continue;
            if (!out.empty()) out += ",";
            out += p.call_id();
        }
    }
    return out;
}

/* Четыре хода, у каждого свой вызов такого размера. */
std::vector<Message> four_turns(long long tokens) {
    std::vector<Message> h;
    append_turn_with_tool(h, "0", "call_0", "bash", tokens);
    append_turn_with_tool(h, "1", "call_1", "read_file", tokens);
    append_turn_with_tool(h, "2", "call_2", "bash", tokens);
    append_turn_with_tool(h, "3", "call_3", "read_file", tokens);
    return h;
}

/* Четыре хода, где вывод сосредоточен в самом старом и в первом из
 * просматриваемых: защита съедает второй, очищается первый. */
std::vector<Message> old_and_protected(long long old_tokens) {
    std::vector<Message> h;
    append_turn_with_tool(h, "0", "call_old", "bash", old_tokens);
    append_turn_with_tool(h, "1", "call_keep", "read_file",
                          limits::kPruneProtectTokens);
    append_turn_with_tool(h, "2", "call_f2", "bash", 50000);
    append_turn_with_tool(h, "3", "call_f3", "read_file", 50000);
    return h;
}

} // anonymous namespace

TEST(fresh_tool_output_is_never_pruned) {
    /* Пока объём просмотренного вывода не перевалил защиту, не очищается
     * ничего — даже очень старые выводы. Иначе агент лишился бы того, что
     * разбирает прямо сейчас, и получал бы метки вместо данных, на
     * которые сам же сослался двумя шагами раньше. */
    std::vector<Message> small = four_turns(5000);
    ASSERT_TRUE(prune_candidates(small).empty());
    std::vector<Message> mid = four_turns(15000);
    ASSERT_TRUE(prune_candidates(mid).empty());
    /* Ровно на пороге защиты — тоже ещё ничего: «не меньше порога» и
     * «больше порога» — разные вещи, и сдвиг на единицу меняет,
     * очищается ли хоть что-то. */
    std::vector<Message> exact = four_turns(10000);
    ASSERT_TRUE(prune_candidates(exact).empty());
}

TEST(old_output_goes_when_the_tail_is_still_untouched) {
    /* Защита — 40000 токенов самых свежих просмотренных выводов. Ставим
     * ровно 40000 свежих и 30000 старых: свежие уцелеют (счёт дошёл до
     * порога и не превысил), старый вызов очистится. */
    std::vector<Message> h = old_and_protected(30000);
    const std::vector<MessagePart*> c = prune_candidates(h);
    ASSERT_EQ(c.size(), size_t(1));
    ASSERT_EQ(c[0]->call_id(), std::string("call_old"));
    c[0]->clear_output();
    ASSERT_EQ(cleared_calls(h), std::string("call_old"));

    /* Вывод в части ОСТАЁТСЯ: его показывают UI и файл сессии, а модели
     * уходит метка. Стертый текст означал бы, что перезагруженная сессия
     * потеряла данные, которых не было никогда. */
    ASSERT_EQ(c[0]->output().output.size(), chars_of(30000));
    ASSERT_TRUE(c[0]->has_result());
}

TEST(the_last_two_turns_are_never_touched_however_big) {
    /* Два последних хода защищены счётом реплик пользователя, а не
     * размером: 100000 токенов вывода в них остаются целыми. Очистить их
     * — значит оборвать рассуждение, на которое агент ссылается сейчас.
     *
     * Счёт защиты при этом их вывод НЕ расходует: обход пропускает эти
     * ходы целиком, не начисляя токены. Иначе одна и та же цифра
     * означала бы разное в зависимости от того, откуда начинали считать,
     * и «защищено 40000» перестало бы быть правдой. */
    std::vector<Message> h = four_turns(100000);
    const std::vector<MessagePart*> c = prune_candidates(h);
    /* Очищаются первый и второй ходы с начала — то есть все, кроме двух
     * последних, как бы ни были велики их выводы. */
    ASSERT_EQ(c.size(), size_t(2));
    ASSERT_EQ(c[0]->call_id(), std::string("call_1"));
    ASSERT_EQ(c[1]->call_id(), std::string("call_0"));
    for (MessagePart* p : c) p->clear_output();
    /* Список идёт по ИСТОРИИ, а не в том порядке, в каком обход отбирал:
     * обход идёт от конца. */
    ASSERT_EQ(cleared_calls(h), std::string("call_0,call_1"));
}

TEST(a_small_saving_is_not_worth_the_markers) {
    /* Порог снизу отвечает на вопрос «а стоит ли», а не «что нельзя»: меток
     * «очищено» в промпте вышло бы больше пользы, чем выгоды от них. */
    std::vector<Message> cheap = old_and_protected(15000);
    ASSERT_TRUE(prune_candidates(cheap).empty());
    /* А 30000 — уже выгода. Разница между этими двумя случаями и есть
     * весь смысл порога: при 15000 очистка «съела» бы два вывода и
     * оставила бы две метки, ради которых нечего было начинать. */
    std::vector<Message> h = old_and_protected(30000);
    const std::vector<MessagePart*> c = prune_candidates(h);
    ASSERT_EQ(c.size(), size_t(1));
    c[0]->clear_output();
    ASSERT_EQ(cleared_calls(h), std::string("call_old"));
}

TEST(already_cleared_output_is_skipped_and_costs_nothing) {
    /* Повторный запуск на уже прореженной истории не предлагает ничего:
     * очищенные вызовы стоят в модели десятком токенов, поэтому защита
     * ими не расходуется, а сами они повторно не очищаются. */
    std::vector<Message> h = four_turns(30000);
    std::vector<MessagePart*> c = prune_candidates(h);
    ASSERT_EQ(c.size(), size_t(1));
    ASSERT_EQ(c[0]->call_id(), std::string("call_0"));
    c[0]->clear_output();
    ASSERT_EQ(cleared_calls(h), std::string("call_0"));
    ASSERT_TRUE(prune_candidates(h).empty());

    /* За очищенным может лежать НЕОЧИЩЕННЫЙ гигант — так бывает, если
     * сессию правили руками. Обход обязан его увидеть: остановка на
     * границе (как в порте) оставила бы его в контексте навсегда, и
     * переполнение вернулось бы при том, что прореживание «уже отработало».
     *
     * Арифметика: очищенный вызов 50000 не в счёт, оставшийся 50000
     * переваливает защиту 40000 и очищается. */
    std::vector<Message> patched = four_turns(50000);
    patched[3].parts[0].clear_output();
    std::vector<MessagePart*> again = prune_candidates(patched);
    ASSERT_EQ(again.size(), size_t(1));
    ASSERT_EQ(again[0]->call_id(), std::string("call_0"));

    /* И обратная сторона того же правила: очищенный вывод не расходует
     * защиту, поэтому неочищенный рядом с ним остаётся целым.
     *
     * Арифметика: очищенный 30000 + неочищенный 25000. Считай очищенный —
     * набралось бы 55000, и 25000 вышли бы за защиту 40000 и были бы
     * очищены (порог «а стоит ли» 20000 пройден); не считаем — 25000, всё
     * под защитой, список пуст. Числа подобраны так, чтобы различала и
     * вторая проверка снизу: при 15000 выигрыш был бы ниже порога и
     * список оказался бы пустым в обоих случаях, то есть проверка
     * прошла бы при любом коде. Разница одна, и она и есть смысл
     * правила: место в контексте занимает метка, а не удалённый текст. */
    std::vector<Message> neighbour = four_turns(30000);
    neighbour[3].parts[0].clear_output();
    neighbour[1].parts[0] = big_tool("call_0", "bash", 25000);
    ASSERT_TRUE(prune_candidates(neighbour).empty());
}

TEST(the_skill_body_is_never_pruned) {
    /* Вывод skill — это инструкция, по которой агент работает. Очистить
     * его значит не освободить место, а сломать работу на середине, причём
     * модель об этом даже не узнает: получит метку и продолжит «по
     * инструкции», которой нет. */
    std::vector<Message> h = four_turns(50000);
    /* Промежуточный вызов — инструкция навыка, а не прочий вывод, и
     * размером он не меньше прочих. */
    h[3].parts[0] = big_tool("call_1", "skill", 50000);
    std::vector<MessagePart*> c = prune_candidates(h);
    /* Очистился только не-skill; skill не тронут, хотя он старше
     * очищенного и такой же большой. */
    for (const MessagePart* p : c) {
        ASSERT_TRUE(p->tool_name() != std::string("skill"));
    }
    for (MessagePart* p : c) p->clear_output();
    ASSERT_EQ(cleared_calls(h), std::string("call_0"));
}

TEST(a_failed_tool_call_is_never_pruned) {
    /* Отказ и так короткий, а метка «очищено» на отказе выглядела бы как
     * поломка инструмента: модель решила бы, что вызов не удался из-за
     * объёма, и повторила бы его — то есть потратила ещё и деньги. */
    std::vector<Message> h = four_turns(50000);
    /* Второй ход с начала — отказ с длинным пояснением вместо вывода. */
    h[3].parts[0] = MessagePart::tool("call_err", "bash")
                        .set_error(std::string(chars_of(50000), 'e'));
    std::vector<MessagePart*> c = prune_candidates(h);
    ASSERT_EQ(c.size(), size_t(1));
    ASSERT_EQ(c[0]->call_id(), std::string("call_0"));
    for (MessagePart* p : c) p->clear_output();
    /* Отказ не тронут: у него нет вывода, а метка «очищено» выглядела бы
     * как поломка инструмента, и модель повторила бы вызов. */
    ASSERT_EQ(cleared_calls(h), std::string("call_0"));
}

TEST(pruning_stops_at_the_summary) {
    /* Всё, что старше сводки, уже выкинуто из истории (7.8), и трогать там
     * нечего. Если бы обход пошёл дальше, он набрал бы токены по остаткам
     * и решил бы, что пора чистить, — метки появились бы там, где модели
     * всё равно ничего не достаётся. */
    std::vector<Message> h;
    append_turn_with_tool(h, "z", "call_z", "bash", 60000);
    h.push_back(summary_wrapper("c", "сводка"));
    append_turn_with_tool(h, "0", "call_0", "bash", 30000);
    append_turn_with_tool(h, "1", "call_1", "read_file", 40000);
    append_turn_with_tool(h, "2", "call_2", "bash", 50000);
    append_turn_with_tool(h, "3", "call_3", "read_file", 50000);

    const std::vector<MessagePart*> c = prune_candidates(h);
    ASSERT_EQ(c.size(), size_t(1));
    ASSERT_EQ(c[0]->call_id(), std::string("call_0"));
}

TEST(a_cleared_result_reaches_the_model_as_a_marker) {
    /* Модель должна увидеть метку, а не пустоту: пустой RESULT выглядел бы
     * как инструмент без вывода, и агент решил бы, что команда ничего не
     * нашла. Строка названа в системном промпте — иначе модель приняла бы
     * её за содержимое и стала бы рассуждать о тексте, которого нет
     * (ровно класс D2, только для другой строки формата). */
    Message turn = Message::assistant("msg_user_1");
    turn.id = "msg_assist_1";
    turn.parts.push_back(big_tool("call_1", "grep_search", 1000));
    turn.parts[0].clear_output();

    Message whole = Message::assistant("msg_user_1");
    whole.id = "msg_assist_2";
    whole.parts.push_back(big_tool("call_2", "grep_search", 1000));

    const std::vector<ModelMessage> msgs = to_model_messages({turn});
    ASSERT_EQ(msgs.size(), size_t(2));
    ASSERT_EQ(msgs[0].role, std::string(kRoleAssistant));
    ASSERT_EQ(msgs[1].role, std::string(kRoleUser));
    ASSERT_EQ(msgs[1].content,
              std::string("RESULT [grep_search]:\n") +
                  limits::kClearedToolOutput);
    ASSERT_TRUE(std::string(kBaseSystemPrompt).find(
                    limits::kClearedToolOutput) != std::string::npos);

    /* Не очищенный результат метку не получает — иначе очистка была бы
     * неотличима от пустого вывода. */
    ASSERT_TRUE(to_model_messages({whole})[1].content.find("ooo") !=
                std::string::npos);
    ASSERT_TRUE(to_model_messages({whole})[1].content.find(
                    limits::kClearedToolOutput) == std::string::npos);
    /* И оценка объёма меняется — именно ради этого очистка и нужна:
     * метка короткая, иначе прореживание ничего бы не освободило и
     * считалось бы вхолостую. */
    /* Метка — 35 символов, вместе с меткой строки RESULT это меньше
     * сотни токенов, а очищенный вывод стоил тысячу: очистка обязана
     * освобождать место, иначе прореживание работало бы вхолостую. */
    const long long marker_only = estimate_history_tokens({turn});
    ASSERT_TRUE(marker_only < 100LL);
    ASSERT_TRUE(estimate_history_tokens({whole}) > 1000LL);
}

TEST(clearing_keeps_the_result_and_the_state) {
    /* clear_output() не трогает ни вывод, ни состояние: у очищенного вызова
     * по-прежнему есть результат, по которому видно, что вызов был и что
     * он удался. Это нужно UI («очищено, но отработало»), это нужно
     * условию завершения хода (И5.8) — и это то, что отличает очистку от
     * потери вызова. */
    MessagePart p = big_tool("call_1", "read_file", 10);
    ASSERT_FALSE(p.output_cleared());
    p.clear_output();
    ASSERT_TRUE(p.output_cleared());
    ASSERT_TRUE(p.has_result());
    ASSERT_EQ(std::string(p.state_name()), std::string("completed"));
    /* Повторная отметка ничего не ломает: вызов могут отметить и снова. */
    p.clear_output();
    ASSERT_TRUE(p.output_cleared());

    /* На незавершённом и отказавшем вызове отметки нет: там либо нет
     * вывода, либо метка выглядела бы как поломка. */
    ASSERT_FALSE(MessagePart::tool("call_x", "bash")
                     .clear_output().output_cleared());
    ASSERT_FALSE(MessagePart::tool("call_y", "bash")
                     .set_error("отказ").clear_output().output_cleared());
    /* И не на части другого вида: очищать нечего, а молчаливый no-op
     * здесь опаснее отказа (тот же аргумент, что у set_result). */
    ASSERT_FALSE(MessagePart::text("просто текст")
                     .clear_output().output_cleared());
}

TEST(the_prune_flag_is_read_from_settings) {
    /* Прореживание выключено по умолчанию, как в порте: оно меняет то, что
     * видит модель, а не то, что видит пользователь. */
    LimitsGuard guard;
    {
        std::map<std::string, std::string> v;
        HostCallbacks cb = settings_host(v);
        engine().init(cb);
        engine().load_settings();
    }
    ASSERT_FALSE(engine_state().compaction_config.prune);
    {
        std::map<std::string, std::string> v;
        v["wp_coder.compaction_prune"] = "true";
        HostCallbacks cb = settings_host(v);
        engine().init(cb);
        engine().load_settings();
    }
    ASSERT_TRUE(engine_state().compaction_config.prune);
    /* Мусор → выключено, а не включено: лишнее слово в настройке, которая
     * стирает данные из контекста, не должно включать стирание. */
    {
        std::map<std::string, std::string> v;
        v["wp_coder.compaction_prune"] = "да";
        HostCallbacks cb = settings_host(v);
        engine().init(cb);
        engine().load_settings();
    }
    ASSERT_FALSE(engine_state().compaction_config.prune);
}

/* ======================================================================
 * И7.11 — границы
 * ======================================================================
 *
 * Задача эта про границы, и границы у сжатия трёх видов:
 *
 *   - ВЫБОР ХВОСТА. Ровно на пороге ход уцелеет целиком, на один токен
 *     больше — уйдёт в сводку. Проверка на «примерно» здесь бесполезна:
 *     ошибка на единицу токенов стоит либо лишнего запроса к модели, либо
 *     обрезанного хвоста, и обе не видны ни в одном «среднем» тесте.
 *   - ПОРОГ ОКНА. Ровно usable — уже переполнение. Проверено ещё в 7.1
 *     (overflow_starts_exactly_at_the_threshold), и повторять то же самое
 *     второй раз незачем: повторная проверка того же утверждения создаёт
 *     впечатление покрытия, а не покрытия.
 *   - ПОВТОРНОЕ СЖАТИЕ. Сжатая история сжимается снова, и это единственный
 *     способ проверить, что сводка не копится и не теряется: на второй
 *     итерации встречаются сразу и часть compaction, и реплика
 *     продолжения, и прошлая сводка.
 *
 * Размеры в проверках считаются ЧЕРЕЗ estimate_history_tokens, а не
 * вписаны числами: вписывание числа рядом с изменением оценки даёт
 * проверку, которая падает не по делу. */

namespace {

/* Сколько токенов занимает один ход фикстуры — считается на живой
 * истории, чтобы порог в проверках был честным. */
long long one_turn_tokens() {
    std::vector<Message> h = history_of_turns(1, 40);
    return estimate_history_tokens(h);
}

/* Идентификаторы списком: ids_of() склеивает их в строку ради
 * ASSERT_EQ, а здесь нужно сравнивать множества. */
std::vector<std::string> id_list(const std::vector<Message>& v) {
    std::vector<std::string> out;
    for (const Message& m : v) out.push_back(m.id);
    return out;
}


} // anonymous namespace

TEST(selection_of_an_empty_history_is_not_a_crash) {
    /* Пустая история — не «сжимать нечего», а «сессия только началась».
     * Функция обязана вернуть пустой результат, а не историю из одного
     * фантомного хвоста: такой хвост сжатие посчитало бы за работу,
     * которой не было. */
    const std::vector<Message> nothing;
    const Selection sel =
        select_to_compact(nothing, limits_with_usable(100000), keep_n_tokens(1000));
    ASSERT_TRUE(sel.head.empty());
    ASSERT_TRUE(sel.tail.empty());
    ASSERT_EQ(sel.tail_tokens, 0LL);
    /* И пустая история не проходит через сжатие как сжимаемая. */
    const std::vector<Message> after =
        compacted_history(sel, "", CompactionConfig());
    ASSERT_EQ(after.size(), size_t(0));
}

TEST(a_lone_request_is_kept_whole_or_summarized_whole) {
    /* Ход из ОДНОЙ реплики разделить нечем: разрез идёт со второго
     * сообщения (решение 3), а второго нет — уцелел бы ровно тот же
     * текст, который и не влезает. Значит выбор только один: целиком в
     * хвост или целиком в сводку. Полумеры здесь означали бы, что модель
     * получит половину задачи.
     *
     * Первая версия этой проверки утверждала, что одиночная реплика
     * остаётся хвостом «при любом пороге». Это не так и не должно быть:
     * порог в 1 токен меньше самой реплики, и единственный честный ответ —
     * сжать её. Настоящий случай «реплика не потеряна» — соседняя
     * проверка, где порог её вмещает. */
    std::vector<Message> big;
    big.push_back(user_turn("0", 1000));
    const Selection over =
        select_to_compact(big, limits_with_usable(100000), keep_n_tokens(1));
    ASSERT_TRUE(over.tail.empty());
    ASSERT_EQ(over.head.size(), size_t(1));

    /* А когда реплика в порог вмещается — не «хвост из неё», а
     * «сжимать нечего»: уцелела вся история, и решение 4 отдаёт её в
     * голову с пустым хвостом. Проверка требовала хвоста и была неправа
     * по той же причине: пустой хвост у полностью уцелевшей истории —
     * это ответ «нечего сжимать», а ответ «сжать всё». */
    std::vector<Message> small;
    small.push_back(user_turn("0", 40));
    const Selection fits =
        select_to_compact(small, limits_with_usable(100000), keep_n_tokens(1000));
    ASSERT_TRUE(fits.tail.empty());
    ASSERT_EQ(fits.head.size(), size_t(1));
}

TEST(a_single_turn_too_big_for_the_tail_is_summarized_whole) {
    /* Ход, который не влезает в порог ЦЕЛИКОМ, уходит в сводку, и хвост
     * пуст. Это не «сжимать нечего»: обрезка внутри хода (решение 3)
     * может оставить его кусок — но только если этот кусок влезает. Здесь
     * не влезает даже ответ ассистента. */
    std::vector<Message> h = history_of_turns(1, 4000);
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(100));
    ASSERT_TRUE(sel.tail.empty());
    ASSERT_EQ(sel.head.size(), h.size());

    /* И такая история после сжатия рабочая: сводка и продолжение. */
    const std::vector<Message> after =
        compacted_history(sel, "## Цель\n- всё в сводке", CompactionConfig());
    ASSERT_EQ(after.size(), size_t(2));
    ASSERT_TRUE(after.front().parts[0].is(PartKind::Compaction));
    ASSERT_TRUE(after.back().parts[0].is(PartKind::CompactionContinue));
}

TEST(the_tail_boundary_is_inclusive) {
    /* Порог ровно в один ход: ход уцелеет ЦЕЛИКОМ. На один токен меньше —
     * не уцелеет ничего, потому что ни ход целиком, ни его ответ не
     * влезают, а разрез хода идёт со второго сообщения.
     *
     * Считается на живой истории: если оценка изменится, проверка
     * поедет вместе с ней, а не станет падать по не связанной с делом
     * причине. */
    std::vector<Message> h = history_of_turns(3, 40);
    const long long turn_tokens = one_turn_tokens();
    ASSERT_TRUE(turn_tokens > 0);

    const Selection exact =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(turn_tokens));
    ASSERT_EQ(exact.tail.size(), size_t(2));
    ASSERT_EQ(estimate_history_tokens(exact.tail), turn_tokens);
    ASSERT_EQ(std::string(last_id(exact.tail)), std::string("msg_assist_2"));

    /* На один токен меньше ход целиком не влезает, но его ОТВЕТ влезает
     * ровно — и уцелеет он один (решение 3). Проверка требовала пустого
     * хвоста и была неправа: пустой хвост означал бы «не влезает даже
     * свежий ход», а здесь влезает его половина. */
    const Selection less =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(turn_tokens - 1));
    ASSERT_EQ(less.tail.size(), size_t(1));
    ASSERT_EQ(std::string(first_id(less.tail)), std::string("msg_assist_2"));
    ASSERT_EQ(less.tail_tokens, turn_tokens - 1);
}

TEST(tail_turns_caps_the_candidates_and_the_budget_caps_the_result) {
    /* Два ограничения независимы: tail_turns говорит «не больше N ходов»,
     * порог — «столько-то токенов». Проверяются обе половины по отдельности,
     * иначе ошибка в одной маскировалась бы другой. */
    std::vector<Message> h = history_of_turns(5, 100);

    /* tail_turns = 2, места с запасом: кандидатов два, и оба помещаются. */
    CompactionConfig two;
    two.tail_turns = 2;
    two.preserve_recent_tokens = 100000;
    const Selection wide =
        select_to_compact(h, limits_with_usable(100000), two);
    ASSERT_EQ(wide.tail.size(), size_t(4));
    ASSERT_EQ(std::string(first_id(wide.tail)), std::string("msg_user_3"));

    /* tail_turns = 1 при том же месте: кандидат один ход. */
    CompactionConfig one = two;
    one.tail_turns = 1;
    const Selection narrow = select_to_compact(h, limits_with_usable(100000), one);
    ASSERT_EQ(narrow.tail.size(), size_t(2));
    ASSERT_EQ(std::string(first_id(narrow.tail)), std::string("msg_user_4"));

    /* tail_turns = 1, но места хватает только на половину хода: уцелеет
     * ответ без реплики пользователя, на которую он отвечал. */
    CompactionConfig tight = one;
    tight.preserve_recent_tokens =
        estimate_history_tokens({answer_turn("msg_user_4", "4", 99)});
    const Selection split = select_to_compact(h, limits_with_usable(100000), tight);
    ASSERT_EQ(split.tail.size(), size_t(1));
    ASSERT_EQ(std::string(first_id(split.tail)), std::string("msg_assist_4"));
}

TEST(the_second_compaction_replaces_the_first_summary) {
    /* Итеративность целиком: сжатая история сжимается снова, и за один
     * проход не остаётся НИ ОДНОЙ старой сводки. Три вещи, которые видно
     * только здесь: сводка не копится (вторая заменяет первую), хвост
     * остаётся хвостом, а сводщик видит прошлую сводку и новый разговор
     * РАЗДЕЛЬНО — иначе он выбрал бы одно из двух и потерял бы другое. */
    const CompactionConfig cfg;

    /* Первое сжатие. */
    std::vector<Message> first = history_of_turns(6, 100);
    const Selection sel1 =
        select_to_compact(first, limits_with_usable(100000), keep_n_tokens(200));
    CapturingTurn cap1;
    const CompactionResult r1 = summarize(sel1.head, cap1.fn());
    ASSERT_EQ((int)r1.outcome, (int)CompactionOutcome::Continue);
    std::vector<Message> after1 = compacted_history(sel1, r1.summary, cfg);
    ASSERT_EQ(cap1.calls_had_prior, false);

    /* Работа продолжается, история опять растёт. */
    append_turn(after1, "6", 100);
    append_turn(after1, "7", 100);
    append_turn(after1, "8", 100);

    /* Второе сжатие. */
    const Selection sel2 =
        select_to_compact(after1, limits_with_usable(100000), keep_n_tokens(200));
    CapturingTurn cap2;
    const CompactionResult r2 = summarize(sel2.head, cap2.fn());
    ASSERT_EQ((int)r2.outcome, (int)CompactionOutcome::Continue);

    /* Прошлая сводка дошла до сводщика отдельной полосой. */
    ASSERT_TRUE(cap2.calls_had_prior);
    ASSERT_EQ(cap2.prior_text, r1.summary);

    const std::vector<Message> after2 = compacted_history(sel2, r2.summary, cfg);
    /* Ровно одна сводка в истории, и она новая. */
    int summaries = 0;
    for (const Message& m : after2) {
        for (const MessagePart& p : m.parts) {
            if (!p.is(PartKind::Compaction)) continue;
            ++summaries;
            ASSERT_EQ(p.text(), r2.summary);
        }
    }
    ASSERT_EQ(summaries, 1);
    /* Продолжений тоже ровно одно: старое ушло в сводку вместе с головой. */
    int continuations = 0;
    for (const Message& m : after2) {
        for (const MessagePart& p : m.parts) {
            if (p.is(PartKind::CompactionContinue)) ++continuations;
        }
    }
    ASSERT_EQ(continuations, 1);
    ASSERT_TRUE(after2.back().parts[0].is(PartKind::CompactionContinue));
}

TEST(after_reordering_no_tail_message_is_duplicated_or_lost) {
    /* Самое обидное последствие переупорядочивания — не потерять и не
     * задвоить содержимое. Свёрнутое сообщение, попавшее и в сводку, и в
     * хвост, стоило бы модели дважды пересказанного хода; выпавшее из
     * хвоста — тихой потери работы. Проверяется сверкой множеств
     * идентификаторов: оба хвоста — подмножества исходной истории, и
     * вместе они не пересекаются. */
    const CompactionConfig cfg;
    std::vector<Message> h = history_of_turns(5, 60);
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(100));
    const std::vector<Message> after =
        compacted_history(sel, "сводка", cfg);

    const std::vector<std::string> head_ids = id_list(sel.head);
    const std::vector<std::string> tail_ids = id_list(sel.tail);
    ASSERT_TRUE(!head_ids.empty() && !tail_ids.empty());

    /* Каждый идентификатор головы встречается в новой истории НЕ БОЛЬШЕ
     * одного раза (в сводку он попал по тексту, но не отдельным
     * сообщением). */
    const std::vector<std::string> in_history = id_list(after);
    for (const std::string& id : head_ids) {
        int found = 0;
        for (const std::string& got : in_history) {
            if (got == id) ++found;
        }
        ASSERT_EQ(found, 0);
    }
    for (const std::string& id : tail_ids) {
        int found = 0;
        for (const std::string& got : in_history) {
            if (got == id) ++found;
        }
        ASSERT_EQ(found, 1);
    }
    /* В новой истории ровно хвост плюс обёртка сводки и продолжение. */
    ASSERT_EQ(in_history.size(), tail_ids.size() + 2);
}

TEST(tool_results_in_the_tail_survive_compaction) {
    /* Сжатая история уходит модели, и хвост в ней обязан остаться
     * целиком вместе с РЕЗУЛЬТАТАМИ вызовов: именно они и есть работа
     * агента. Проверяется сверкой счетчиков, а не «строки похожи»:
     * потерянный результат и задвоенный выглядели бы одинаково.
     *
     * Первая версия этой проверки искала в транскрипте «RESULT без
     * вызова» — а такая пара в нашей модели невозможна: вызов и его
     * результат живут в одной части, и to_model_messages порождает их
     * вместе. Проверка была тавтологией и ловила бы что угодно, кроме
     * настоящей поломки. */
    std::vector<Message> h;
    for (int i = 0; i < 6; ++i) {
        h.push_back(user_turn(std::to_string(i), 1));
        Message turn = Message::assistant("msg_user_" + std::to_string(i));
        turn.id = "msg_assist_" + std::to_string(i);
        turn.parts.push_back(big_tool("call_" + std::to_string(i), "bash", 800));
        h.push_back(turn);
    }
    const Selection sel =
        select_to_compact(h, limits_with_usable(100000), keep_n_tokens(1000));
    ASSERT_TRUE(!sel.tail.empty());

    /* Сколько результатов должно остаться в хвосте. */
    int tail_results = 0;
    for (const Message& m : sel.tail) {
        for (const MessagePart& p : m.parts) {
            if (p.is(PartKind::Tool) &&
                p.state_name() == std::string("completed")) {
                ++tail_results;
            }
        }
    }
    ASSERT_TRUE(tail_results > 0);

    const std::vector<Message> after =
        compacted_history(sel, "сводка", CompactionConfig());
    const std::vector<ModelMessage> msgs = to_model_messages(after);
    int results = 0;
    for (const ModelMessage& m : msgs) {
        if (m.content.rfind("RESULT [", 0) == 0) ++results;
    }
    ASSERT_EQ(results, tail_results);
    /* И сводка пришла ровно один раз — впереди, отдельной репликой
     * ассистента: задвоенная сводка означала бы, что ход пересказан
     * дважды. */
    int summaries = 0;
    for (const ModelMessage& m : msgs) {
        if (m.content == std::string("сводка")) ++summaries;
    }
    ASSERT_EQ(summaries, 1);
    ASSERT_EQ(msgs[0].role, std::string(kRoleAssistant));
    ASSERT_EQ(msgs[0].content, std::string("сводка"));
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

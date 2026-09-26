/*
 * test_llm_events.cpp — И5.1: событийная модель ответа LLM.
 *
 * Проверяется не «есть класс», а инвариант, на котором держится вся
 * остальная итерация: у события РОВНО те данные, которые полагаются
 * его виду, и никаких других.
 *
 * Почему это стоит проверять таблицей, а не тремя тестами на фабрики.
 * Вариант эмулирован закрытым конструктором и фабриками, а данные лежат
 * в общих полях. Ошибка «положил не туда» (текст ошибки у Finish,
 * имя инструмента у ToolResult) не даёт ни исключения, ни красного
 * теста — она даёт тихо неверную историю диалога, из которой потом
 * нельзя понять, что произошло. Таблица «вид → какие аксессоры
 * заполнены» ловит и забытую фабрикой полезную нагрузку, и лишнюю.
 */

#include "test_framework.h"
#include "test_printers.h"
#include "../core/json_utils.h"
#include "../core/llm_event.h"

#include <ostream>
#include <set>
#include <string>
#include <vector>

using namespace coder;


namespace {

json::JsonValue args_object() {
    json::JsonValue a = json::JsonValue::object();
    a.set("path", "core/engine.cpp");
    a.set("offset", 42);
    return a;
}

ToolOutput read_output() {
    ToolOutput out;
    out.title = "read core/engine.cpp";
    out.output = "engine.h — Универсальный ReAct-движок";
    out.metadata = json::JsonValue::object();
    out.metadata.set("lines", 404);
    out.truncated = true;
    return out;
}

/* Одно событие + ожидаемое содержимое каждого аксессора.
 *
 * Поля идут в порядке аксессоров класса, а у всех кроме finish_reason
 * проверка — «заполнен или пуст». Пустое значение finish_reason означает
 * «у этого вида его быть не должно»: так в таблице не остаётся
 * позиционной пустоты, из-за которой одна ошибка в разметке таблицы
 * тихо проверяла бы соседнее событие. */
struct Case {
    LlmEvent event;
    bool delta;          /* delta()          */
    bool call_id;        /* call_id()        */
    bool tool_name;      /* tool_name()      */
    bool arguments;      /* arguments()      */
    bool tool_output;    /* tool_output()    */
    bool usage;          /* usage()          */
    bool error;          /* error()          */
    std::string finish;  /* finish_reason()  */

    Case(LlmEvent e,
         bool delta_ = false, bool call_id_ = false, bool tool_name_ = false,
         bool arguments_ = false, bool tool_output_ = false, bool usage_ = false,
         bool error_ = false, std::string finish_ = "")
        : event(std::move(e)), delta(delta_), call_id(call_id_),
          tool_name(tool_name_), arguments(arguments_),
          tool_output(tool_output_), usage(usage_), error(error_),
          finish(std::move(finish_)) {}
};

bool has_output(const ToolOutput& o) {
    return !o.title.empty() || !o.output.empty() || o.truncated ||
           o.metadata.type() != json::JsonValue::Type::Null;
}

bool has_arguments(const json::JsonValue& v) {
    return v.type() != json::JsonValue::Type::Null;
}

bool has_usage(const Usage& u) {
    return !u.empty();
}

Usage some_usage() {
    return Usage::from_provider(1000, 200, 400, 0, true);
}

std::vector<Case> all_cases() {
    return {
        Case(LlmEvent::step_start()),
        Case(LlmEvent::text_start()),
        Case(LlmEvent::text_delta("а"), true),
        Case(LlmEvent::text_end()),
        Case(LlmEvent::reasoning_start()),
        Case(LlmEvent::reasoning_delta("п"), true),
        Case(LlmEvent::reasoning_end()),
        Case(LlmEvent::tool_input_start("call_0", "read_file"), false, true, true),
        Case(LlmEvent::tool_input_delta("call_0", "{"), true, true),
        Case(LlmEvent::tool_input_end("call_0"), false, true),
        Case(LlmEvent::tool_call("call_0", "read_file", args_object()),
             false, true, true, true),
        Case(LlmEvent::tool_result("call_0", read_output()),
             false, true, false, false, true),
        Case(LlmEvent::tool_error("call_0", "отказ режима"),
             false, true, false, false, false, false, true),
        Case(LlmEvent::step_finish()),
        /* Finish встречается дважды намеренно: с метриками и без них.
         * Провайдер, не присылающий usage, — обычное дело, и его путь
         * не должен отличаться от пути с usage, кроме самого usage. */
        Case(LlmEvent::finish("length"), false, false, false, false, false,
             false, false, "length"),
        Case(LlmEvent::finish("stop", some_usage()), false, false, false,
             false, false, true, false, "stop"),
        Case(LlmEvent::provider_error("таймаут"),
             false, false, false, false, false, false, true),
    };
}

} // namespace

/* Шестнадцать видов — ровно столько, сколько в opencode
 * (packages/llm/src/schema/events.ts:209). Считаем явно, а не «пока
 * перечисление совпадает»: новый вид события обязан быть добавлен в
 * таблицу теста, иначе тест об этом не узнает. */
TEST(llm_event_has_exactly_sixteen_kinds) {
    int count = 0;
    for (int i = 0; i < static_cast<int>(LlmEventKind::kCount); ++i) {
        ++count;
    }
    ASSERT_EQ(count, 16);
    ASSERT_EQ(static_cast<int>(LlmEventKind::kCount), count);

    /* И таблица теста обязана перечислять все шестнадцать видов: строк
     * в ней может быть больше (Finish проверяется двумя — с usage и
     * без), но каждый вид обязан встретиться, иначе таблица проверяет
     * меньше, чем кажется. */
    std::set<int> covered;
    for (const Case& c : all_cases()) {
        covered.insert(static_cast<int>(c.event.kind()));
    }
    if (covered.size() != static_cast<size_t>(count)) {
        std::cerr << "  в таблице теста " << covered.size() << " видов из "
                  << count << ": "
                  << "не перечислен какой-то LlmEventKind" << std::endl;
    }
    ASSERT_EQ(covered.size(), static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        ASSERT_TRUE(covered.count(i) == 1);
    }
}

/* Имя вида — не украшение: по нему UI и лог отличают ToolInputDelta от
 * ToolCall. Пропущенный или продублированный случай в switch означал бы
 * «unknown» в логе у одного из видов. */
TEST(llm_event_kind_names_are_unique_and_never_empty) {
    std::set<std::string> names;
    for (int i = 0; i < static_cast<int>(LlmEventKind::kCount); ++i) {
        const char* name =
            llm_event_kind_name(static_cast<LlmEventKind>(i));
        ASSERT_TRUE(name != nullptr);
        ASSERT_TRUE(std::string(name) != "unknown");
        ASSERT_TRUE(std::string(name) != "");
        names.insert(name);
    }
    ASSERT_EQ(names.size(), static_cast<size_t>(
        static_cast<int>(LlmEventKind::kCount)));
    /* Значение вне диапазона (адаптер отдал неизвестный вид) не должно
     * ронять процесс и не должно выглядеть как известное событие. */
    ASSERT_EQ(std::string(llm_event_kind_name(static_cast<LlmEventKind>(999))),
              std::string("unknown"));
}

TEST(llm_event_name_matches_the_kind_of_the_event) {
    for (const Case& c : all_cases()) {
        ASSERT_EQ(std::string(c.event.kind_name()),
                  std::string(llm_event_kind_name(c.event.kind())));
        ASSERT_TRUE(c.event.is(c.event.kind()));
    }
}

/* Главный инвариант: у каждого вида заполнены ровно те аксессоры,
 * которые этому виду полагаются. */
TEST(llm_event_carries_only_its_own_payload) {
    for (const Case& c : all_cases()) {
        const LlmEvent& e = c.event;
        ASSERT_EQ(!e.delta().empty(), c.delta);
        ASSERT_EQ(!e.call_id().empty(), c.call_id);
        ASSERT_EQ(!e.tool_name().empty(), c.tool_name);
        ASSERT_EQ(has_arguments(e.arguments()), c.arguments);
        ASSERT_EQ(has_output(e.tool_output()), c.tool_output);
        ASSERT_EQ(has_usage(e.usage()), c.usage);
        ASSERT_EQ(!e.error().empty(), c.error);
        /* finish_reason — единственное поле, где «пусто» неоднозначно:
         * пустое ожидание означает «у этого вида его нет». */
        ASSERT_EQ(e.finish_reason(), c.finish);
    }
}

/* То же самое с другой стороны: значения, а не признаки заполненности.
 * Иначе фабрика, кладущая чужую строку в своё поле, прошла бы тест
 * выше (аксессор всё равно пуст), а данные потерялись бы. */
TEST(llm_event_payload_holds_the_exact_values_it_was_given) {
    const json::JsonValue args = args_object();
    const ToolOutput out = read_output();
    const std::vector<Case> cases = all_cases();
    for (const Case& c : cases) {
        if (c.tool_name) {
            /* Имя есть и у ToolInputStart, и у ToolCall: при стриминге
             * провайдер присылает его вместе с началом аргументов. */
            ASSERT_EQ(c.event.tool_name(), std::string("read_file"));
        }
        if (c.arguments) {
            ASSERT_EQ(c.event.arguments().get_string("path"),
                      std::string("core/engine.cpp"));
            ASSERT_EQ(c.event.arguments().get_int("offset"), 42);
        }
        if (c.tool_output) {
            ASSERT_EQ(c.event.tool_output().title, out.title);
            ASSERT_EQ(c.event.tool_output().output, out.output);
            ASSERT_EQ(c.event.tool_output().truncated, out.truncated);
            ASSERT_EQ(c.event.tool_output().metadata.get_int("lines"), 404);
        }
        if (c.usage) {
            ASSERT_EQ(c.event.usage().input, 1000);
            ASSERT_EQ(c.event.usage().output, 200);
            ASSERT_EQ(c.event.usage().cache_read, 400);
            ASSERT_EQ(c.event.usage().total, 1200);
            ASSERT_TRUE(c.event.usage().invariants_hold());
        }
        if (c.call_id) {
            ASSERT_EQ(c.event.call_id(), std::string("call_0"));
        }
        if (c.error) {
            /* Текст ошибки лежит в том же поле, что и дельта, поэтому
             * проверяем через error(), а не через внутреннее поле. */
            ASSERT_TRUE(!c.event.error().empty());
        }
    }
    /* Ссылка на аргументы не должна быть общей: ToolCall создаётся из
     * готового JsonValue, и изменение вызывающего не должно менять
     * событие после копирования. */
    LlmEvent call = LlmEvent::tool_call("call_0", "read_file", args);
    json::JsonValue changed = call.arguments();
    changed.set("offset", 7);
    ASSERT_EQ(call.arguments().get_int("offset"), 42);
}

/* Копия события самостоятельна: цикл держит события в очереди и
 * разбирает их позже, уже после того, как провайдер прислал следующие. */
TEST(llm_event_copy_keeps_its_payload) {
    LlmEvent original = LlmEvent::text_delta("часть ");
    LlmEvent copy = original;
    original = LlmEvent::provider_error("сбой");
    ASSERT_EQ(copy.delta(), std::string("часть "));
    ASSERT_EQ(copy.kind(), LlmEventKind::TextDelta);
    ASSERT_TRUE(copy.error().empty());
}

/* ======================================================================
 * И5.2: Usage — счётчики, которые не перекрываются
 * ====================================================================== */

/* Главное свойство: один и тот же запрос, описанный двумя конвенциями
 * провайдеров, даёт ОДНО И ТО ЖЕ число токенов. */
TEST(usage_is_the_same_for_both_provider_conventions) {
    /* 1000 входных токенов, из них 400 из кэша, 200 выходных. */
    const Usage openai_style = Usage::from_provider(1000, 200, 400, 0, true);
    const Usage anthropic_style = Usage::from_provider(600, 200, 400, 0, false);

    ASSERT_EQ(openai_style.input, 1000);
    ASSERT_EQ(anthropic_style.input, 1000);
    ASSERT_EQ(openai_style.cache_read, 400);
    ASSERT_EQ(anthropic_style.cache_read, 400);
    ASSERT_EQ(openai_style.non_cached_input(), 600);
    ASSERT_EQ(anthropic_style.non_cached_input(), 600);
    ASSERT_EQ(openai_style.output, 200);
    ASSERT_EQ(anthropic_style.total, 1200);
    ASSERT_EQ(anthropic_style.total, openai_style.total);
    ASSERT_TRUE(openai_style.invariants_hold());
    ASSERT_TRUE(anthropic_style.invariants_hold());
}

/* Ключевой инвариант неперекрытия проверяется на числах, а не на
 * «total совпал»: иначе счётчик, испорченный вдвое, дал бы нулевую
 * разницу и проверка прошла бы. */
TEST(usage_counters_do_not_overlap) {
    const std::vector<Usage> samples = {
        Usage::from_provider(1000, 200, 400, 100, true),
        Usage::from_provider(1000, 200, 400, 100, false),
        Usage::from_provider(0, 0, 0, 0, true),
        Usage::from_provider(10, 0, 0, 0, false),
        Usage::from_provider(10, 7, 0, 0, true),
    };
    for (const Usage& u : samples) {
        ASSERT_TRUE(u.invariants_hold());
        ASSERT_EQ(u.non_cached_input() + u.cache_read + u.cache_write, u.input);
        ASSERT_EQ(u.total, u.input + u.output);
    }
    const Usage with_cache_write = Usage::from_provider(1000, 200, 400, 100, true);
    ASSERT_EQ(with_cache_write.cache_write, 100);
    ASSERT_EQ(with_cache_write.non_cached_input(), 500);
    ASSERT_EQ(with_cache_write.input, 1000);
    /* Кэш не может перекрывать сам себя: 400 чтения и 100 записи
     * вместе дают 500 из 1000, остальное — некэшированный вход. */
    ASSERT_EQ(with_cache_write.cache_read + with_cache_write.cache_write, 500);
}

TEST(usage_never_lets_reasoning_exceed_output) {
    Usage u = Usage::from_provider(1000, 200, 0, 0, true);
    u.set_reasoning(50);
    ASSERT_EQ(u.reasoning, 50);
    ASSERT_TRUE(u.invariants_hold());
    /* Часть провайдеров считает размышление ОТДЕЛЬНО от output. */
    u.set_reasoning(500);
    ASSERT_EQ(u.reasoning, 200);
    ASSERT_TRUE(u.invariants_hold());
    u.set_reasoning(-10);
    ASSERT_EQ(u.reasoning, 0);
    ASSERT_TRUE(u.invariants_hold());
    /* Размышление — часть output, поэтому total не меняется. */
    ASSERT_EQ(u.total, 1200);
}

TEST(usage_survives_a_broken_provider) {
    /* Отрицательные числа, кэш больше входа, мусор вместо объекта. */
    const Usage negative = Usage::from_provider(-100, -20, -5, -5, true);
    ASSERT_EQ(negative.input, 0);
    ASSERT_EQ(negative.output, 0);
    ASSERT_EQ(negative.cache_read, 0);
    ASSERT_EQ(negative.cache_write, 0);
    ASSERT_EQ(negative.total, 0);
    ASSERT_TRUE(negative.invariants_hold());
    ASSERT_TRUE(negative.empty());

    const Usage too_much_cache = Usage::from_provider(100, 20, 900, 900, true);
    ASSERT_TRUE(too_much_cache.cache_read <= too_much_cache.input);
    ASSERT_TRUE(too_much_cache.cache_read + too_much_cache.cache_write <=
               too_much_cache.input);
    ASSERT_TRUE(too_much_cache.invariants_hold());

    const Usage empty = Usage::from_json(json::JsonValue());
    ASSERT_TRUE(empty.empty());
    ASSERT_TRUE(empty.invariants_hold());

    json::JsonValue arr = json::JsonValue::array();
    arr.push_back(json::JsonValue(1));
    ASSERT_TRUE(Usage::from_json(arr).empty());

    json::JsonValue garbage = json::JsonValue::object();
    garbage.set("prompt_tokens", "много");
    ASSERT_TRUE(Usage::from_json(garbage).empty());
}

TEST(usage_from_json_reads_both_layouts) {
    /* OpenAI-совместимая: кэш лежит ВНУТРИ prompt_tokens. */
    json::JsonValue openai = json::JsonValue::object();
    openai.set("prompt_tokens", 1000);
    openai.set("completion_tokens", 200);
    openai.set("total_tokens", 9999);           /* провайдер врёт */
    json::JsonValue details = json::JsonValue::object();
    details.set("cached_tokens", 400);
    openai.set("prompt_tokens_details", details);
    json::JsonValue out_details = json::JsonValue::object();
    out_details.set("reasoning_tokens", 60);
    openai.set("completion_tokens_details", out_details);

    const Usage a = Usage::from_json(openai);
    ASSERT_EQ(a.input, 1000);
    ASSERT_EQ(a.cache_read, 400);
    ASSERT_EQ(a.non_cached_input(), 600);
    ASSERT_EQ(a.output, 200);
    ASSERT_EQ(a.reasoning, 60);
    /* total_tokens провайдера игнорируется: 9999 не равно 1200, и
     * согласованные счётчики полезнее чужих. */
    ASSERT_EQ(a.total, 1200);
    ASSERT_TRUE(a.invariants_hold());

    /* Anthropic-стиль: input_tokens — только некэшированная часть. */
    json::JsonValue anthropic = json::JsonValue::object();
    anthropic.set("input_tokens", 600);
    anthropic.set("output_tokens", 200);
    anthropic.set("cache_read_input_tokens", 400);
    anthropic.set("cache_creation_input_tokens", 100);

    const Usage b = Usage::from_json(anthropic, false);
    ASSERT_EQ(b.input, 1100);
    ASSERT_EQ(b.cache_read, 400);
    ASSERT_EQ(b.cache_write, 100);
    ASSERT_EQ(b.non_cached_input(), 600);
    ASSERT_EQ(b.output, 200);
    ASSERT_EQ(b.total, 1300);
    ASSERT_TRUE(b.invariants_hold());
}

TEST(usage_invariants_catch_a_counter_that_was_edited_by_hand) {
    /* Смысл invariants_hold() — ловить испорченные счётчики. Если бы он
     * всегда возвращал true, все проверки выше проверяли бы только то,
     * что поля вообще существуют. */
    Usage broken = Usage::from_provider(1000, 200, 400, 0, true);
    broken.input = 1400;                 /* кэш посчитан второй раз */
    ASSERT_FALSE(broken.invariants_hold());

    Usage bad_total = Usage::from_provider(1000, 200, 0, 0, true);
    bad_total.total = 999;
    ASSERT_FALSE(bad_total.invariants_hold());

    Usage bad_reasoning = Usage::from_provider(1000, 200, 0, 0, true);
    bad_reasoning.reasoning = 201;
    ASSERT_FALSE(bad_reasoning.invariants_hold());

    Usage bad_sign = Usage::from_provider(1000, 200, 0, 0, true);
    bad_sign.output = -5;
    ASSERT_FALSE(bad_sign.invariants_hold());
}

/* ======================================================================
 * И5.3: свёртка событий в ответ
 * ====================================================================== */

namespace {

/* Прогнать последовательность событий через свёртку. */
LlmResponse fold(const std::vector<LlmEvent>& events) {
    LlmResponse r;
    for (const LlmEvent& e : events) LlmResponse::reduce(r, e);
    return r;
}

json::JsonValue read_args() {
    json::JsonValue a = json::JsonValue::object();
    a.set("path", "core/engine.cpp");
    a.set("offset", 42);
    return a;
}

ToolOutput ok_output() {
    ToolOutput out;
    out.output = "прочитал 404 строки";
    return out;
}

/* События стримингового вызова инструмента: имя в ToolInputStart,
 * аргументы дельтами, закрытие ToolInputEnd. */
std::vector<LlmEvent> streamed_read_call() {
    return {
        LlmEvent::tool_input_start("call_0", "read_file"),
        LlmEvent::tool_input_delta("call_0", "{\"path\": \"core/en"),
        LlmEvent::tool_input_delta("call_0", "gine.cpp\", \"offset\": 42}"),
        LlmEvent::tool_input_end("call_0"),
    };
}

} // namespace

/* Свёртка — чистая функция: результат зависит только от потока событий.
 * Проверяется не «вызов не упал», а равенство двух независимых свёрток
 * и пустота после clear(). */
TEST(llm_response_reduce_is_pure) {
    const std::vector<LlmEvent> events = {
        LlmEvent::step_start(),
        LlmEvent::text_start(),
        LlmEvent::text_delta("Читаю "),
        LlmEvent::text_delta("файл"),
        LlmEvent::text_end(),
        LlmEvent::finish("stop", some_usage()),
    };
    const LlmResponse a = fold(events);
    LlmResponse b;
    for (const LlmEvent& e : events) LlmResponse::reduce(b, e);

    ASSERT_EQ(a.text(), std::string("Читаю файл"));
    ASSERT_EQ(b.text(), a.text());
    ASSERT_EQ(a.text_deltas(), 2);
    ASSERT_EQ(b.text_deltas(), 2);
    ASSERT_EQ(a.finish_reason(), std::string("stop"));
    ASSERT_EQ(b.usage().total, 1200);

    LlmResponse reused;
    reused.clear();
    ASSERT_TRUE(reused.empty());
    ASSERT_EQ(reused.text_deltas(), 0);
    for (const LlmEvent& e : events) LlmResponse::reduce(reused, e);
    ASSERT_EQ(reused.text(), a.text());
}

/* Текст и «размышление» больше не склеиваются. Раньше провайдер с
 * reasoning_content отдавал одно поле, и рассуждение попадало в историю
 * как обычный ответ — модель затем «повторяла» его пользователю. */
TEST(llm_response_keeps_reasoning_apart_from_the_answer) {
    const LlmResponse r = fold({
        LlmEvent::reasoning_start(),
        LlmEvent::reasoning_delta("Сначала посмотрю "),
        LlmEvent::reasoning_delta("структуру."),
        LlmEvent::reasoning_end(),
        LlmEvent::text_start(),
        LlmEvent::text_delta("Вот ответ."),
        LlmEvent::text_end(),
    });
    ASSERT_EQ(r.reasoning(), std::string("Сначала посмотрю структуру."));
    ASSERT_EQ(r.text(), std::string("Вот ответ."));
}

/* Символ, разрезанный дельтой, склеивается. Это не «аккуратность»:
 * разрыв в середине UTF-8 даёт битый текст в истории, который
 * sanitize_utf8 потом заменит на «замены» — то есть модель увидит мусор
 * вместо своего же ответа. */
TEST(llm_response_reassembles_a_symbol_split_between_deltas) {
    const std::string cyrillic = "Случай";      /* 6 букв по 2 байта */
    ASSERT_EQ(cyrillic.size(), size_t(12));
    LlmResponse r;
    LlmResponse::reduce(r, LlmEvent::text_start());
    LlmResponse::reduce(r, LlmEvent::text_delta(cyrillic.substr(0, 1)));
    /* Незавершённый символ в текст НЕ попадает: половина символа в UI
     * выглядит как мусор, и в буфер обмена уедет именно она. */
    ASSERT_EQ(r.text(), std::string(""));
    LlmResponse::reduce(r, LlmEvent::text_delta(cyrillic.substr(1, 4)));
    /* «С» достроен, «лу» целиком — в тексте всегда валидный UTF-8. */
    ASSERT_TRUE(text::is_valid_utf8(r.text()));
    LlmResponse::reduce(r, LlmEvent::text_delta(cyrillic.substr(5)));
    LlmResponse::reduce(r, LlmEvent::text_end());
    ASSERT_EQ(r.text(), cyrillic);
    ASSERT_TRUE(text::is_valid_utf8(r.text()));

    /* Тот же случай на эмодзи (4 байта), разбитом на три дельты. */
    const std::string emoji = "🚀 Поехали";
    const LlmResponse r2 = fold({
        LlmEvent::text_delta(emoji.substr(0, 2)),
        LlmEvent::text_delta(emoji.substr(2, 1)),
        LlmEvent::text_delta(emoji.substr(3)),
        LlmEvent::text_end(),
    });
    ASSERT_EQ(r2.text(), emoji);
    ASSERT_TRUE(text::is_valid_utf8(r2.text()));
}

/* Недобранный хвост отдаётся при закрытии блока и при Finish: провайдер,
 * оборвавший ответ посреди символа, не должен терять его молча. */
TEST(llm_response_flushes_an_incomplete_tail_at_the_end) {
    /* Только первый байт «С», и ни TextEnd, ни Finish не будет. */
    LlmResponse r = fold({LlmEvent::text_start(),
                          LlmEvent::text_delta("\xD0")});
    ASSERT_EQ(r.text(), std::string(""));
    LlmResponse::reduce(r, LlmEvent::text_end());
    ASSERT_EQ(r.text().size(), size_t(1));

    /* Тот же хвост, но провайдер не прислал TextEnd — только Finish. */
    LlmResponse r2 = fold({LlmEvent::text_delta("a\xD0"),
                           LlmEvent::finish("length")});
    ASSERT_EQ(r2.text().size(), size_t(2));
    ASSERT_EQ(r2.finish_reason(), std::string("length"));
}

/* Битый байт посреди потока не должен ОСТАНОВИТЬ поток. Проверяется
 * ПОСЛЕ КАЖДОЙ дельты, а не по итогу: при битом байте, удержанном до
 * конца шага, итоговый текст был бы верным, а весь остаток ответа не
 * показался бы в UI никогда — агент выглядел бы зависшим. Различие
 * видно только между дельтами. */
TEST(llm_response_does_not_stall_on_a_corrupt_byte) {
    LlmResponse r;
    LlmResponse::reduce(r, LlmEvent::text_delta("начало "));
    ASSERT_EQ(r.text(), std::string("начало "));

    /* Битый байт уходит в текст как есть, но НЕ удерживает поток: он не
     * «начало символа», ждать нечего, и следующая дельта обязана быть
     * видна сразу. */
    LlmResponse::reduce(r, LlmEvent::text_delta("\xFF"));
    ASSERT_EQ(r.text().size(), std::string("начало ").size() + 1);

    LlmResponse::reduce(r, LlmEvent::text_delta("конец"));
    ASSERT_TRUE(r.text().find("конец") != std::string::npos);
    ASSERT_TRUE(r.text().find("начало") == 0);

    LlmResponse::reduce(r, LlmEvent::text_end());
    /* Мусор убран при очистке, а не потерян молча. */
    const std::string clean = text::sanitize_utf8(r.text());
    ASSERT_TRUE(text::is_valid_utf8(clean));
    ASSERT_TRUE(clean.find("начало") == 0);
    ASSERT_TRUE(clean.find("конец") != std::string::npos);
}

TEST(llm_response_collects_a_streamed_tool_call) {
    LlmResponse r = fold(streamed_read_call());
    ASSERT_EQ(r.tool_calls().size(), size_t(1));
    const LlmToolCall& c = r.tool_calls()[0];
    ASSERT_EQ(c.call_id, std::string("call_0"));
    ASSERT_EQ(c.name, std::string("read_file"));
    ASSERT_EQ(c.arguments.get_string("path"), std::string("core/engine.cpp"));
    ASSERT_EQ(c.arguments.get_int("offset"), 42);
    ASSERT_TRUE(c.input_closed);
    ASSERT_FALSE(c.from_native_call);
    ASSERT_TRUE(c.runnable());
    /* Нет результата — значит, вызов ещё открыт, и ход нельзя считать
     * завершённым (условие И5.8 опирается на это). */
    ASSERT_TRUE(r.has_open_tool_call());
    ASSERT_FALSE(r.ok());
}

TEST(llm_response_takes_a_native_tool_call_without_deltas) {
    const LlmResponse r = fold({
        LlmEvent::tool_call("call_x", "write_file", read_args()),
    });
    ASSERT_EQ(r.tool_calls().size(), size_t(1));
    const LlmToolCall& c = r.tool_calls()[0];
    ASSERT_EQ(c.name, std::string("write_file"));
    ASSERT_TRUE(c.from_native_call);
    ASSERT_EQ(c.arguments.get_int("offset"), 42);
    ASSERT_TRUE(c.runnable());
}

/* Провайдер без нативного вызова не присылает ToolInputEnd: аргументы
 * закрывает StepFinish. Без коммита по StepFinish такой вызов остался бы
 * навсегда «незакрытым вводом» и потерялся бы целиком. */
TEST(llm_response_commits_args_even_without_tool_input_end) {
    std::vector<LlmEvent> events = streamed_read_call();
    events.pop_back();                     /* убираем ToolInputEnd */
    LlmResponse r = fold(events);
    ASSERT_EQ(r.tool_calls().size(), size_t(1));
    /* Пока шаг не закрыт, ввод действительно не закрыт: цикл не должен
     * исполнить вызов по неполным аргументам. */
    ASSERT_TRUE(!r.tool_calls()[0].input_closed);
    ASSERT_TRUE(!r.tool_calls()[0].runnable());

    LlmResponse::reduce(r, LlmEvent::step_finish());
    ASSERT_TRUE(r.tool_calls()[0].input_closed);
    ASSERT_EQ(r.tool_calls()[0].arguments.get_string("path"),
              std::string("core/engine.cpp"));
    ASSERT_TRUE(r.tool_calls()[0].runnable());
}

/* Мусор в аргументах не должен ни бросить исключение, ни исчезнуть:
 * вызов закрывается ошибкой с сохранением сырья. */
TEST(llm_response_closes_a_malformed_tool_input_with_an_error) {
    const LlmResponse r = fold({
        LlmEvent::tool_input_start("call_0", "read_file"),
        LlmEvent::tool_input_delta("call_0", "{путь: core"),
        LlmEvent::tool_input_end("call_0"),
    });
    ASSERT_EQ(r.tool_calls().size(), size_t(1));
    const LlmToolCall& c = r.tool_calls()[0];
    ASSERT_TRUE(c.failed);
    ASSERT_TRUE(c.finished);
    ASSERT_TRUE(!c.runnable());
    ASSERT_TRUE(c.error.find("аргумент") != std::string::npos);
    ASSERT_EQ(c.raw_input, std::string("{путь: core"));
    /* Ошибка закрыла вызов, поэтому ход не «завис» на нём. */
    ASSERT_FALSE(r.has_open_tool_call());
}

TEST(llm_response_flags_a_call_without_arguments_or_name) {
    /* Имя есть, аргументов нет — исполнять нечего. */
    const LlmResponse no_args = fold({
        LlmEvent::tool_input_start("call_0", "todowrite"),
        LlmEvent::tool_input_end("call_0"),
    });
    ASSERT_TRUE(no_args.tool_calls()[0].failed);
    ASSERT_TRUE(!no_args.tool_calls()[0].runnable());

    /* Аргументы есть, имени нет (провайдер без нативного вызова). */
    const LlmResponse no_name = fold({
        LlmEvent::tool_input_start("call_1", ""),
        LlmEvent::tool_input_delta("call_1", "{\"path\":\"a.txt\"}"),
        LlmEvent::tool_input_end("call_1"),
    });
    ASSERT_TRUE(no_name.tool_calls()[0].failed);
    ASSERT_TRUE(!no_name.tool_calls()[0].runnable());
}

/* Два вызова, результаты приходят в обратном порядке: привязка строго по
 * call_id, иначе результат второго вызова ушёл бы к первому. */
TEST(llm_response_matches_results_to_calls_by_id) {
    LlmResponse r = fold({
        LlmEvent::tool_input_start("call_0", "read_file"),
        LlmEvent::tool_input_delta("call_0", "{\"path\":\"a.txt\"}"),
        LlmEvent::tool_input_end("call_0"),
        LlmEvent::tool_input_start("call_1", "read_file"),
        LlmEvent::tool_input_delta("call_1", "{\"path\":\"b.txt\"}"),
        LlmEvent::tool_input_end("call_1"),
    });
    ASSERT_EQ(r.tool_calls().size(), size_t(2));
    ASSERT_EQ(r.tool_calls()[0].call_id, std::string("call_0"));
    ASSERT_EQ(r.tool_calls()[1].call_id, std::string("call_1"));

    ToolOutput second;
    second.output = "содержимое b.txt";
    LlmResponse::reduce(r, LlmEvent::tool_result("call_1", second));
    ASSERT_TRUE(!r.tool_calls()[0].finished);
    ASSERT_TRUE(r.tool_calls()[1].finished);
    ASSERT_TRUE(r.has_open_tool_call());

    LlmResponse::reduce(r, LlmEvent::tool_result("call_0", ok_output()));
    ASSERT_TRUE(r.tool_calls()[0].finished);
    ASSERT_EQ(r.tool_calls()[0].output.output, std::string("прочитал 404 строки"));
    ASSERT_EQ(r.tool_calls()[1].output.output, std::string("содержимое b.txt"));
    ASSERT_FALSE(r.has_open_tool_call());
    ASSERT_TRUE(r.ok());
}

TEST(llm_response_tells_a_refusal_from_an_error) {
    LlmResponse refused = fold({
        LlmEvent::tool_input_start("call_0", "deploy"),
        LlmEvent::tool_input_delta("call_0", "{\"path\":\"/\"}"),
        LlmEvent::tool_input_end("call_0"),
        LlmEvent::tool_error("call_0", "[отказ] пользователь не разрешил"),
    });
    ASSERT_TRUE(refused.tool_calls()[0].failed);
    ASSERT_TRUE(refused.tool_calls()[0].finished);
    ASSERT_TRUE(refused.tool_calls()[0].output.output.empty());
    ASSERT_EQ(refused.tool_calls()[0].error,
              std::string("[отказ] пользователь не разрешил"));

    /* Сбой провайдера — это НЕ отказ инструмента: у него нет вызова. */
    LlmResponse broken = fold({
        LlmEvent::text_start(),
        LlmEvent::text_delta("часть ответа"),
        LlmEvent::provider_error("таймаут LLM-вызова"),
    });
    ASSERT_EQ(broken.error(), std::string("таймаут LLM-вызова"));
    ASSERT_TRUE(broken.tool_calls().empty());
    ASSERT_FALSE(broken.ok());
    /* Ответ, полученный ДО сбоя, не теряется: агент и пользователь видят,
     * что было сказано, и чем закончилось. */
    ASSERT_EQ(broken.text(), std::string("часть ответа"));
}

/* Результат вызова, которого не было, нельзя молча привязать к чему
 * попало: иначе он просто исчез бы из хода. */
TEST(llm_response_flags_a_result_without_a_call) {
    LlmResponse r = fold({LlmEvent::tool_result("call_ghost", ok_output())});
    ASSERT_EQ(r.tool_calls().size(), size_t(1));
    ASSERT_TRUE(r.tool_calls()[0].failed);
    ASSERT_TRUE(r.tool_calls()[0].finished);
    ASSERT_TRUE(!r.tool_calls()[0].error.empty());
    ASSERT_FALSE(r.has_open_tool_call());
}

/* Текст появляется между вызовами: один ход — это текст И вызовы, а не
 * «что-то одно». */
TEST(llm_response_keeps_text_around_tool_calls) {
    std::vector<LlmEvent> events = {
        LlmEvent::text_start(), LlmEvent::text_delta("Сначала "),
        LlmEvent::text_delta("прочитаю."), LlmEvent::text_end(),
    };
    for (const LlmEvent& e : streamed_read_call()) events.push_back(e);
    events.push_back(LlmEvent::text_start());
    events.push_back(LlmEvent::text_delta("Готово."));
    events.push_back(LlmEvent::text_end());

    const LlmResponse r = fold(events);
    ASSERT_EQ(r.text(), std::string("Сначала прочитаю.Готово."));
    ASSERT_EQ(r.tool_calls().size(), size_t(1));
    ASSERT_TRUE(r.has_open_tool_call());
    /* Ход не пустой даже без результата вызова. */
    ASSERT_FALSE(r.empty());
}

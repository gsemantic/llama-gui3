#pragma once

/*
 * llm_event.h — Событийная модель ответа LLM (И5.1, порт
 * packages/llm/src/schema/events.ts:209).
 *
 * Что не так было до И5. Ответ модели приходил одной строкой:
 * LlmReply{content, finish_reason, prompt_tokens, completion_tokens}, и
 * цикл знал про строку. Отсюда три беды, все три тихо искажали смысл:
 *
 *  1. Текст и «размышление» модели неразличимы. Провайдеры, у которых
 *     reasoning_content и content приходят разными полями, склеивались
 *     в одну строку, а reasoning попадал в историю как обычный ответ.
 *  2. Вызов инструмента и текст приходили одним куском, поэтому цикл
 *     вырезал вызов из текста эвристикой extract_action — шесть
 *     догадок вместо одного факта от провайдера.
 *  3. Части ответа не имели границ. Два соседних вызова инструментов
 *     давали два блока текста, и «последний ответ» определялся
 *     эвристикой, а не тем, что блок закрыт.
 *
 * Событийная модель снимает все три: приём ответа перестаёт быть
 * разбором строки и становится свёрткой событий. Строка по-прежнему
 * доступна (её собирает LlmResponse::reduce, И5.3), но она теперь
 * ВЫВОД, а не вход.
 *
 * Шестнадцать вариантов — как в opencode, и по одной причине: набор
 * покрывает поток провайдера целиком, а любое добавление состояния
 * (retry, compaction) — это новый вид события, а не флаг внутри
 * TextDelta. Иначе та же болезнь, что D12: одно и то же описано в
 * двух местах, и места расходятся.
 *
 * Почему свой тип, а не std::variant: проект — C++17 без новых
 * зависимостей (SESSION_START.md, «чего не делать»). Вариант
 * эмулируется закрытым конструктором и фабриками: собрать событие
 * можно ТОЛЬКО фабрикой, поэтому у события не бывает «текст у
 * ToolCall» или «ошибка у Finish». Аксессор, вызванный не на своём
 * виде события, возвращает пустое значение, а не чужое поле.
 *
 * Про UTF-8: дельта провайдера приходит БАЙТАМИ и может разрезать
 * многобайтовый символ пополам. Склейкой по кодам символов занимается
 * свёртка (LlmResponse::reduce, И5.3); здесь это зафиксировано
 * письменно, потому что потерянный байт не даёт ошибки — он тихо
 * портит текст в истории и в UI.
 */

#include "json.h"
#include "tool.h"

#include <string>
#include <vector>

namespace coder {

/* --- И5.2: учёт токенов ---
 *
 * Раньше учёт был двумя целыми: prompt_tokens и completion_tokens, и
 * каждый потребитель считал сам — панель складывала «сколько всего»,
 * компакшн (И7) считал долю контекста по-своему, и числа в двух местах
 * расходились. Кроме того, счётчики ПЕРЕКРЫВАЛИСЬ: у OpenAI-совместимых
 * провайдеров prompt_tokens уже включает кэшированные токены, поэтому
 * сложение «prompt + cache_read» давало двойной счёт, а вычитание давало
 * отрицательный «некэшированный» остаток.
 *
 * Поэтому счётчики названы так, что НЕ перекрываются, а правила
 * закреплены инвариантами, а не прозой:
 *   input  = non_cached_input() + cache_read + cache_write
 *   reasoning ≤ output   (размышление — часть ответа, не сверх него)
 *   total   = input + output
 *
 * Меняются счётчики ТОЛЬКО через from_provider()/from_json() и
 * set_reasoning(). Ручная правка поля ломает инвариант, и проверка
 * invariants_hold() об этом скажет: поэтому потребитель не вычисляет
 * ничего сам, а читает готовое.
 */
struct Usage {
    /* Все входные токены запроса, кэш ВКЛЮЧЁН. */
    long long input = 0;
    /* Сгенерированные токены ответа, reasoning ВКЛЮЧЁН. */
    long long output = 0;
    /* Часть output, потраченная на «размышление». */
    long long reasoning = 0;
    long long cache_read = 0;
    long long cache_write = 0;
    /* Готовая сумма input + output. Не вычисляется потребителем. */
    long long total = 0;

    /* Входные токены вне кэша (может быть 0). */
    long long non_cached_input() const;
    /* Доля кэша во входе, 0..1 — для панели токенов (И11.9). */
    double cache_ratio() const;
    bool empty() const;
    /* Проверка инвариантов. Тесты обязаны звать её на собранном
     * значении: единственный способ узнать, что счётчики не
     * перекрылись, — спросить у них же. */
    bool invariants_hold() const;

    /* Сборка из чисел провайдера.
     *
     * cache_counted_in_prompt — тот самый параметр, который нигде больше
     * не должен решаться: у OpenAI-совместимых провайдеров prompt_tokens
     * ВКЛЮЧАЕТ кэш, у Anthropic-стиля input_tokens НЕ включает. Ошибка
     * здесь даёт двойной счёт в панели и заниженную оценку контекста в
     * компакшне (И7), то есть тихую порчу данных в обоих местах. */
    static Usage from_provider(long long prompt_tokens,
                               long long completion_tokens,
                               long long cache_read_tokens = 0,
                               long long cache_write_tokens = 0,
                               bool cache_counted_in_prompt = true);

    /* Сборка из JSON-объекта usage.
     *
     * Поддерживаются обе раскладки: OpenAI-совместимая
     * (prompt_tokens / completion_tokens, кэш в
     * prompt_tokens_details.cached_tokens) и Anthropic-стиля
     * (input_tokens / output_tokens / cache_read_input_tokens /
     * cache_creation_input_tokens). Итоговое поле провайдера
     * (total_tokens) ИГНОРИРУЕТСЯ: провайдеры считают его по-разному, а
     * молчаливое расхождение с input+output хуже, чем единое
     * согласованное число.
     *
     * Пустой объект или мусор даёт пустой Usage, а не исключение:
     * отсутствие метрик не должно ронять ход. */
    static Usage from_json(const json::JsonValue& usage_json,
                           bool cache_counted_in_prompt = true);

    /* Размышление приходит отдельным числом и у части провайдеров не
     * входит в output. Значение подрезается до output: «больше
     * размышления, чем ответа» в панели выглядит как баг, который никто
     * не может объяснить. */
    void set_reasoning(long long tokens);
};

/* --- И6.8: что сломалось ---
 *
 * Не «ошибка» одним словом. Отмена пользователем и отказ провайдера — разные
 * вещи: первое не повторяют, второе повторяют, и обе должны быть видны в
 * UI и в транскрипте раздельно. Список закрытый: новый вид — это правка
 * этого перечисления, а не новая строка в тексте ошибки.
 *
 * Объявлено ПЕРЕД LlmEvent, а не рядом с LlmResponse: с И11.14 вид сбоя
 * несёт и событие (им помечен отказ `aborted()`), иначе понадобился бы
 * второй носитель того же факта, и они разошлись бы — как уже разошлись
 * объявление и присваивание. */
enum class FailureKind {
    None = 0,   /* сбоя нет */
    Provider,   /* провайдер не ответил: сеть, таймаут, отказ хоста */
    Tool,       /* упал инструмент: у вызова есть ToolError */
    Aborted,    /* пользователь нажал «стоп» */
    /* И7.5: контекст исчерпан настолько, что даже сводка не влезает.
     * Отдельный вид, а не Provider: с провайдером всё в порядке, вопрос
     * в размере, и лечится он иначе — сжатием истории (или её началом
     * заново). Назвать его «провайдер» значило бы показать пользователю
     * «ошибка сети» там, где сеть работает. Полная таксономия ошибок —
     * И12.5; здесь только тот вид, который уже понадобился. */
    ContextOverflow
};

/* Человекочитаемое имя — для UI и логов, чтобы не собирать его в строках. */
const char* failure_kind_name(FailureKind kind);

/* Вид события. Порядок значения не имеют (события сравниваются по
 * имени), но он зафиксирован, чтобы switch по нему можно было
 * проверять компилятором на полноту. */
enum class LlmEventKind : int {
    StepStart = 0,   // начало шага: провайдер начал генерировать ответ
    TextStart,       // открыт блок обычного текста
    TextDelta,       // кусок текста внутри блока
    TextEnd,         // блок текста закрыт
    ReasoningStart,  // открыт блок «размышления»
    ReasoningDelta,  // кусок размышления
    ReasoningEnd,    // блок размышления закрыт
    ToolInputStart,  // провайдер начал присылать аргументы вызова
    ToolInputDelta,  // кусок аргументов (сырой JSON, может быть обрезан)
    ToolInputEnd,    // аргументы присланы целиком
    ToolCall,        // разобранный вызов: пора выполнять инструмент
    ToolResult,      // результат инструмента (ToolOutput целиком)
    ToolError,       // вызов не состоялся: отказ режима/разрешения/ошибка
    StepFinish,      // шаг закрыт, провайдер закончил генерацию
    Finish,          // весь ответ закрыт, пришёл finish_reason
    ProviderError,   // провайдер не ответил: сеть, таймаут, отказ хоста

    kCount           // не событие: столько их всего
};

/* Имя вида события — для логов, UI и сообщений об ошибках. */
const char* llm_event_kind_name(LlmEventKind kind);

/* Событие ответа LLM.
 *
 * Экземпляр создаётся только фабриками: конструктор закрыт. Это и есть
 * «вариантный тип» без std::variant — невозможность собрать событие с
 * несовместимыми полями обеспечивается компилятором на уровне списка
 * фабрик, а не соглашением.
 */
class LlmEvent {
public:
    /* --- Фабрики (по одной на вид события) --- */
    static LlmEvent step_start();
    static LlmEvent text_start();
    static LlmEvent text_delta(std::string delta);
    static LlmEvent text_end();
    static LlmEvent reasoning_start();
    static LlmEvent reasoning_delta(std::string delta);
    static LlmEvent reasoning_end();
    static LlmEvent tool_input_start(std::string call_id,
                                     std::string tool_name);
    static LlmEvent tool_input_delta(std::string call_id, std::string delta);
    static LlmEvent tool_input_end(std::string call_id);
    static LlmEvent tool_call(std::string call_id, std::string tool,
                              json::JsonValue args);
    static LlmEvent tool_result(std::string call_id, ToolOutput output);
    static LlmEvent tool_error(std::string call_id, std::string error);
    static LlmEvent step_finish();
    static LlmEvent finish(std::string finish_reason);
    /* Финал с метриками: usage приходит вместе с закрытием ответа, а не
     * с каждой дельтой — иначе провайдер, присылающий usage в последнем
     * чанке, а счётчики дельт, дал бы сто накоплений. */
    static LlmEvent finish(std::string finish_reason, Usage usage);
    static LlmEvent provider_error(std::string error);
    /* И11.14: отказ хода, который устроил НЕ провайдер, а пользователь.
     *
     * Отдельная фабрика, а не флаг у provider_error: «стоп» и «сеть
     * упала» — разные вещи с разными последствиями (первое не
     * повторяют, второе повторяют), и И6.8 завела под них РАЗНЫЕ виды
     * `FailureKind`. Пока отмену нечем было пометить, `Aborted`
     * объявлялся, печатался в `failure_kind_name` — и не присваивался
     * нигде: список видов был наполовину вымышленным.
     *
     * Вид события остаётся `ProviderError` — ход не состоялся, и разбор
     * протокола не должен знать про кнопку «стоп». Различаются они
     * видом СБОЯ, а не видом события. */
    static LlmEvent aborted(std::string reason);

    LlmEventKind kind() const { return kind_; }
    const char* kind_name() const { return llm_event_kind_name(kind_); }
    bool is(LlmEventKind k) const { return kind_ == k; }

    /* --- Данные события ---
     *
     * Каждый аксессор отвечает только на свой вид события; на чужом
     * возвращает пустое значение. Обратная сторона: «пусто» значит и
     * «нет значения», поэтому порядок проверки всегда kind() → данные,
     * а не наоборот.
     */

    /* TextDelta / ReasoningDelta / ToolInputDelta. */
    const std::string& delta() const;

    /* ToolInputStart/Delta/End, ToolCall, ToolResult, ToolError.
     *
     * Идентификатор вызова обязателен: без него результат инструмента
     * не к чему привязать, когда вызовов было два. Провайдеры, которые
     * нумеруют вызовы индексом вместо строки (llama-server), адаптер
     * обязан превратить индекс в call_0, call_1, … — иначе две
     * параллельные сборки аргументов склеятся в одну. */
    const std::string& call_id() const;

    /* ToolInputStart / ToolCall.
     *
     * ToolInputStart несёт имя инструмента: при стриминге провайдер
     * присылает аргументы дельтами, а ToolCall (с именем и уже
     * разобранными аргументами) приходит не всегда. Без имени на
     * ToolInputStart стриминговый вызов остался бы неисполнимым. */
    const std::string& tool_name() const;
    /* ToolCall: разобранные аргументы (JsonValue-объект). Сырые
     * дельты ToolInputDelta сюда НЕ попадают: неразобранный кусок
     * JSON нельзя ни проверить по схеме, ни показать в UI как вызов. */
    const json::JsonValue& arguments() const;

    /* ToolResult: результат инструмента целиком, включая заголовок,
     * метаданные и признак усечения (ToolOutput, И4.10). */
    const ToolOutput& tool_output() const;

    /* Finish. */
    const std::string& finish_reason() const;
    /* Finish: метрики хода. Для остальных видов — пустой Usage. */
    const Usage& usage() const;

    /* ToolError / ProviderError. */
    const std::string& error() const;

    /* И6.8: вид сбоя СОБЫТИЯ. У события он нужен с И11.14: отмена
     * пользователем — то же самое «ход не состоялся», то есть тот же вид
     * события, но разный вид сбоя, и различает их именно это поле. */
    FailureKind failure() const { return failure_; }

private:
    explicit LlmEvent(LlmEventKind k) : kind_(k) {}

    LlmEventKind kind_;
    /* Поле для дельт и текстов ошибок. */
    std::string text_;
    std::string call_id_;
    std::string tool_name_;
    json::JsonValue arguments_;
    ToolOutput tool_output_;
    std::string finish_reason_;
    Usage usage_;
    /* И11.14: см. failure(). Значение по умолчанию — None, то есть
     * «сбой не классифицирован», и reduce() домысливает Provider для
     * обычного провайдерского отказа. */
    FailureKind failure_ = FailureKind::None;
};


/* --- И5.3: свёртка событий в ответ ---
 *
 * Один вызов инструмента, собранный из потока событий. Это НЕ сессионная
 * часть сообщения (она живёт в core/message.h, И5.4): здесь только то, что
 * пришло от провайдера и что исполнил цикл, — потому что условие
 * завершения хода (И5.8) спрашивает именно «есть ли незакрытый вызов».
 */
struct LlmToolCall {
    std::string call_id;
    std::string name;
    /* Сырые дельты ToolInputDelta, склеенные как есть. Нужны для
     * разбора и для показа в UI при сбое разбора. */
    std::string raw_input;
    /* Разобранные аргументы. Заполняется на ToolInputEnd либо сразу на
     * ToolCall, если провайдер умеет нативный вызов инструмента. */
    json::JsonValue arguments;
    ToolOutput output;
    /* Текст отказа: запрещён режимом, не разрешён пользователем, ошибка
     * инструмента. */
    std::string error;
    /* Аргументы закрыты (ToolInputEnd / ToolCall). */
    bool input_closed = false;
    /* Вызов закрыт результатом ИЛИ ошибкой. */
    bool finished = false;
    /* Закрыт именно ошибкой, а не успехом. */
    bool failed = false;
    /* Вызов пришёл одним куском (нативный tool calling), а не дельтами. */
    bool from_native_call = false;

    /* Аргументы пригодны к запуску инструмента. */
    bool runnable() const {
        return input_closed && !finished && !failed && !name.empty();
    }
};

/* Свёртка потока событий в один ответ.
 *
 * reduce — ЧИСТАЯ функция: не ходит ни в сеть, ни в файлы, ни в состояние
 * движка и не берёт мьютексов. Именно поэтому её можно юнит-тестировать
 * без сети и без Engine, а цикл — собирать из событий в тесте, а не в
 * живом прогоне.
 */
class LlmResponse {
public:
    void clear();

    /* Свёртка одного события в накопленное состояние. */
    static void reduce(LlmResponse& state, const LlmEvent& event);

    /* Текст ответа без вызовов инструментов. */
    const std::string& text() const { return text_; }
    /* «Размышление» модели — отдельно от текста (впервые доступно
     * отдельно; раньше провайдеры, присылающие reasoning_content,
     * склеивали его с content в одну строку). */
    const std::string& reasoning() const { return reasoning_; }
    const std::string& finish_reason() const { return finish_reason_; }
    const Usage& usage() const { return usage_; }
    /* Текст ProviderError. */
    const std::string& error() const { return error_; }

    /*
     * ЧТО именно сломалось (И6.8).
     *
     * Одного текста ошибки мало: «прервано пользователем» и «провайдер
     * отказал» — разные события с разными последствиями. Первое — намерение
     * пользователя, и повторять ход бессмысленно; второе — сбой, и ход надо
     * повторить. Сливая их в «ошибка», цикл объявлял задачу выполненной
     * после отказа провайдера, а UI показывал одно и то же сообщение и про
     * «стоп», и про HTTP 429.
     *
     * Вид хранится здесь, а не в отдельном флаге движка, потому что ответ
     * УЖЕ знает, какое событие его породило, — повторная классификация
     * разбирала бы текст ошибки заново и рано или поздно разошлась бы с
     * событием.
     */
    FailureKind failure() const { return failure_; }
    const std::vector<LlmToolCall>& tool_calls() const { return calls_; }

    /* Ответ без ошибки провайдера и без незакрытых вызовов. */
    bool ok() const { return error_.empty() && !has_open_tool_call(); }
    /* Есть вызов, результат которого ещё не пришёл. Условие завершения
     * хода (И5.8) требует, чтобы таких не было: иначе цикл объявит
     * задачу выполненной, потеряв результат инструмента. */
    bool has_open_tool_call() const;
    /* Ни текста, ни вызовов, ни ошибки — ход не дал ничего. */
    bool empty() const;
    /* Сколько дельт склеено в текст: диагностика для лога (в UI пойдёт
     * в И11, как «показано 400 символов за 87 дельт»). */
    long long text_deltas() const { return text_deltas_; }

private:
    /* Склеить поток UTF-8: из буфера выносится только то, что уже не
     * может быть началом разрезанного символа. */
    static void drain_utf8_tail(std::string& out, std::string& tail);
    LlmToolCall& call_for(const std::string& call_id);
    LlmToolCall* find_call(const std::string& call_id);
    /* Аргументы склеены и пришло время их разобрать. */
    void commit_pending_calls();

    std::string text_;
    std::string reasoning_;
    /* Недоставленные байты в конце потока: символ, разрезанный дельтой. */
    std::string text_tail_;
    std::string reasoning_tail_;
    std::string finish_reason_;
    std::string error_;
    /* И6.8: вид сбоя. Ставится тем же событием, что и текст. */
    FailureKind failure_ = FailureKind::None;
    Usage usage_;
    std::vector<LlmToolCall> calls_;
    long long text_deltas_ = 0;
};

/* Равенство ответов для тестов (И5.9). Вид сбоя (И6.8) входит в сравнение:
 * два ответа с одинаковым текстом ошибки, но разным видом сбоя, — разные
 * ответы, и сверка обоих путей обязана это замечать. */
inline bool operator==(const LlmResponse& a, const LlmResponse& b) {
    return a.text() == b.text()
        && a.reasoning() == b.reasoning()
        && a.finish_reason() == b.finish_reason()
        && a.error() == b.error()
        && a.failure() == b.failure()
        && a.usage().input == b.usage().input
        && a.usage().output == b.usage().output
        && a.usage().cache_read == b.usage().cache_read
        && a.usage().cache_write == b.usage().cache_write
        && a.usage().reasoning == b.usage().reasoning
        && a.usage().total == b.usage().total
        && a.tool_calls().size() == b.tool_calls().size();
}
inline bool operator!=(const LlmResponse& a, const LlmResponse& b) {
    return !(a == b);
}

} // namespace coder

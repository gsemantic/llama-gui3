#pragma once

/*
 * message.h — сессионная модель сообщений (И5.4, порт
 * packages/schema/src/v1/session.ts и packages/llm/src/message-v2.ts).
 *
 * До И5 вся история агента была `std::vector<ChatMsg>` — список строк с
 * меткой роли. Этого хватало, пока ответ модели приходил одним куском.
 * Как только у ответа появились части (текст, размышление, вызов
 * инструмента, его результат), список строк перестал быть источником
 * истины по трём причинам:
 *
 *  1. Порядок терялся. Ответ «думаю → вызвал инструмент → получил
 *     результат → отвечаю» раскладывался в четыре отдельных сообщения,
 *     и модель на следующем шаге видела текст после результата
 *     инструмента как отдельную реплику.
 *  2. Части нельзя было адресовать. «Покажи мне, что делал второй
 *     вызов» требовало знать, какой строке принадлежит результат, — а
 *     принадлежность выражалась префиксом «RESULT [read_file]:»,
 *     то есть текстом, который можно было подделать и случайно
 *     испортить (в И4.7 это уже стоило двух BOM, см. отклонение №25).
 *  3. Отмена и «идёт ли инструмент» невыразимы. У строки нет
 *     состояния, а у вызова инструмента оно есть по определению.
 *
 * Поэтому сообщение — это контейнер ЧАСТЕЙ, у каждой части свой
 * идентификатор, а у сообщения — parent_id на сообщение, его
 * породившее. Всё, что раньше выражалось префиксом строки, теперь
 * выражается структурой; текстовые маркеры остались только в одном
 * месте — при сборке истории для модели (to_model_messages, И5.10).
 *
 * Идентификаторы монотонны и с префиксами msg_/prt_/ses_
 * (см. id_prefix.h, И5.5) — они же порядок сортировки, поэтому
 * «история» и «отсортированная история» совпадают.
 */

#include "json.h"
#include "llm_event.h"
#include "tool.h"

#include <string>
#include <vector>

namespace coder {

/* Роль сообщения. Строка, а не enum: роль приходит от модели и из
 * файла сессии, а нераспознанное значение должно быть видно, а не
 * молча превратиться в дефолт (то же правило, что у статусов
 * TodoItem, И4.6).
 *
 * Константы, а не namespace role: поле Message::role называется так же,
 * и внутри методов `role::kUser` означал бы «поле role как
 * пространство имён» — ошибка компиляции вместо чтения. */
inline constexpr const char* kRoleUser = "user";
inline constexpr const char* kRoleAssistant = "assistant";
inline constexpr const char* kRoleSystem = "system";

/* Состояние части-вызова инструмента: 4 состояния вместо прежней
 * пары «есть текст результата / нет текста». Прежде всего не хватало
 * РАЗЛИЧЕНИЯ между «не начал» и «начал, но не закончил»: цикл,
 * прерванный посреди инструмента, выглядел как «инструмента не
 * было», и результат терялся молча (условие завершения хода в И5.8
 * построено ровно на этом различии). */
enum class ToolState {
    Pending,     // вызов получен, инструмент ещё не запускался
    Running,     // инструмент работает
    Completed,   // результат получен
    Error,       // отказ режима/разрешения или ошибка инструмента
};

const char* tool_state_name(ToolState s);

/* Часть сообщения. Как и LlmEvent (И5.1), вариант эмулирован: у части
 * есть вид, и собирается она только фабриками.
 *
 * Виды: Text, Reasoning, Tool, StepStart, StepFinish, Patch, Retry,
 * Compaction, Subtask. Последние пять пока не порождаются циклом —
 * И10 (Patch), И12.6 (Retry), И7 (Compaction), И8 (Subtask) — но
 * объявлены здесь, чтобы файл сессии не пришлось менять по формату
 * при их появлении: старые сессии должны читаться новым кодом, а не
 * наоборот. */
enum class PartKind {
    Text = 0,
    Reasoning,
    Tool,
    StepStart,
    StepFinish,
    Patch,
    Retry,
    Compaction,
    Subtask,
};

const char* part_kind_name(PartKind k);

/* Часть сообщения. */
class MessagePart {
public:
    /* Фабрики по виду. */
    static MessagePart text(std::string text);
    static MessagePart reasoning(std::string text);
    static MessagePart tool(std::string call_id, std::string tool_name,
                            json::JsonValue args = json::JsonValue::object());
    /* С сырым блоком вызова, каким его написала модель: в транскрипт
     * уходит именно он (см. to_model_messages). */
    static MessagePart tool(std::string call_id, std::string tool_name,
                            json::JsonValue args, std::string raw_block);
    static MessagePart step_start(std::string step_name = std::string());
    static MessagePart step_finish(std::string step_name = std::string());
    static MessagePart patch(std::string snapshot_hash,
                             json::JsonValue files = json::JsonValue::array());
    static MessagePart retry(int attempt, int next_attempt_in_ms);
    static MessagePart compaction(std::string summary,
                                  std::vector<std::string> replaced_ids);
    static MessagePart subtask(std::string task_id, std::string subagent);

    PartKind kind() const { return kind_; }
    const char* kind_name() const { return part_kind_name(kind_); }
    bool is(PartKind k) const { return kind_ == k; }

    /* Text / Reasoning / Compaction. */
    const std::string& text() const;
    /* Tool. */
    const std::string& call_id() const;
    const std::string& tool_name() const;
    const json::JsonValue& args() const;
    /* Tool: сырой блок вызова, каким его написала модель. Собственный
     * аксессор, а не text(): text() по виду отсекает всё, что не
     * Text/Reasoning/Compaction, и сырой блок был бы полем, которое
     * нельзя прочитать. */
    const std::string& raw_call() const;
    ToolState state() const { return state_; }
    const char* state_name() const { return tool_state_name(state_); }
    /* Вызов ещё не дал исхода: не начат (Pending) или работает (Running).
     * Ровно это различие отличает «инструмента не было» от «инструмент
     * не закончен», и на нём стоит условие завершения хода (И5.8). */
    bool is_open() const {
        return is(PartKind::Tool) &&
               (state_ == ToolState::Pending || state_ == ToolState::Running);
    }
    /* Tool: результат (ToolOutput) либо текст отказа в error(). */
    const ToolOutput& output() const;
    const std::string& error() const;
    bool has_result() const;

    /* Шаги: StepStart / StepFinish. */
    const std::string& step_name() const;
    /* Retry: номер попытки и пауза до следующей. */
    int attempt() const { return attempt_; }
    int next_attempt_in_ms() const { return next_attempt_in_ms_; }
    /* Patch: хеш снапшота и список изменившихся файлов (И10). */
    const std::string& snapshot_hash() const;
    const json::JsonValue& files() const;
    /* Subtask: идентификатор дочерней задачи и имя субагента (И8). */
    const std::string& task_id() const;
    const std::string& subagent() const;
    /* Compaction: какие части сообщений свёрнуты в сводку. */
    const std::vector<std::string>& replaced_ids() const;

    /* Перевод состояния части-вызова: Pending → Running → Completed /
     * Error. Вынесено в метод, потому что «пометить результатом» из
     * двух мест — это два места, где забудут про error.
     *
     * На части другого вида — no-op: состояние инструмента не может
     * быть у текстовой части, и молчаливый no-op здесь опаснее отказа,
     * поэтому он зафиксирован тестом. */
    MessagePart& set_running();
    MessagePart& set_result(ToolOutput out);
    MessagePart& set_error(std::string error);

    /* Конструктора по умолчанию НЕТ намеренно: у части всегда есть вид,
     * а пустая часть без вида — это «часть, о которой забыли сказать, что
     * она». Парсер файла поэтому добавляет части в список сразу (append),
     * а не разбирает «в объект». */
private:
    explicit MessagePart(PartKind k) : kind_(k) {}

    PartKind kind_;
    std::string text_;
    std::string call_id_;
    std::string tool_name_;
    json::JsonValue args_;
    json::JsonValue files_;
    ToolOutput output_;
    ToolState state_ = ToolState::Pending;
    std::string error_;
    std::string step_name_;
    std::string snapshot_hash_;
    std::string task_id_;
    std::string subagent_;
    std::vector<std::string> replaced_ids_;
    int attempt_ = 0;
    int next_attempt_in_ms_ = 0;
};

/* Сообщение сессии: контейнер частей плюс происхождение.
 *
 * parent_id — идентификатор сообщения, инициировавшего ход. По нему
 * цикл отвечает на вопрос «на какое сообщение отвечает этот ход» (это
 * и есть условие завершения в И5.8), а UI — «что было до этого». */
struct Message {
    std::string id;
    std::string role;            // kRoleUser / kRoleAssistant / kRoleSystem
    std::string parent_id;       // пусто у первого сообщения
    std::vector<MessagePart> parts;

    /* Есть ли части, которые ещё не закрыты. Незакрытая часть
     * инструмента означает, что ход нельзя считать законченным. */
    bool has_open_tool_part() const;
    /* Текстовая часть: склейка всех Text (без размышления и без
     * инструментов). Именно это попадает в результат задачи. */
    std::string text() const;
    /* Текст для истории модели: части в исходном порядке, каждая в
     * своём формате. Порядок НЕ переставляется — он и есть смысл хода. */
    std::string to_model_string() const;
    /* К этому сообщению ведут дети? (для UI-дерева, И11.3) */
    bool is_assistant() const { return role == kRoleAssistant; }
};

/* Сообщение в том виде, в каком его видит модель.
 *
 * Отдельная структура, а не ChatMsg из engine.h: message.h не включает
 * engine.h (то же основание, что у ToolContext в tool.h) — иначе любое
 * подключение модели сообщений тянуло бы весь движок. Размер полей тот
 * же, переход тривиален.
 *
 * Роль отдельного сообщения: пользователь — это то, что ввёл человек
 * (задача, ответ на вопрос) и результат инструмента. */
struct ModelMessage {
    std::string role;
    std::string content;
};

/* Сборка истории в сообщения для модели.
 *
 * Живёт здесь, а не в цикле, по конкретной причине: ход, в котором
 * были вызовы инструментов, ПРИНЦИПИАЛЬНО не помещается в одно
 * сообщение. Вызов — реплика ассистента, результат — реплика
 * пользователя, и склеенное сообщение сказало бы модели, что результат
 * написала она сама. Если это разбиение делать в AgentLoop, то маркер
 * «RESULT [инструмент]:» окажется в коде цикла — а сегодня он там
 * уже есть, и это ровно тот класс расхождения, который стоил D2
 * (инструмент есть, а протокол описан в другом файле).
 *
 * Правила, и каждое выбрано против конкретной беды:
 *   - размышление в транскрипт НЕ попадает: модель не получает обратно
 *     собственный «внутренний» текст, а строка формата для него не
 *     существует, и заводить её без записи в kBaseSystemPrompt — значит
 *     повторить D2. Появится ли она — решает И12 (промпты по семейству
 *     модели), и тогда же появится строка в промпте;
 *   - служебные виды (StepStart/StepFinish/Patch/Retry/Subtask) в
 *     транскрипт не попадают: у них нет формата, о котором модель
 *     знает. Когда их включит своя итерация (И7, И8, И10, И12.6), она
 *     же обязана добавить сюда строку формата И запись в промпт;
 *   - пустые куски не отправляются: сообщение с пустым содержимым
 *     половина провайдеров считает ошибкой протокола;
 *   - порядок частей сохраняется. Он и есть смысл хода: «сначала
 *     посмотрел, потом сказал». */
std::vector<ModelMessage> to_model_messages(const std::vector<Message>& history);

} // namespace coder

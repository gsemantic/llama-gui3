#pragma once

/*
 * tool.h — Типизированный инструмент (И1.3–1.6, решение D-3).
 *
 * До И1 инструмент был `std::function<std::string(const ToolArgs&)>`
 * плюс фиксированная структура ToolArgs на 8 строковых слотов. Отсюда
 * были две беды, обе зафиксированы в плане:
 *   - добавление параметра требовало правки минимум в 4 файлах
 *     (объявление в module_api.h, извлечение в tool_protocol.cpp,
 *      строка в промпте, разбор в агенте) — и эти правки расходились;
 *   - у реестра не было НИКАКИХ метаданных об инструменте, кроме
 *     описания строкой, поэтому режимы (research/plan) приходилось
 *     проверять внутри самих инструментов. Проверки стояли в 4 из 50
 *     инструментов, а bash, git_commit, deploy, cron_add,
 *     systemd_restart, docker_run, wp_create_site, pip_install шли мимо.
 *
 * Теперь инструмент описывается декларативно (ToolDef): имя, описание,
 * JSON-схема параметров и битовая маска ToolFlags. Режимы enforce'ятся
 * в одном месте — ToolRunner::run (И1.7) — и покрывают все 50
 * инструментов, а не 4.
 *
 * Схема — не «полный» JSON Schema, а подмножество, которого хватает
 * для проверки аргументов и генерации описания для модели:
 *   {"type":"object",
 *    "properties":{"path":{"type":"string","description":"..."}},
 *    "required":["path"],
 *    "additionalProperties":false}
 * Поддерживаемые type: string, integer, number, boolean, array, object.
 * Поддерживаемые ограничения: minimum, maximum, enum.
 */

#include "json.h"

#include <functional>
#include <string>
#include <vector>

#include "abort.h"

namespace coder {

/* --- И1.4: флаги инструмента (битовая маска) ---
 *
 * Флаги отвечают на два разных вопроса, и их важно не путать:
 *   «что инструмент УМЕЕТ»  — EXECUTES (запускает код), NETWORK (ходит
 *                             в сеть), WRITES_FILES (трогает файлы);
 *   «что инструмент ДЕЛАЕТ» — READ_ONLY (ничего не меняет) и
 *                             DESTRUCTIVE (необратимое изменение).
 *
 * Именно поэтому READ_ONLY — единственный флаг, по которому решается
 * допуск в режиме research: git_status запускает git, но не меняет
 * ничего, и запрещать его в режиме «только чтение» бессмысленно.
 * Обратная сторона: инструмент, помеченный WRITES_FILES, но без
 * READ_ONLY, в research не пройдёт автоматически — при неверной
 * разметке это отказ, а не дыра, что правильно.
 */
enum ToolFlag : unsigned {
    TF_NONE         = 0u,
    /* Не меняет ничего: только читает, даже если что-то запускает.
     * Единственный флаг, разрешённый в режиме research. */
    TF_READ_ONLY    = 1u << 0,
    /* Создаёт/изменяет/удаляет файлы. */
    TF_WRITES_FILES = 1u << 1,
    /* Запускает код: shell, git, docker, systemctl, ssh, интерпретатор. */
    TF_EXECUTES     = 1u << 2,
    /* Ходит в сеть: HTTP, wp rest, headless-браузер. */
    TF_NETWORK      = 1u << 3,
    /* Изменение, которое инструмент не может отменить сам: деплой,
     * перезапуск сервиса, создание БД, установка пакета. По этому
     * флагу И2 будет требовать подтверждения пользователя. */
    TF_DESTRUCTIVE  = 1u << 4,
    /* Долгая операция: bash, docker build, pip install.
     * Учитывается таймаутом (И4.8) и политикой разрешений (И2). */
    TF_SLOW         = 1u << 5,

    /* Служебный флаг И1.3–И1.7: инструмент зарегистрирован старым
     * способом (без схемы и флагов) и ждёт миграции на И1.8.
     * Enforcement относится к нему fail-closed: в research/plan такой
     * инструмент не вызывается. После И1.8 флага быть не должно —
     * это проверяет тест no_unclassified_tools. */
    TF_UNCLASSIFIED = 1u << 6,
};

inline unsigned tf_has(unsigned flags, ToolFlag f) {
    return (flags & static_cast<unsigned>(f)) != 0u;
}

/* Что запрещено в режиме плана: запуск кода и необратимые операции.
 * WRITES_FILES здесь разрешён — запись перехватывает
 * ToolContext::propose_write и превращает правку в предложение.
 *
 * Для research запрет выражается не маской, а требованием TF_READ_ONLY:
 * список «безопасных» инструментов меняется вместе с набором флагов,
 * и перечислять его руками — значит забыть какой-нибудь. */
constexpr unsigned kPlanForbidden = TF_UNCLASSIFIED | TF_EXECUTES | TF_DESTRUCTIVE;
constexpr unsigned kAllFlags = TF_READ_ONLY | TF_WRITES_FILES | TF_EXECUTES |
                               TF_NETWORK | TF_DESTRUCTIVE | TF_SLOW;

const char* tool_flag_names(unsigned flags);   // для отладки и сообщений

/* --- И1.3: результат работы инструмента --- */
struct ToolOutput {
    /* Короткий заголовок для UI/таймлайна (И11): «read a.txt», «git status». */
    std::string title;
    /* Текст, который увидит модель в RESULT. */
    std::string output;
    /* Структурные сведения: exit code, число совпадений, предложенная
     * правка и т.п. И11 будет рисовать по ним, И5 — хранить. */
    json::JsonValue metadata;
    /* Вывод обрезан лимитом (универсальное усечение — И4.10). */
    bool truncated = false;
};

/* --- Контекст выполнения ---
 *
 * ToolContext намеренно НЕ включает engine.h: tools_registry.h включён
 * из engine.h, и полноценный тип здесь создал бы циклическую
 * зависимость. Поэтому здесь только указатели и forward declaration,
 * а реализация методов — в core/tool.cpp.
 */
struct EngineState;
struct HostCallbacks;

class ToolContext {
public:
    ToolContext(EngineState& state, HostCallbacks& cb) : state_(&state), cb_(&cb) {}

    EngineState& state() const;
    HostCallbacks& callbacks() const;

    /* Текущий режим агента: 0=Code, 1=Research, 2=Review. */
    int mode() const;
    /* Режим «сначала план»: правки не применяются, а предлагаются. */
    bool plan_mode() const;
    bool research_mode() const;
    bool review_mode() const;

    std::string project_dir() const;
    std::string php_bin() const;

    /* Гейт на путь за пределами проекта. Пустая строка — можно,
     * иначе текст отказа (и агент переведён в ожидание разрешения). */
    std::string check_external_permission(const std::string& abs_path);

    /* И1.7: единая точка записи файла для агента.
     *
     * В plan_mode файл НЕ пишется, а предлагается пользователю
     * (state.pending) — ровно то поведение, которое раньше было
     * продублировано в write_file / search_replace / edit_file /
     * undo_edit четырьмя разными способами. Инструмент не решает, что
     * делать, — это решает контекст. Возвращает true, если запись
     * предложена, а не выполнена. */
    bool propose_write(const std::string& rel_path, const std::string& content);

    /* И6.7: отмена текущего хода.
     *
     * Инструмент, который умеет прекращаться (bash), спрашивает это в цикле
     * ожидания. Без этого «стоп» обрывал только ожидание LLM, а уже
     * запущенная команда продолжала работать: агент уходил на следующий
     * шаг, а команда в фоне продолжала писать файлы — то есть «стоп» был
     * враньём ровно там, где он особенно нужен.
     *
     * nullptr означает «отменять нечем» (ход без токена) — и инструмент
     * обязан трактовать это как «не отменять», а не падать. */
    AbortToken* abort() const;

private:
    EngineState* state_;
    HostCallbacks* cb_;
};

/* --- И1.3: обработчик инструмента --- */
using ToolHandler2 = std::function<ToolOutput(const json::JsonValue& args, ToolContext& ctx)>;

/* --- И1.3: описание инструмента --- */
struct ToolDef {
    std::string name;
    std::string description;
    /* Динамическое описание (И8.13): если задано, оно ЗАМЕЩАЕТ `description`
     * при сборке каталога, а не дополняет его.
     *
     * Заменой, а не дополнением, потому что два описания одного
     * инструмента разошлись бы при первой правке любого из них, а
     * каталог попадает в КАЖДЫЙ запрос: модель увидела бы оба.
     *
     * Нужно там, где описание зависит от состояния: у `task` список
     * субагентов меняется от сессии к сессии и от агента к агенту, и
     * вызывающий не должен видеть в списке тех, кого звать запрещено
     * (И8.10). */
    std::function<std::string()> describe_dynamic;
    /* JSON-схема параметров (объект). Пустая — валидация пропускается. */
    json::JsonValue parameters;
    /* Битовая маска ToolFlag. */
    unsigned flags = TF_UNCLASSIFIED;
    /* Ключ разрешения для системы И2 (permission_key): «write», «bash»,
     * «deploy»… Пока пусто — заполняет И2 (задача 2.9). */
    std::string permission_key;
    ToolHandler2 handler;
};

/* --- Сборка схемы параметров ---
 *
 * Без этой обёртки объявление схемы занимало бы 15 строк на параметр
 * (вложенные объекты) — ровно та цена, ради снятия которой И1 и
 * существует. Здесь каждый параметр — одна строка.
 */
class SchemaBuilder {
public:
    SchemaBuilder& str(const char* name, const char* description);
    SchemaBuilder& integer(const char* name, const char* description);
    SchemaBuilder& integer_range(const char* name, const char* description,
                                 long long min_value, long long max_value);
    SchemaBuilder& number(const char* name, const char* description);
    SchemaBuilder& boolean(const char* name, const char* description);
    SchemaBuilder& string_enum(const char* name, const char* description,
                               const std::vector<std::string>& values);
    /* Массив объектов (И4.6, todowrite). Схема описывает форму массива, но
     * не разбирает содержимое элементов: проверку значений делает сам
     * инструмент, где можно сказать модели, какое поле и почему не так. */
    SchemaBuilder& object_array(const char* name, const char* description,
                                const char* item_description);
    /* Пометить параметр обязательным. */
    SchemaBuilder& required(const char* name);
    /* Запретить неизвестные ключи. По умолчанию они разрешены и
     * игнорируются: локальные 7B-модели постоянно добавляют «note»,
     * «comment», «path_» — жёсткий отказ заставлял бы их сжигать
     * шаги впустую. */
    SchemaBuilder& strict();

    json::JsonValue build() const;

private:
    json::JsonValue props_ = json::JsonValue::object();
    json::JsonValue required_ = json::JsonValue::array();
    bool strict_ = false;
};

/* Собрать схему с одним параметром path — самый частый случай. */
json::JsonValue schema_path_only(const char* description);

/* --- И1.6: валидация аргументов по схеме --- */

/* Проверить args против схемы tool. detail — человекочитаемая причина
 * (на английском: модель получает её в тексте ошибки). */
bool validate_tool_args(const json::JsonValue& parameters,
                        const json::JsonValue& args,
                        std::string& detail);

/* Сообщение об ошибке в стиле opencode (tools/registry.ts:135):
 * модель должна понять, что именно исправить, и переписать вызов. */
std::string invalid_arguments_message(const std::string& tool_name,
                                      const std::string& detail);

/* --- И1.7: политика режимов ---
 *
 * Единственное место в плагине, где решается, можно ли вызвать
 * инструмент в текущем режиме агента. Правило раньше было продублировано
 * внутри 4 инструментов из 50 (write_file, search_replace, edit_file,
 * undo_edit), из-за чего 8 опасных инструментов шли мимо: bash,
 * git_commit, deploy, cron_add, systemd_restart, docker_run,
 * wp_create_site, pip_install (дефект D4), а режим Research вообще
 * был только текстом в промпте (D3).
 *
 * Семантика:
 *   Research (mode == 1) — только чтение. Пропускаются исключительно
 *       инструменты с TF_READ_ONLY. Всё, что запускает код, пишет
 *       файлы или меняет что-то необратимо, — запрещено. Сеть
 *       разрешена, если инструмент при этом только читает: web_fetch и
 *       headless_render — это чтение.
 *   План (plan_mode) — запрещены запуск кода и необратимые операции.
 *       Инструменты, пишущие файлы, не блокируются: они выполняются,
 *       но ToolContext::propose_write переводит запись в предложение
 *       правки, и пользователь её подтверждает.
 *   Code / Review — ограничений нет.
 *
 * Инструмент без флагов (TF_UNCLASSIFIED, ещё не мигрированный на И1.8)
 * запрещён в обоих режимах: неизвестно, что он делает, а «пропустить
 * на всякий случай» здесь означало бы дыру.
 */
std::string check_tool_mode_policy(const std::string& tool_name, unsigned flags,
                                   int mode, bool plan_mode);

/* То же для ToolDef. */
std::string check_tool_mode_policy(const std::string& tool_name,
                                   const ToolDef& def, int mode, bool plan_mode);

/* --- И2.7: политика разрешений --- */

/* Значение, по которому правила судят о вызове инструмента.
 *
 * Правило пишется человеком («read *.env → спросить»), и оно должно
 * видеть ровно то, что имеет смысл спрашивать: путь для файловых
 * инструментов, команду для shell, URL для сети. Поэтому значение
 * берётся из первого же известного параметра по фиксированному
 * списку имён, а не из всего дампа аргументов: дамп в правиле
 * превратился бы в нечитаемую простыню и перестал бы совпадать при
 * любом лишнем поле. Список имён — одна функция, а не размазанная по
 * инструментам логика.
 *
 * Если подходящего параметра нет (инструменту нечего предъявлять) —
 * «*»: значит, решение принимается по самому факту вызова инструмента,
 * и точечное правило по нему не сработает никогда. */
std::string permission_pattern(const ToolDef& def, const json::JsonValue& args);

/* Ключ разрешения инструмента. Пустой permission_key — ещё не
 * заполнен (И2.9); тогда берётся имя инструмента, иначе поведение
 * зависело бы от того, в какой задаче плана инструмент появился. */
std::string permission_key_of(const ToolDef& def);

/* --- Скрытие инструментов по правилам (И2.8, И8.7) ---
 *
 * Один вопрос — «какие инструменты показывать модели» — задаётся в
 * двух местах: правилами сессии (PermissionEngine::visible_tools) и
 * правилами агента (agent::Info, из них собирается каталог субагента).
 * Написать это дважды — значит получить две функции об одном факте,
 * которые разойдутся при первом же изменении правила скрытия.
 *
 * denied_whole_key отвечает на вопрос «запрещён ли ключ ЦЕЛИКОМ».
 * Точечный запрет (паттерн) инструмент НЕ прячет: он запрещает
 * значения, а сам инструмент нужен.
 *
 * Порядок результата — как во входном списке, а не по алфавиту:
 * каталог уходит в промпт целиком, и перестановка меняла бы его
 * содержимое без единой смены правил. */
std::vector<std::string> visible_tool_names(
        const std::vector<ToolDef>& all,
        const std::function<bool(const std::string& permission_key)>& denied_whole_key);

/* Канонический ключ разрешения для того, что написал ЧЕЛОВЕК (И8.3).
 *
 * В описании агента ключом может быть названа и группа инструментов
 * («bash»), и конкретный инструмент («write_file»), и — если
 * конфиг писали по порту — имя из чужого словаря («edit»). Всё это
 * обязано сойтись к одному ключу, иначе правило «edit → запретить»
 * молча не сработало бы ни на одном инструменте: агент выглядел бы
 * настроенным, а правил бы файлы.
 *
 * Порядокlookup'а: имя инструмента → его permission_key (источник
 * истины — ToolDef, он же то, по чему Enforcement и спрашивает);
 * затем само имя, если оно УЖЕ ключ («bash», «read», «write»);
 * затем таблица псевдонимов порта. Неизвестное имя возвращается как
 * есть, а known = false: правило по несуществующему ключу безвредно
 * и заработает, если инструмент появится, — но пользователь должен
 * узнать о нём из замечания, а не из того, что правило «не сработало». */
std::string canonical_permission_key(const std::string& name, bool* known = nullptr);

/* Что предложить кнопке «всегда». Берётся само значение, а не его
 * каталог: правило вида «<path>/*» из «/etc/passwd» разрешило бы всему
 * /etc, и ошибка была бы не гипотетической. Узкое «всегда для этой
 * команды» — И3.5 (CommandPolicy::always_pattern), там у паттерна
 * появляется осмысленный безопасный префикс. */
std::string permission_suggested_pattern(const ToolDef& def,
                                         const std::string& pattern);

} // namespace coder

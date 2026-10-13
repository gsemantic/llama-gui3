/*
 * test_native_tools.cpp — И12.2: нативный вызов инструмента.
 *
 * Что здесь проверяется и почему именно так
 * -----------------------------------------
 * Нативный вызов — механизм с двумя половинами, и обе обязаны быть
 * проверены: ЗАПРОС (инструменты уходят провайдеру) и ПРОТОКОЛ (промпт
 * перестаёт требовать текстовый блок, когда вызов подтверждён).
 *
 * Проверка только второй половины дала бы зелёный цвет коду, который
 * шлёт идеальный промпт провайдеру, не умеющему нативный вызов, и
 * получает вместо ответа пустоту. Проверка только первой — обратное:
 * инструменты уходят, модель отвечает текстом, а плагин считает, что
 * переключился, и убирает единственный способ позвать инструмент.
 *
 * ПОЧЕМУ ОТКАЗ, А НЕ «ВЫЗОВА НЕ БЫЛО». Нативный вызов выключает только
 * явный отказ провайдера. Отсутствие вызова доказательством не является:
 * модель может просто ответить текстом, и тогда нативный вызов
 * выключался бы вслед за каждой нетривиальной репликой человека. Проверка
 * на «нет вызова → выключить» здесь была бы проверкой неверного поведения.
 *
 * ПОЧЕМУ `tools` ПРОВЕРЯЕТСЯ ЧЕРЕЗ ЗАПРОС, А НЕ ЧЕРЕЗ ФУНКЦИЮ. Сборка
 * списка инструментов — чистая функция, но ценность её в том, что список
 * ДОЕЗЖАЕТ до провайдера тем же путём, что и остальной запрос. Проверка
 * «build_tool_specs_json вернула непустую строку» была бы зелёной при
 * функции, которую никто не зовёт, — ровно класс executeShortcut.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/agent_registry.h"
#include "../core/engine.h"
#include "../core/json.h"
#include "../core/llm_client.h"
#include "../core/prompts.h"
#include "../core/tool.h"
#include "../core/tools_registry.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace coder;

namespace {

/* --- Хост-имитация --- */

std::string g_request_json;
std::string g_done_json;
bool g_tool_deltas = false;

void reset_host_script() {
    g_request_json.clear();
    g_done_json = "{\"ok\":1,\"content\":\"\",\"finish_reason\":\"stop\"}";
    g_tool_deltas = false;
}

HostCallbacks recording_host() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&,
                         std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.path_data_dir = []() { return std::string(); };
    cb.path_config_dir = []() { return std::string(); };
    cb.chat_event = [](const std::string&) {};
    cb.settings_get = [](const std::string&, const std::string& d) {
        return d;
    };
    cb.settings_set = [](const std::string&, const std::string&) {};
    /* Стриминг без сети: хост отвечает заранее заданным финальным JSON и,
     * по требованию, дельтами нативного вызова. */
    cb.llm_chat_stream =
        [](const std::string&, const std::vector<ModelMessage>&,
           const std::string& request_json, std::function<void(void*)>,
           std::function<void(const char*, int)> on_delta,
           std::function<void(const char*, const char*, const char*)> on_tool_delta,
           std::function<void(const std::string&)> on_done) -> bool {
        g_request_json = request_json;
        if (g_tool_deltas) {
            on_tool_delta("call_0", "read_file", "{\"path\":\"a.txt\"}");
        }
        on_done(g_done_json);
        return true;
    };
    return cb;
}

/* Состояние движка протекает между проверками (Engine — синглтон,
 * правило 5). Возвращается всё, что проверка меняет, INCLUDING вывод о
 * нативном вызове: он влияет на протокол промпта, и унаследованный
 * Confirmed переписал бы текстовый промпт у следующей проверки. */
struct EngineStateGuard {
    Engine& e;
    NativeToolSupport native_tools = NativeToolSupport::Unknown;
    RunScope scope;
    std::string project_dir;
    std::string agent_system_prompt;
    std::string cached;
    bool dirty = true;

    explicit EngineStateGuard(Engine& engine) : e(engine) {
        std::lock_guard<std::mutex> lk(e.state().mtx);
        native_tools = e.state().native_tools;
        scope = e.state().scope;
        project_dir = e.state().project_dir;
        agent_system_prompt = e.state().agent_system_prompt;
        cached = e.state().cached_system_prompt;
        dirty = e.state().prompt_dirty;
    }
    ~EngineStateGuard() {
        {
            std::lock_guard<std::mutex> lk(e.state().mtx);
            e.state().native_tools = native_tools;
            e.state().scope = scope;
            e.state().project_dir = project_dir;
            e.state().agent_system_prompt = agent_system_prompt;
            e.state().cached_system_prompt = cached;
            e.state().prompt_dirty = dirty;
        }
        /* Колбэк настроек в синглтоне указывает на таблицу проверки, а
         * таблица умирает вместе с ней. */
        e.callbacks().settings_get =
            [](const std::string&, const std::string& d) { return d; };
    }
};

/* Подготовка: пустой проект, никакого агента, никакого профиля. */
void prepare(Engine& eng) {
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().scope = RunScope();
        eng.state().agent_system_prompt.clear();
        eng.state().project_dir.clear();
    }
    eng.invalidate_prompt_cache();
}

std::string fetch_once(Engine& eng) {
    reset_host_script();
    std::vector<LlmEvent> events;
    LlmClient::fetch(recording_host(), "sys", {{"user", "x"}}, events,
                     nullptr, 1000);
    (void)eng;
    return g_request_json;
}

/* Сколько инструментов объявлено в запросе. */
size_t tools_in_request(const std::string& request_json) {
    json::JsonValue parsed;
    if (!json::JsonValue::parse(request_json, parsed)) return 0;
    const json::JsonValue& tools = parsed.get("tools");
    return tools.is_array() ? tools.size() : 0;
}

/* Инструмент по имени из разобранного запроса; nullptr — нет. */
const json::JsonValue* tool_by_name(const json::JsonValue& tools,
                                    const std::string& name) {
    if (!tools.is_array()) return nullptr;
    for (size_t i = 0; i < tools.size(); ++i) {
        const json::JsonValue& entry = tools.at(i);
        if (entry.get("function").get("name").as_string() == name) {
            return &entry;
        }
    }
    return nullptr;
}

bool request_has_tool(const std::string& request_json, const std::string& name) {
    return request_json.find("\"name\":\"" + name + "\"") != std::string::npos;
}

} // namespace

TEST(инструменты_уходят_в_запрос_без_профиля_harness) {
    /* Именно БЕЗ профиля: профиль и инструменты — разные вещи, и ранний
     * выход «профиля нет — запрос пуст» уводил бы инструменты вместе с
     * параметрами генерации. */
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(recording_host());
    prepare(eng);

    const std::string request_json = fetch_once(eng);
    if (tools_in_request(request_json) == 0) {
        std::cerr << "  в запросе нет инструментов; длина запроса "
                  << request_json.size() << std::endl;
    }
    ASSERT_TRUE(tools_in_request(request_json) > 0);
}

TEST(спецификация_инструмента_несёт_имя_описание_и_схему) {
    /* Форма — OpenAI-совместимая, и разбирает её ХОСТ (apply_request_json).
     * Проверяется по-настоящему, а не «строка непустая»: при неверной
     * форме хост либо отверг бы весь запрос, либо молча выбросил бы
     * инструменты — и агент остался бы без единого способа позвать их. */
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(recording_host());
    prepare(eng);

    const std::string request_json = fetch_once(eng);
    ASSERT_TRUE(request_has_tool(request_json, "read_file"));

    json::JsonValue parsed;
    ASSERT_TRUE(json::JsonValue::parse(request_json, parsed));
    const json::JsonValue& tools = parsed.get("tools");
    ASSERT_TRUE(tools.is_array());
    ASSERT_TRUE(tools.size() > 0);

    const json::JsonValue* entry = tool_by_name(tools, "read_file");
    if (entry == nullptr) {
        std::cerr << "  read_file не объявлен среди инструментов; объявлено "
                  << tools.size() << std::endl;
    }
    ASSERT_TRUE(entry != nullptr);
    ASSERT_EQ(entry->get("type").as_string(), std::string("function"));
    ASSERT_TRUE(!entry->get("function").get("description").as_string().empty());
    /* Схема обязана быть ОБЪЕКТОМ с properties: так её проверяет
     * validate_tool_args, и так её ждёт хост. Пустой схемы здесь быть не
     * может — провайдер отверг бы спецификацию без parameters. */
    const json::JsonValue& params = entry->get("function").get("parameters");
    ASSERT_TRUE(params.is_object());
    const json::JsonValue& props = params.get("properties");
    ASSERT_TRUE(props.is_object());
    /* Именно СВОЯ схема, а не пустой объект-заглушка.
     *
     * Сборщик подставляет `{"type":"object","properties":{}}` инструменту
     * без схемы — иначе провайдер отверг бы спецификацию. Проверка на
     * «properties — объект» прошла бы и при заглушке, то есть не увидела
     * бы, что у read_file параметры исчезли. Настоящий параметр виден
     * только в его собственной схеме. */
    if (props.get("path").is_null()) {
        std::cerr << "  у read_file нет собственного параметра path; "
                     "схема подменена заглушкой" << std::endl;
    }
    ASSERT_TRUE(props.get("path").is_object());
    ASSERT_EQ(props.get("path").get("type").as_string(), std::string("string"));
}

TEST(отказ_провайдера_убирает_инструменты_со_следующего_хода) {
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(recording_host());
    prepare(eng);

    /* Первый ход: инструменты были, отказа нет. */
    ASSERT_TRUE(tools_in_request(fetch_once(eng)) > 0);

    /* Провайдер отказал: хост повторил запрос без них и сообщил ключом. */
    /* ЧИСЛОМ, а не true: плагин читает ключ json::int_, и булево значение
     * вернуло бы 0, то есть «отказа не было». Хост пишет так же, и здесь
     * проверяется тот же контракт — с той стороны, которая его читает. */
    g_done_json = "{\"ok\":1,\"content\":\"\",\"finish_reason\":\"stop\","
                  "\"tools_rejected\":1}";
    std::vector<LlmEvent> events;
    LlmClient::fetch(recording_host(), "sys", {{"user", "x"}}, events,
                     nullptr, 1000);
    ASSERT_TRUE(eng.native_tool_support() == NativeToolSupport::Rejected);

    /* Следующий ход — уже без инструментов. */
    ASSERT_EQ(tools_in_request(fetch_once(eng)), size_t(0));
}

TEST(нативный_вызов_подтверждается_дельтой_а_не_фактом_пустого_ответа) {
    /* Дельта on_tool_delta приходит ТОЛЬКО от провайдера: разобранный
     * текстовый блок даёт тот же вызов в ответе, но дельты не порождает.
     * Различие и есть доказательство, и без него переключение включалось
     * бы от собственного текста модели — то есть у модели, которая писать
     * блок обязана. */
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(recording_host());
    prepare(eng);

    reset_host_script();
    std::vector<LlmEvent> events;
    LlmClient::fetch(recording_host(), "sys", {{"user", "x"}}, events,
                     nullptr, 1000);
    ASSERT_TRUE(eng.native_tool_support() == NativeToolSupport::Unknown);

    g_tool_deltas = true;
    reset_host_script();
    g_tool_deltas = true;
    events.clear();
    LlmClient::fetch(recording_host(), "sys", {{"user", "x"}}, events,
                     nullptr, 1000);
    if (eng.native_tool_support() != NativeToolSupport::Confirmed) {
        std::cerr << "  дельта нативного вызова не подтвердила протокол"
                  << std::endl;
    }
    ASSERT_TRUE(eng.native_tool_support() == NativeToolSupport::Confirmed);
}

TEST(подтверждённый_протокол_меняет_промпт_и_убирает_каталог) {
    /* Обе половины переключения проверяются на ЖИВОМ промпте движка:
     * требование писать блок исчезает, и каталог инструментов тоже —
     * он был бы второй копией схемы, за которую платят на каждом ходе. */
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(recording_host());
    prepare(eng);

    const std::string text_prompt = eng.build_system_prompt();
    ASSERT_TRUE(text_prompt.find("wp_action") != std::string::npos);
    ASSERT_TRUE(text_prompt.find("## ИНСТРУМЕНТЫ") != std::string::npos);

    eng.note_native_tool_seen();

    const std::string native_prompt = eng.build_system_prompt();
    if (native_prompt.find("wp_action") != std::string::npos) {
        std::cerr << "  подтверждённый нативный вызов не убрал требование "
                     "писать блок" << std::endl;
    }
    ASSERT_TRUE(native_prompt.find("wp_action") == std::string::npos);
    ASSERT_TRUE(native_prompt.find("## ИНСТРУМЕНТЫ") == std::string::npos);
    /* Хвост промпта на месте — переключение не выбрасывает проект и
     * инструкции. */
    ASSERT_TRUE(native_prompt.find("## РАБОТА С ПУТЯМИ") !=
                std::string::npos);

    /* Возврат по отказу: текстовый протокол и каталог обязаны вернуться. */
    eng.note_tools_rejected(true);
    ASSERT_TRUE(eng.native_tool_support() == NativeToolSupport::Rejected);
    const std::string back = eng.build_system_prompt();
    ASSERT_TRUE(back.find("wp_action") != std::string::npos);
    ASSERT_TRUE(back.find("## ИНСТРУМЕНТЫ") != std::string::npos);
    ASSERT_EQ(back, text_prompt);
}

TEST(список_инструментов_в_запросе_и_в_каталоге_один) {
    /* Список видимых — ОДИН (Engine::visible_tools) на каталог промпта и на
     * нативные спецификации. Собрать его дважды — значит получить два
     * места, где он разъедётся: модель увидит в промпте инструмент, которого
     * не сможет вызвать, или наоборот. */
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(recording_host());
    prepare(eng);
    test_support::approve_all_permissions();

    const std::vector<std::string> visible = eng.visible_tools();
    ASSERT_TRUE(!visible.empty());

    const std::string request_json = fetch_once(eng);
    for (const std::string& name : visible) {
        if (!request_has_tool(request_json, name)) {
            std::cerr << "  видимый инструмент не ушёл в запрос: " << name
                      << std::endl;
        }
        ASSERT_TRUE(request_has_tool(request_json, name));
    }
    /* И наоборот: лишних нет. */
    ASSERT_EQ(tools_in_request(request_json), visible.size());
}

TEST(скрытый_правилами_инструмент_не_уходит_в_нативные_спецификации) {
    /* Отдельная проверка, а не продолжение предыдущей.
     *
     * Пока разрешено всё, «видимый список» и «полный список реестра» —
     * одно и то же, и проверка «в запросе ровно visible.size()» прошла бы
     * даже при коде, который берёт ВСЕ инструменты и фильтр не применяет.
     * Именно на этом пропала бы первая мутация: фильтр молча сняли, а
     * запрещённый инструмент всё равно уходил бы провайдеру, и модель
     * тратила бы шаг на вызов, который отклонят.
     *
     * Закрывается настоящим запретом, а не флагом проверки. */
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(recording_host());
    prepare(eng);
    test_support::approve_all_permissions();

    ASSERT_TRUE(request_has_tool(fetch_once(eng), "bash"));

    eng.permissions().add_rule(
        Rule{"bash", "*", PermissionAction::Deny,
             "тест: bash запрещён, чтобы проверить фильтр видимых"});

    const std::vector<std::string> visible = eng.visible_tools();
    bool bash_still_visible = false;
    for (const std::string& name : visible) {
        if (name == "bash") bash_still_visible = true;
    }
    ASSERT_FALSE(bash_still_visible);

    const std::string request_json = fetch_once(eng);
    if (request_has_tool(request_json, "bash")) {
        std::cerr << "  запрещённый инструмент ушёл в нативные спецификации"
                  << std::endl;
    }
    ASSERT_TRUE(!request_has_tool(request_json, "bash"));
    /* И в промпте его нет — тот же список, значит каталог тоже очищен.
     *
     * Ищется ОПИСАНИЕ ИНСТРУМЕНТА, а не «- bash »: такая строка есть и в
     * базовом протоколе («- bash — КРАЙНЯЯ мера»), и проверка на неё
     * прошла бы при каталоге, полном запрещённых инструментов. */
    const std::string prompt = eng.build_system_prompt();
    if (prompt.find("Запуск shell-команды") != std::string::npos) {
        std::cerr << "  запрещённый инструмент остался в каталоге промпта"
                  << std::endl;
    }
    ASSERT_TRUE(prompt.find("Запуск shell-команды") == std::string::npos);
    /* А строка про bash в протоколе осталась — она к каталогу отношения не
     * имеет, и её отсутствие означало бы, что промпт потерял правило. */
    ASSERT_TRUE(prompt.find("КРАЙНЯЯ мера") != std::string::npos);
}

TEST(переинициализация_сбрасывает_вывод_о_нативном_вызове) {
    /* Вывод относится к ПРОВАЙДЕРУ. Переинициализация движка означает, что
     * хост переставил колбэки, и унаследованный отказ держал бы
     * текстовый протокол у провайдера, который нативный вызов умеет. */
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(recording_host());
    prepare(eng);

    eng.note_tools_rejected(true);
    ASSERT_TRUE(eng.native_tool_support() == NativeToolSupport::Rejected);

    eng.init(recording_host());
    ASSERT_TRUE(eng.native_tool_support() == NativeToolSupport::Unknown);
    ASSERT_TRUE(tools_in_request(fetch_once(eng)) > 0);
}
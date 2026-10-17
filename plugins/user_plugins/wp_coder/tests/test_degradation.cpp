/*
 * test_degradation.cpp — И13.8: хост, у которого нет поля.
 *
 * Три деградации задачи: нет llm_chat_stream, нет llm_ast_symbols, нет
 * llm_chat_messages. Формулировка задачи мягче правды, и разница найдена
 * ДО правки, на живом коде:
 *
 *   1. Слой деградации ЖИВ целиком — и не был проверен ничем. Слоты
 *      HostCallbacks ставились прямо в src/plugin_main.cpp, а src/ в сборку
 *      тестов не входит (правило И0.9), то есть «какой слот у какого
 *      хоста» было знанием, доступным только чтением исходника.
 *   2. Фолбэк без llm_chat_messages был НАПИСАН дважды: копия в
 *      src/plugin_main.cpp — живая, потому что слот llm_chat заполнялся
 *      ВСЕГДА, — и единственная в core/llm_source.cpp, мёртвая, потому что
 *      ядро всегда получало непустой llm_chat. Заголовок llm_source.h
 *      объявлял себя единственным местом этого запасного пути, и объявление
 *      было неправдой. Теперь слот ставится по пробе, а решение принимает
 *      ядро (host_bridge/llm_slots.cpp).
 *   3. Потеря метрик оказалась ЧЕСТНОЙ, и это проверяется, а не
 *      объявляется: пустой Usage доезжает до панели как «нет измерения»,
 *      движок подставляет оценку chars/4 и ставит флаг, панель называет
 *      оценку словом. Последняя проверка файла — именно эта честность;
 *      без неё «фолбэк есть» ничего не говорило бы о том, что человек
 *      увидит.
 *
 * МАТЕРИАЛ — НАСТОЯЩИЙ LlamaHostApi с усечённой структурой: size обрывается
 * на начале отсутствующего поля, ровно как у хоста, собранного против
 * старого заголовка (именно это и было причиной падения на PC=0x1, И0.2).
 * Подставленные вручную HostCallbacks проверяли бы ядро, но не мост, а
 * ломается мост: «слот не заполнился» и «слот заполнился» снаружи
 * неотличимы.
 *
 * Что здесь НЕ проверяется, чтобы не выдавать чужую работу за свою:
 * разбор чанков и время жизни колбэков стриминга (test_llm_stream_abi),
 * раскладка событий блокирующего пути (test_llm_source), решение
 * AST/скан и обзор проекта (test_ast_symbols, И13.5). Здесь — только
 * соответствие слотов хосту и то, что после этого соответствия ход
 * остаётся правильным.
 */

#include "core/ast_symbols.h"
#include "core/engine.h"
#include "core/json.h"
#include "core/llm_client.h"
#include "core/llm_source.h"
#include "core/message.h"
#include "core/token_panel.h"
#include "host_bridge/llm_slots.h"

#include "test_framework.h"

#include "plugins/plugin_api.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace coder;
namespace fs = std::filesystem;

namespace {

/* --- Хост-подделка --- */

/* Что хост отдаёт и что видел. Один объект на прогон: фейковые функции
 * висят в таблице как обычные указатели, и «состояние» у них одно. */
struct FakeHost {
    std::string chat_json =
        "{\"ok\":1,\"content\":\"Ответ блокирующего пути.\","
        "\"finish_reason\":\"stop\",\"prompt_tokens\":11,"
        "\"completion_tokens\":6}";
    std::string single_shot_answer = "Ответ одногилого пути.";
    std::string ast_json =
        "{\"ok\":1,\"ast\":true,\"language\":\"cpp\",\"symbols\":["
        "{\"kind\":\"class\",\"name\":\"Хост\",\"parent\":\"\","
        "\"line\":3,\"end_line\":9}]}";

    /* Что хост увидел на входе. */
    std::string seen_sys;
    std::string seen_messages;   /* llm_chat_messages: вся история */
    std::string seen_single;     /* llm_complete_ex: одна реплика */
    std::string seen_ast_path;

    /* Ответы по попыткам: пусто — хост отдаёт один и тот же chat_json.
     * Нужен для проверки повтора, где первая попытка обязана ПРОВАЛИТЬСЯ,
     * а вторая — пройти; один и тот же ответ проверил бы только успех. */
    std::vector<std::string> chat_json_sequence;
    int chat_seq_next = 0;

    int chat_calls = 0;
    int single_calls = 0;
    int ast_calls = 0;
    int stream_calls = 0;
    int cancel_calls = 0;
};

FakeHost* g_fake = nullptr;

/* Строка хоста выделяется malloc и освобождается его же free_string —
 * ровно тот контракт, на котором держатся и мост, и llm_blocking. */
char* host_string(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

int fake_llm_is_connected(LlamaPluginHost*) { return 1; }

void fake_free_string(LlamaPluginHost*, char* s) { std::free(s); }

int fake_llm_complete_ex(LlamaPluginHost*, const char* sys, const char* user,
                         char** out) {
    if (!g_fake || !out) return 0;
    ++g_fake->single_calls;
    g_fake->seen_sys = sys ? sys : "";
    g_fake->seen_single = user ? user : "";
    *out = host_string(g_fake->single_shot_answer);
    return 1;
}

char* fake_llm_chat_messages(LlamaPluginHost*, const char* sys,
                             const char* messages_json) {
    if (!g_fake) return nullptr;
    ++g_fake->chat_calls;
    g_fake->seen_sys = sys ? sys : "";
    g_fake->seen_messages = messages_json ? messages_json : "";
    if (!g_fake->chat_json_sequence.empty()) {
        const size_t i = static_cast<size_t>(g_fake->chat_seq_next++);
        const std::string& body = g_fake->chat_json_sequence[
            i < g_fake->chat_json_sequence.size() ? i
                                                  : g_fake->chat_json_sequence.size() - 1];
        return host_string(body);
    }
    return host_string(g_fake->chat_json);
}

char* fake_llm_ast_symbols(LlamaPluginHost*, const char* path, const char*) {
    if (!g_fake) return nullptr;
    ++g_fake->ast_calls;
    g_fake->seen_ast_path = path ? path : "";
    return host_string(g_fake->ast_json);
}

/* Поток отдаётся дельтами и закрывается тем же JSON, что и блокирующий
 * путь: сравнение ходов современного и старого хоста имеет смысл только
 * если «модель» отвечает одно и то же. */
int fake_llm_chat_stream(LlamaPluginHost*, const char* sys_prompt,
                         const char* messages_json, const char*,
                         void* user_data,
                         void (*on_delta)(void*, const char*, int),
                         void (*)(void*, const char*, const char*, const char*),
                         void (*on_done)(void*, const char*)) {
    if (!g_fake) return 0;
    ++g_fake->stream_calls;
    g_fake->seen_sys = sys_prompt ? sys_prompt : "";
    g_fake->seen_messages = messages_json ? messages_json : "";
    on_delta(user_data, "Ответ блокирующего пути.", 0);
    on_done(user_data, g_fake->chat_json.c_str());
    return 1;
}

void fake_llm_chat_cancel(LlamaPluginHost*, void*) {
    if (g_fake) ++g_fake->cancel_calls;
}

/* Хост сегодняшний: структура заполнена целиком, все поля на месте. */
LlamaHostApi modern_host() {
    LlamaHostApi api{};
    api.size = sizeof(LlamaHostApi);
    api.llm_is_connected = &fake_llm_is_connected;
    api.free_string = &fake_free_string;
    api.llm_complete_ex = &fake_llm_complete_ex;
    api.llm_chat_messages = &fake_llm_chat_messages;
    api.llm_chat_stream = &fake_llm_chat_stream;
    api.llm_chat_cancel = &fake_llm_chat_cancel;
    api.llm_ast_symbols = &fake_llm_ast_symbols;
    return api;
}

/* Непустой host — хоста у тестов нет, а nullptr отвергли бы проверку
 * раньше, чем она дойдёт до сути. */
LlamaPluginHost* fake_host() {
    static int marker = 0;
    return reinterpret_cast<LlamaPluginHost*>(&marker);
}

/* История из трёх реплик: на одногилом пути модель получит только
 * последнюю, и проверка обязана видеть, что именно последнюю. */
std::vector<ModelMessage> three_turns() {
    return {{kRoleUser, "первая реплика"},
            {kRoleAssistant, "ответ ассистента"},
            {kRoleUser, "третья реплика"}};
}

/* Сколько реплик дошло до хоста. Разбором, а не подсчётом подстроки:
 * содержимое реплики может само содержать кавычки и role. */
size_t messages_seen(const std::string& messages_json) {
    json::JsonValue v;
    if (!json::JsonValue::parse(messages_json, v) || !v.is_array()) return 0;
    return v.size();
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

/* Состояние движка протекает между тестами (SESSION_START, правило 5), и
 * блокирующий путь спрашивает ровно этот флаг: слот llm_chat отдаёт хосту
 * is_cancelled, который читает engine().state().abort_requested. Оставленная
 * предыдущей проверкой «стоп» убила бы ход на первой же итерации ожидания,
 * и проверка падала бы с «Прервано пользователем» на коде, который ничего не
 * сломал. Именно так она и падала: диагностика напечатала этот текст.
 *
 * В приложении флаг сбрасывает run_task в начале задачи; у теста своего
 * хода нет, поэтому сброс здесь. */
void forget_previous_abort() {
    engine().state().abort_requested.store(false);
}

fs::path make_tmp_file(const std::string& tag, const std::string& body,
                       const std::string& name) {
    fs::path tmp = fs::temp_directory_path()
        / ("wp_coder_degr_" + tag + "_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    register_tmp_tree(tmp.string());
    const fs::path file = tmp / name;
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f << body;
    return file;
}

} // namespace

/* --- 1. Ответный контроль: у современного хоста заполнено всё --- */

/*
 * Первая проверка файла — отрицательный контроль для остальных. Без неё
 * набор «пустые слоты» прошёл бы и на мосте, который не заполняет НИ
 * ОДИН слот: все проверки деградации стали бы зелёными на коде, в
 * котором слотов нет вообще. То есть у деградации должен быть живой
 * современный хост, иначе «деградация» неотличима от поломки.
 */
TEST(современный_хост_получает_все_слоты_которые_просит) {
    FakeHost fake;
    g_fake = &fake;
    const LlamaHostApi api = modern_host();
    HostCallbacks cb;
    host::install_llm_slots(cb, &api, fake_host());
    g_fake = nullptr;

    ASSERT_TRUE((bool)cb.llm_is_connected);
    ASSERT_TRUE((bool)cb.llm_complete);
    ASSERT_TRUE((bool)cb.llm_chat);
    ASSERT_TRUE((bool)cb.llm_chat_stream);
    ASSERT_TRUE((bool)cb.llm_chat_cancel);
    ASSERT_TRUE((bool)cb.ast_symbols);
}

/* --- 2. Матрица слотов: хост каждого поколения --- */

/*
 * Одна таблица вместо трёх отдельных проверок, и в этом её смысл:
 * «слот заполнен, если у хоста есть поле» — правило, а не три случая.
 * Строки таблицы — РЕАЛЬНЫЕ поколения хоста (структура обрывается на
 * начале отсутствующего поля), а не выдуманные комбинации флагов.
 *
 * Последняя строка — не выдумка, а реальность ABI: поле есть по размеру,
 * а хост его не заполнил. Именно такой хост и читался как мусор в И0.2.
 */
TEST(слоты_следуют_за_размером_структуры_хоста) {
    struct Row {
        const char* name;
        size_t size;
        bool stream;
        bool cancel;
        bool chat;
        bool ast;
    };

    const LlamaHostApi full = modern_host();

    /* size = offsetof(поле) — хост заполнил структуру ДО этого поля. */
    const Row rows[] = {
        {"современный хост", sizeof(LlamaHostApi), true, true, true, true},
        /* Порядок полей в структуре замерено, а не выдумано:
         * complete_ex(264) < chat_messages(296) < chat_stream(304) <
         * ast_symbols(312) < chat_cancel(320), sizeof = 328. Хост без
         * ast_symbols отсекает и отмену — она объявлена после него. */
        {"без ast_symbols", offsetof(LlamaHostApi, llm_ast_symbols),
         true, false, true, false},
        {"без llm_chat_stream", offsetof(LlamaHostApi, llm_chat_stream),
         false, false, true, false},
        {"совсем старый: без llm_chat_messages",
         offsetof(LlamaHostApi, llm_chat_messages),
         false, false, false, false},
    };

    for (const Row& row : rows) {
        FakeHost fake;
        g_fake = &fake;
        LlamaHostApi api = full;
        api.size = row.size;
        HostCallbacks cb;
        host::install_llm_slots(cb, &api, fake_host());
        g_fake = nullptr;

        const std::string seen = row.name;
        /* Одногилый слот и проверка связи заполняются ВСЕГДА: это поля,
         * которые были у хоста с самого начала, и «деградировать» до их
         * отсутствия нельзя — иначе плагин падал бы на ровно том хосте,
         * ради которого всё это затевалось. */
        if (!cb.llm_complete || !cb.llm_is_connected) {
            std::cerr << "  " << seen
                      << ": слот безусловного поля пуст (llm_complete или"
                      << " llm_is_connected)" << std::endl;
            ASSERT_TRUE(false);
        }
        const bool got_stream = (bool)cb.llm_chat_stream;
        const bool got_cancel = (bool)cb.llm_chat_cancel;
        const bool got_chat = (bool)cb.llm_chat;
        const bool got_ast = (bool)cb.ast_symbols;
        if (got_stream != row.stream || got_cancel != row.cancel ||
            got_chat != row.chat || got_ast != row.ast) {
            std::cerr << "  " << seen << ": ждали stream=" << row.stream
                      << " cancel=" << row.cancel << " chat=" << row.chat
                      << " ast=" << row.ast << ", получили stream="
                      << got_stream << " cancel=" << got_cancel << " chat="
                      << got_chat << " ast=" << got_ast << std::endl;
            ASSERT_TRUE(false);
        }
    }

    /* Поле есть по размеру, но хост его не заполнил. Отдельный случай
     * последним: он единственный, где size равен sizeof, а слота нет. */
    {
        FakeHost fake;
        g_fake = &fake;
        LlamaHostApi api = modern_host();
        api.llm_ast_symbols = nullptr;
        HostCallbacks cb;
        host::install_llm_slots(cb, &api, fake_host());
        g_fake = nullptr;
        if (cb.ast_symbols) {
            std::cerr << "  поле ast_symbols есть по размеру, но не заполнено:"
                      << " слот всё равно поставлен — это чтение поля, "
                      << "которого у хоста нет" << std::endl;
            ASSERT_TRUE(false);
        }
    }
}

/* --- 3. Хост без llm_chat_stream: теряется только живая доставка --- */

/*
 * Деградация обязана быть УЗКОЙ: хост без стриминга теряет поток, а не
 * историю и не метрики. Проверяется всё содержимое запроса — счётчиком
 * разобранного JSON, а не подстрокой, — потому что «история ушла целиком»
 * и «ушла последняя реплика» отличаются одним числом, и подстрока этого
 * не различила бы.
 */
TEST(хост_без_стриминга_отдаёт_тот_же_ход_с_той_же_историей_и_метриками) {
    FakeHost fake;
    g_fake = &fake;

    LlamaHostApi api = modern_host();
    api.size = offsetof(LlamaHostApi, llm_chat_stream);
    HostCallbacks cb;
    host::install_llm_slots(cb, &api, fake_host());

    /* Пустой слот стриминга — предпосылка проверки, а не её вывод: если
     * он заполнен, ход пошёл бы другим путём и всё остальное ниже
     * означало бы не то, что написано. */
    ASSERT_FALSE((bool)cb.llm_chat_stream);

    forget_previous_abort();
    std::vector<LlmEvent> events;
    const bool ok = LlmClient::fetch(cb, "системный промпт", three_turns(),
                                     events, nullptr, 5000);
    g_fake = nullptr;

    if (!ok) {
        std::cerr << "  ход не состоялся: "
                  << llm_source::fold(events).error() << std::endl;
    }
    ASSERT_TRUE(ok);
    const LlmResponse r = llm_source::fold(events);
    ASSERT_EQ(r.text(), std::string("Ответ блокирующего пути."));
    /* Метрики хоста ДОЛЖНЫ дойти: потерять их — значит объявить задачу
     * бесплатной, а это правда о хосте, которой никто не проверял. */
    ASSERT_EQ(r.usage().input, (long long)11);
    ASSERT_EQ(r.usage().output, (long long)6);
    ASSERT_EQ(fake.chat_calls, 1);
    ASSERT_EQ(fake.single_calls, 0);
    ASSERT_EQ(messages_seen(fake.seen_messages), (size_t)3);
}

/* --- 4. Хост без llm_chat_messages: история и метрики теряются честно --- */

/*
 * Главная деградация задачи. Проверяются все четыре утверждения сразу:
 *
 *   - многоходовой слот ПУСТ (иначе ядро не дошло бы до одногилого пути,
 *     и проверка молча проверяла бы путь, который в приложении не
 *     встречается — именно он и был мёртвым);
 *   - хост получает ПОСЛЕДНЮЮ реплику, а не всю историю: подмена истории
 *     на пустую дала бы ответ на «ни о чём», а это тихая порча работы
 *     агента, поэтому указываем точное содержимое;
 *   - метрики НЕ выдуманы: Usage пуст, а не нулевой счётчик;
 *   - ответ доезжает и задача закрывается: деградация не имеет права
 *     превращаться в отказ.
 */
TEST(хост_без_многоходового_вызова_теряет_историю_и_метрики_честно) {
    FakeHost fake;
    g_fake = &fake;

    LlamaHostApi api = modern_host();
    api.size = offsetof(LlamaHostApi, llm_chat_messages);
    HostCallbacks cb;
    host::install_llm_slots(cb, &api, fake_host());

    ASSERT_FALSE((bool)cb.llm_chat);
    ASSERT_TRUE((bool)cb.llm_complete);

    std::vector<LlmEvent> events;
    const bool ok = LlmClient::fetch(cb, "системный промпт", three_turns(),
                                     events, nullptr, 5000);
    g_fake = nullptr;

    ASSERT_TRUE(ok);
    ASSERT_EQ(fake.single_calls, 1);
    ASSERT_EQ(fake.chat_calls, 0);
    ASSERT_EQ(fake.seen_sys, std::string("системный промпт"));
    ASSERT_EQ(fake.seen_single, std::string("третья реплика"));
    /* Ранние реплики в запрос не попали: ровно одна строка ушла хосту. */
    ASSERT_FALSE(has(fake.seen_single, "первая реплика"));
    ASSERT_FALSE(has(fake.seen_single, "ответ ассистента"));

    const LlmResponse r = llm_source::fold(events);
    ASSERT_EQ(r.text(), std::string("Ответ одногилого пути."));
    /* Метрики неизвестны, и это обязан быть ПУСТОЙ Usage, а не нули с
     * флагом: ноль — это измерение, и панель напечатала бы «вход: 0». */
    ASSERT_TRUE(r.usage().empty());
    ASSERT_EQ(r.usage().total, (long long)0);
}

/*
 * Половина честности, которая видна человеку. Пустой Usage — это ещё не
 * половина: движок на нём подставляет оценку chars/4 и ставит флаг, и
 * панель обязана называть это словом. Без этой связи «метрики честно
 * потеряны» означало бы только «внутри структуры ноль», а человек увидел
 * бы оценку и не отличил её от измерения.
 */
TEST(оценка_вместо_потерянных_метрик_помечает_себя_словом) {
    EngineState st;
    /* Ответ без usage: ровно то, что приходит от одногилого хоста. */
    apply_task_speed(st, "Ответ одногилого пути.", 2.0);

    ASSERT_TRUE(st.tokens_estimated);

    token_panel::Snapshot snap;
    snap.tokens_per_second = st.last_tokens_per_second;
    snap.tokens_estimated = st.tokens_estimated;
    snap.llm_seconds = 2.0;
    snap.steps = 1;
    const std::string line = token_panel::speed_line(snap);
    ASSERT_TRUE(has(line, "оценка"));
    ASSERT_TRUE(has(line, "провайдер не прислал usage"));
}

/* --- 5. Хост без llm_ast_symbols: обзор уходит на скан и говорит об этом --- */

/*
 * Деградация обзора молчалива ровно настолько, что неотличима от
 * «файла без определений»: пустой список символов на файле с двадцатью
 * функциями читается как файл без функций. Поэтому проверяется не список,
 * а ПРИЧИНА в обзоре, и сверху — что хост с отсутствующим полем вообще не
 * спрашивался (обращение к нему и есть тот segfault на PC=0x1).
 */
TEST(хост_без_ast_оставляет_строковый_скан_и_называет_причину) {
    FakeHost fake;
    g_fake = &fake;

    LlamaHostApi api = modern_host();
    api.size = offsetof(LlamaHostApi, llm_ast_symbols);
    HostCallbacks cb;
    host::install_llm_slots(cb, &api, fake_host());

    ASSERT_FALSE((bool)cb.ast_symbols);
    /* Слоты, которых деградация не касается, на этом хосте живы. */
    ASSERT_TRUE((bool)cb.llm_chat);
    ASSERT_TRUE((bool)cb.llm_chat_stream);

    const fs::path file = make_tmp_file(
        "ast", "<?php\nfunction hello_world() { return 1; }\n", "f.php");
    const ast_symbols::FileSymbols got = ast_symbols::file_symbols(cb, file.string());
    g_fake = nullptr;

    ASSERT_EQ(fake.ast_calls, 0);
    ASSERT_FALSE(got.from_ast);
    ASSERT_EQ(got.degrade, std::string("хоста без AST-разбора"));
    /* Скан нашёл то, что нашёл бы всегда: деградация меняет источник, а
     * не состав обзора. */
    ASSERT_EQ(got.symbols.size(), (size_t)1);
    ASSERT_EQ(ast_symbols::symbol_text(got.symbols[0]),
              std::string("f:hello_world"));
}

/*
 * Обратная сторона той же проверки: у хоста С ПОЛЕМ символы берутся у
 * хоста, а не у скана. Без неё проверка выше была бы зелёной и на коде,
 * который вообще не спрашивает хост об AST.
 */
TEST(хост_с_ast_отдаёт_символы_хоста_а_не_скана) {
    FakeHost fake;
    g_fake = &fake;

    const LlamaHostApi api = modern_host();
    HostCallbacks cb;
    host::install_llm_slots(cb, &api, fake_host());

    const fs::path file = make_tmp_file(
        "ast2", "<?php\nfunction hello_world() { return 1; }\n", "f.php");
    const ast_symbols::FileSymbols got = ast_symbols::file_symbols(cb, file.string());
    g_fake = nullptr;

    ASSERT_EQ(fake.ast_calls, 1);
    ASSERT_TRUE(got.from_ast);
    ASSERT_TRUE(got.degrade.empty());
    ASSERT_EQ(got.symbols.size(), (size_t)1);
    /* Имя из ответа хоста, а не из файла: они разные специально. */
    ASSERT_EQ(ast_symbols::symbol_text(got.symbols[0]),
              std::string("c:Хост"));
}

/* --- 6. Деградация не меняет ответ --- */

/*
 * Последнее и самое дорогое утверждение: на трёх поколениях хоста агент
 * получает ОДИН И ТОТ ЖЕ ход. Проверка по слотам говорила бы «слоты
 * расставлены верно», а эта — «человек увидит то же самое». Совпадение
 * текста, вида завершения и метрик сверяется целиком: расхождение в любом
 * из них означало бы, что на старом хосте агент закрывает задачу иначе.
 */
TEST(деградация_хоста_не_меняет_ответ_агента) {
    const std::vector<ModelMessage> history = three_turns();

    auto run = [&history](size_t size, std::string* text, Usage* usage) {
        FakeHost fake;
        g_fake = &fake;
        LlamaHostApi api = modern_host();
        api.size = size;
        HostCallbacks cb;
        host::install_llm_slots(cb, &api, fake_host());
        forget_previous_abort();
        std::vector<LlmEvent> events;
        const bool ok = LlmClient::fetch(cb, "системный промпт", history,
                                         events, nullptr, 5000);
        g_fake = nullptr;
        if (!ok) {
            *text = "<ход не состоялся>";
            return;
        }
        const LlmResponse r = llm_source::fold(events);
        *text = r.text();
        *usage = r.usage();
    };

    std::string modern_text, no_stream_text;
    Usage modern_usage, no_stream_usage;
    run(sizeof(LlamaHostApi), &modern_text, &modern_usage);
    run(offsetof(LlamaHostApi, llm_chat_stream), &no_stream_text,
        &no_stream_usage);

    ASSERT_EQ(modern_text, std::string("Ответ блокирующего пути."));
    ASSERT_EQ(no_stream_text, modern_text);
    ASSERT_TRUE(modern_usage.invariants_hold());
    ASSERT_EQ(no_stream_usage.input, modern_usage.input);
    ASSERT_EQ(no_stream_usage.output, modern_usage.output);
    ASSERT_EQ(no_stream_usage.total, modern_usage.total);
}
/* ======================================================================
 * И13.9: пауза перед повтором считается на НАСТОЯЩЕМ мосте
 * ====================================================================== */

/*
 * Задержка повтора (`retry_delay_ms_random`) живёт в host_bridge/llm_slots.cpp
 * и была написана в И12.6 вместе с циклом повторов — но до сих пор ни разу не
 * исполнилась: до неё доходит только ветка «провайдер ответил ok=0», а все
 * прежние проверки этого файла отдавали успех с первой попытки, а проверки
 * повторов в test_retry.cpp зовут саму функцию напрямую.
 *
 * Проверяется ПУТЬ целиком: ошибка хоста → классификация → пауза → вторая
 * попытка → ответ. Проверка только «ответ получен» прошла бы и на коде без
 * повторов вообще (первая попытка дала бы успех), поэтому отдельно
 * утверждается, что попыток было ДВЕ, и отдельно — что человек увидел паузу.
 *
 * Про паузу: она реальная (2 с для первой попытки), и проверка не может
 * ждать её «настоящую» — иначе она проверяла бы часы. Утверждается
 * СОДЕРЖАНИЕ объявления: вид сбоя, номер попытки и названное время. Именно
 * это читает человек, у которого ход висит, и именно это отличает повтор от
 * молчаливого зависания (И12.6).
 */
TEST(повтор_после_ошибки_провайдера_объявляет_паузу_и_доводит_ход_до_ответа) {
    FakeHost fake;
    fake.chat_json_sequence = {
        /* 503 — транзиентная ошибка: повтор имеет смысл (И12.5). */
        "{\"ok\":0,\"error\":\"503 Service Unavailable\"}",
        "{\"ok\":1,\"content\":\"Ответ после повтора.\","
        "\"finish_reason\":\"stop\",\"prompt_tokens\":11,"
        "\"completion_tokens\":6}"};
    /* Указатель на подделку живёт до конца ЗОВА: мост зовёт функции хоста
     * по указателям из структуры, а состояние у них одно — глобальное. Снять
     * его до вызова значило бы «хоста нет», и проверка измеряла бы не
     * повтор, а отказ по отсутствию хоста. */
    g_fake = &fake;
    const LlamaHostApi api = modern_host();
    HostCallbacks cb;
    host::install_llm_slots(cb, &api, fake_host());

    forget_previous_abort();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().events.clear();
    }

    LlmReply out;
    const bool ok = cb.llm_chat("системный промпт", three_turns(), out);
    g_fake = nullptr;

    if (!ok) std::cerr << "  ход не ответил: " << out.error << std::endl;
    ASSERT_TRUE(ok);
    ASSERT_EQ(out.content, std::string("Ответ после повтора."));
    /* Две попытки, а не одна: иначе «ответ получен» ничего не говорит о
     * повторе — тот же ответ дала бы и машина без цикла повторов. */
    ASSERT_EQ(fake.chat_calls, 2);
    /* Метрики ответа пришли из УСПЕШНОЙ попытки, а не из первой. */
    ASSERT_EQ(out.completion_tokens, 6);

    /* Человек увидел объявление паузы: вид сбоя, «попытка 2/5» и время. */
    std::string notice;
    for (const AgentEvent& e : engine().events()) {
        if (e.kind == AgentEvent::Retry) notice = e.text;
    }
    if (notice.empty()) std::cerr << "  объявления повтора не было" << std::endl;
    ASSERT_TRUE(!notice.empty());
    ASSERT_TRUE(has(notice, "503"));
    ASSERT_TRUE(has(notice, "2/5"));
    ASSERT_TRUE(has(notice, "через "));
}

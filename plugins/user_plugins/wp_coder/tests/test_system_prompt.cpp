/*
 * test_system_prompt.cpp — И12.1: базовый промпт по семейству модели
 * (SystemPrompt::provider, порт session/system.ts:28).
 *
 * Здесь две группы проверок, и разница между ними существенна.
 *
 * ПЕРВАЯ — чистая функция: какое имя модели какому семейству отвечает и
 * что за текст у этого семейства. Проверяется не «функция вернула
 * непустую строку», а поимённо: family_of() назван словом, потому что
 * проверка, угадывающая семейство по тексту промпта, была бы зелёной
 * на любом тексте (правило 13, грабли 6).
 *
 * ВТОРАЯ — привязка в движке. Без неё первая группа проверяла бы
 * функцию, которой никто не пользуется: ровно этим был мёртвый
 * executeShortcut, и такой же была бы SystemPrompt::provider, если бы
 * build_system_prompt продолжал брать kBaseSystemPrompt напрямую.
 *
 * Отдельно проверяется ТОТАЛЬНОСТЬ: каждый вариант обязан содержать все
 * строки формата, которые разбирает Engine::parse_action. Акцент
 * дописывается к протоколу, а не заменяет его — и единственная защита от
 * тихой подмены здесь в том, что протокол назван в одном месте.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/agent_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/prompts.h"
#include "../core/tools_registry.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace coder;

namespace {

/* Строки формата, без которых модель не сможет ни вызвать инструмент,
 * ни прочитать ответ приложения. Список назван здесь, а не собран из
 * исходников, — это проверка ИМЕННО ТОГО, что акцент дописан, а база
 * на месте; собирать его автоматически означало бы проверять, что
 * промпт совпадает с собой. */
const char* const kSharedStrings[] = {
    "[Old tool result content cleared]",
    "<task id=",
    "<task_result>",
    "task_id",
    "<skill_content name=",
    "## Instructions from: <",
    "## РАБОТА С ПУТЯМИ",
    "## ИТОГОВЫЙ ОТВЕТ",
};

/* Строго ТЕКСТОВОГО протокола: формат вызова, который пишет модель. */
const char* const kTextOnlyStrings[] = {
    "wp_action",
    "```json",
    "РОВНО ОДИН wp_action блок за сообщение",
};

/* Строго НАТИВНОГО: при нативном вызове модель не пишет вызов сама, и
 * требование писать его было бы указанием сделать то, чего она не делает.
 * Проверка отрицательная намеренно: именно она ловит ситуацию, когда
 * нативный промпт уехал вместе с текстовой половиной. */
const char* const kForbiddenInNative[] = {
    "wp_action",
    "```json",
};

bool contains_all(const std::string& prompt, const char* const* items,
                  size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (prompt.find(items[i]) == std::string::npos) return false;
    }
    return true;
}

bool contains_all_protocol_strings(const std::string& prompt) {
    return contains_all(prompt, kSharedStrings,
                        sizeof(kSharedStrings) / sizeof(kSharedStrings[0]));
}

/* Правила сессии для from_def: агенту нужно на чём-то собираться, и
 * сами правила тут ни при чём — проверяется только промпт. */
Ruleset session_rules() {
    Ruleset base;
    base.add("read", "*", PermissionAction::Allow);
    return base;
}

std::shared_ptr<agent::Info> agent_with_model(const std::string& model) {
    AgentDef def;
    def.name = "wp_test";
    def.description = "агент проверки";
    def.prompt = "Промпт агента.";
    def.model = model;
    return agent::Info::from_def(def, session_rules());
}

/* Хост-имитация. Таблица настроек живёт дольше колбэков, которые её
 * читают, и об этом сказано в комментарии к возврату состояния. */
std::map<std::string, std::string> g_settings;

HostCallbacks fake_callbacks() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&,
                         std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.path_data_dir = []() { return std::string(); };
    cb.path_config_dir = []() { return std::string(); };
    cb.chat_event = [](const std::string&) {};
    cb.settings_get = [](const std::string& key,
                         const std::string& def) -> std::string {
        auto it = g_settings.find(key);
        return it != g_settings.end() ? it->second : def;
    };
    cb.settings_set = [](const std::string& key, const std::string& value) {
        g_settings[key] = value;
    };
    return cb;
}

/* Engine — синглтон (правило 5), поэтому всё, что проверка в ней
 * меняет, возвращается: иначе агент с объявленной моделью остался бы в
 * области выполнения и следующая проверка собрала бы промпт по нему. */
struct EngineStateGuard {
    Engine& e;
    RunScope scope;
    std::string project_dir;
    std::string agent_system_prompt;
    std::string cached;
    bool dirty = true;

    explicit EngineStateGuard(Engine& engine) : e(engine) {
        std::lock_guard<std::mutex> lk(e.state().mtx);
        scope = e.state().scope;
        project_dir = e.state().project_dir;
        agent_system_prompt = e.state().agent_system_prompt;
        cached = e.state().cached_system_prompt;
        dirty = e.state().prompt_dirty;
    }
    ~EngineStateGuard() {
        {
            std::lock_guard<std::mutex> lk(e.state().mtx);
            e.state().scope = scope;
            e.state().project_dir = project_dir;
            e.state().agent_system_prompt = agent_system_prompt;
            e.state().cached_system_prompt = cached;
            e.state().prompt_dirty = dirty;
        }
        /* Колбэк настроек в синглтоне после init указывает на таблицу
         * проверки, а таблица умирает вместе с ней: оставляем безобидный,
         * чтобы следующая проверка не Segmentation fault'нулась на мусоре
         * (то же, что в EngineStateGuard test_instruction.cpp). */
        e.callbacks().settings_get =
            [](const std::string&, const std::string& d) { return d; };
    }
};

} // namespace

TEST(семейство_определяется_подстрокой_имени_модели) {
    struct Case {
        const char* model_id;
        const char* family;
    };
    /* Имена настоящие: те, что плагин видит в полях `model:` агента и в
     * настройках облачного провайдера. Одной подстроки на семейство
     * мало, поэтому у локального три разных имени — иначе проверка
     * прошла бы на одном совпадении и пропустила остальные два. */
    const Case cases[] = {
        {"qwen3-coder-30b-a3b-instruct-q4_k_m", "local"},
        {"Qwen2.5-Coder-7B", "local"},
        {"Llama-3.2-3B-Instruct-Q4_K_M.gguf", "local"},
        {"gemma-3-1b-it-UD-Q8_K_XL", "local"},
        {"gpt-4o-mini", "gpt"},
        {"GPT-5", "gpt"},
        {"gpt-oss-120b", "gpt"},
        {"deepseek-v4.1-flash", "deepseek"},
        {"DeepSeek-R1-Distill-Qwen-32B", "deepseek"},
        /* Не узнано: имена, под которые подстрока не подставлялась.
         * `DeepSeek-R1-Distill-Qwen` — не «неизвестное» имя, а проверка
         * ПЕРВОГО совпавшего: deepseek стоит в списке раньше qwen, и
         * порядок списка — это правило, а не случайность. */
        {"mistral-7b-instruct", "default"},
        {"", "default"},
        {"phi-4", "default"},
    };
    for (const Case& c : cases) {
        const std::string got =
            SystemPrompt::family_name(SystemPrompt::family_of(c.model_id));
        ASSERT_EQ(got, std::string(c.family));
    }
}

TEST(неизвестная_модель_получает_текстовый_промпт_без_акцента) {
    /* Побайтно, а не «содержит базу»: пустой model_id у хода сессии без
     * агента, и добавка в этом случае означала бы, что промпт стал
     * длиннее у всех, у кого модель не объявлена.
     *
     * И12.2: к базе добавлен заголовок каталога инструментов — он уехал из
     * хвоста в провайдер, потому что при нативном вызове списка нет, а
     * заголовок со словами «список ниже» остался бы враньём. Заголовок
     * печатает движок вместе со списком, а заголовок при этом остаётся
     * ПЕРЕД ним — то есть ровно там, где был. */
    const std::string expected =
        std::string(kBaseSystemPrompt) + kToolCatalogueSection;
    ASSERT_EQ(SystemPrompt::provider(""), expected);
    ASSERT_EQ(SystemPrompt::provider("mistral-7b-instruct"), expected);
}

TEST(у_каждого_семейства_свой_текст_и_каждый_длиннее_базы) {
    const char* ids[] = {"qwen3-coder", "gpt-4o", "deepseek-chat"};
    std::vector<std::string> texts;
    for (const char* id : ids) {
        const std::string p = SystemPrompt::provider(id);
        /* Акцент ДОПИСЫВАЕТСЯ к протоколу, а не заменяет его: иначе
         * модель получила бы «ровно один блок за сообщение» без
         * описания самого блока. */
        ASSERT_TRUE(p.size() > std::string(kBaseSystemPrompt).size());
        ASSERT_EQ(p.compare(0, std::string(kBaseSystemPrompt).size(),
                            kBaseSystemPrompt),
                  0);
        texts.push_back(p);
    }
    /* Все три текста РАЗНЫЕ: общий акцент на всех семействах — это
     * ровно то состояние, которое задача 12.1 и отменяет. */
    ASSERT_TRUE(texts[0] != texts[1]);
    ASSERT_TRUE(texts[0] != texts[2]);
    ASSERT_TRUE(texts[1] != texts[2]);
}

TEST(ни_один_вариант_не_теряет_строки_формата_протокола) {
    /* Тотальность, а не выборочно: проверка на две-три строки прошла бы
     * и при потере четвёртой. */
    for (const char* id : {"", "qwen3-coder", "gpt-4o", "deepseek-chat",
                           "mistral-7b"}) {
        const std::string p = SystemPrompt::provider(id);
        ASSERT_TRUE(contains_all_protocol_strings(p));
        /* Формат вызова проверяется только для текстового протокола: при
         * нативном его нет by design, и требование было бы ложью. */
        ASSERT_TRUE(contains_all(p, kTextOnlyStrings,
                                 sizeof(kTextOnlyStrings) /
                                     sizeof(kTextOnlyStrings[0])));
    }
}

TEST(нативный_промпт_не_требует_писать_вызов_текстом) {
    /* И12.2: главная проверка переключения. Модель с нативным вызовом
     * отдаёт вызов полем, и текстовый блок в её промпте — указание сделать
     * то, чего она не делает. Обратная ошибка не менее дорога: без блока
     * модель БЕЗ нативного вызова не сможет позвать инструмент вовсе. */
    const std::string native_prompt = SystemPrompt::provider(
        "qwen3-coder", SystemPrompt::Protocol::Native);

    for (const char* forbidden : kForbiddenInNative) {
        if (native_prompt.find(forbidden) != std::string::npos) {
            std::cerr << "  в нативном промпте осталось: " << forbidden
                      << std::endl;
        }
        ASSERT_TRUE(native_prompt.find(forbidden) == std::string::npos);
    }
    /* Общая часть на месте: половина промпта не потерялась вместе с
     * текстовым протоколом. */
    ASSERT_TRUE(contains_all_protocol_strings(native_prompt));
    /* И нативный блок на месте — иначе «нет wp_action» означало бы, что
     * промпт просто не про вызовы. */
    ASSERT_TRUE(native_prompt.find("вызови его по имени") !=
                std::string::npos);
    /* Семейственный акцент тоже переключился: совет про JSON у локальной
     * модели при нативном вызове был бы враньём. */
    ASSERT_TRUE(native_prompt.find("ТОЛЬКО JSON-объект") ==
                std::string::npos);
    ASSERT_TRUE(native_prompt.find("Не пиши JSON") != std::string::npos);
}

TEST(два_протокола_дают_разные_тексты_у_каждого_семейства) {
    /* Попарно, а не «хотя бы один»: иначе переключатель, который забыл
     * про одно из трёх семейств, прошёл бы. */
    const char* ids[] = {"qwen3-coder", "gpt-4o", "deepseek-chat", ""};
    for (const char* id : ids) {
        const std::string text = SystemPrompt::provider(id);
        const std::string native = SystemPrompt::provider(
            id, SystemPrompt::Protocol::Native);
        if (text == native) {
            std::cerr << "  промпт не переключился для модели " << id
                      << std::endl;
        }
        ASSERT_TRUE(text != native);
    }
}

TEST(локальным_моделям_напоминают_один_блок_и_json_only) {
    /* То, что названо в плане для 12.1 дословно. */
    const std::string p = SystemPrompt::provider("qwen3-coder-30b");
    ASSERT_TRUE(p.find("РОВНО ОДИН вызов инструмента на сообщение") !=
                std::string::npos);
    ASSERT_TRUE(p.find("ТОЛЬКО JSON-объект") != std::string::npos);
    /* wp_action остаётся запасным путём, а не запрещённым: обрывок
     * парсер берёт и из него (parse_action), и обещать модели, что
     * формат не работает, было бы враньём. */
    ASSERT_TRUE(p.find("wp_action — запасная") != std::string::npos);
}

TEST(агент_с_объявленной_моделью_получает_промпт_своего_семейства) {
    g_settings.clear();
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(fake_callbacks());
    eng.load_settings();
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().scope = RunScope();
        eng.state().agent_system_prompt.clear();
    }
    eng.invalidate_prompt_cache();

    /* Без агента — база: нечего выбирать. */
    const std::string session_prompt = eng.build_system_prompt();
    if (session_prompt.find("ЛОКАЛЬНОЙ МОДЕЛИ") != std::string::npos) {
        std::cerr << "  ход без агента получил акцент семейства"
                  << std::endl;
    }
    ASSERT_TRUE(session_prompt.find("ЛОКАЛЬНОЙ МОДЕЛИ") ==
                std::string::npos);

    /* Агент объявил qwen — у него локальный акцент. */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().scope.agent = "wp_test";
        eng.state().scope.info = agent_with_model("qwen3-coder-30b");
    }
    eng.invalidate_prompt_cache();
    const std::string agent_prompt = eng.build_system_prompt();
    if (agent_prompt.find("ЛОКАЛЬНОЙ МОДЕЛИ") == std::string::npos) {
        std::cerr << "  агент с моделью qwen не получил локальный промпт"
                  << std::endl;
    }
    ASSERT_TRUE(agent_prompt.find("ЛОКАЛЬНОЙ МОДЕЛИ") != std::string::npos);
    /* Протокол на месте — выбор промпта не выбрасывает базу. */
    ASSERT_TRUE(contains_all_protocol_strings(agent_prompt));

    /* Агент БЕЗ модели — снова база: объявленного имени нет, и
     * выбирать по пустой строке нечего. */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().scope.info = agent_with_model("");
    }
    eng.invalidate_prompt_cache();
    const std::string silent_prompt = eng.build_system_prompt();
    if (silent_prompt.find("ЛОКАЛЬНОЙ МОДЕЛИ") != std::string::npos) {
        std::cerr << "  агент без модели получил акцент семейства"
                  << std::endl;
    }
    ASSERT_TRUE(silent_prompt.find("ЛОКАЛЬНОЙ МОДЕЛИ") == std::string::npos);
}

TEST(промпт_субагента_не_остаётся_в_родителе) {
    /* ScopedAgentScope меняет область и сбрасывает кэш (engine.cpp:402).
     * Без этого субагент с qwen оставил бы родителю свой акцент на
     * остаток сессии — и следующий ход родителя ушёл бы с чужим
     * промптом. */
    g_settings.clear();
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(fake_callbacks());
    eng.load_settings();
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().scope = RunScope();
        eng.state().agent_system_prompt.clear();
    }
    eng.invalidate_prompt_cache();

    const std::string parent_before = eng.build_system_prompt();
    {
        RunScope child;
        child.agent = "wp_test";
        child.info = agent_with_model("gpt-4o-mini");
        ScopedAgentScope scope(eng, child);
        const std::string child_prompt = eng.build_system_prompt();
        if (child_prompt.find("ДЛЯ GPT") == std::string::npos) {
            std::cerr << "  субагент с моделью gpt не получил свой промпт"
                      << std::endl;
        }
        ASSERT_TRUE(child_prompt.find("ДЛЯ GPT") != std::string::npos);
    }
    const std::string parent_after = eng.build_system_prompt();
    ASSERT_TRUE(parent_after.find("ДЛЯ GPT") == std::string::npos);
    ASSERT_EQ(parent_after, parent_before);
}
/* ======================================================================
 * И12.4: edit+write → apply_patch для семейства gpt-*
 * ======================================================================
 *
 * ПОЧЕМУ ТАКАЯ СТРУКТУРА ПРОВЕРОК. Задача состоит из трёх кусков, и
 * проверить любой один из них по отдельности — проверить не задачу:
 *   - решение «у этой модели правка через патч» (чистая функция);
 *   - вычитание имён из списка (чистая функция);
 *   - привязка: решение доезжает до набора инструментов, который видит
 *     модель, а набор — ИМЕННО ТОТ ЖЕ, что уходит полем tools.
 * Третий кусок — единственный, который ловит D2 наоборот: когда модель
 * читает каталог из одних инструментов, а звать может другие. Поэтому
 * привязка проверяется на ОБЕИХ сторонах — каталоге и списке tools, — а не
 * на одной.
 *
 * ПОЧЕМУ ОТДЕЛЬНО ПРОВЕРЯЕТСЯ, ЧТО ПРОМПТ НЕ ВРЁТ. Подмена действует не у
 * всего семейства (gpt-4 и gpt-oss исключены), а акцент семейства у них
 * общий. Совет «правь через apply_patch» внутри общего акцента означал бы,
 * что gpt-4 получит указание пользоваться инструментом, которого у него в
 * наборе нет. Промпт, обещающий то, чего нет, — враньё модели ровно так же,
 * как выдуманный golden враньё проверке; поэтому это утверждение стоит
 * первым по важности, а не «для полноты».
 */

TEST(подмена_правки_включается_только_для_нужных_моделей) {
    struct Case {
        const char* model_id;
        bool expected;
        const char* why;
    };
    /* Список назван целиком, а не «парочка примеров»: исключения читаются
     * подстроками, и именно подстроками ломаются — «gpt-4o» и «gpt-4.1»
     * содержат «gpt-4», и оба обязаны это утверждать (отклонение 204). */
    const Case cases[] = {
        {"gpt-5",            true,  "подмена есть"},
        {"GPT-5",            true,  "регистр не важен"},
        {"gpt-5-mini",       true,  "то же поколение, что у gpt-5"},
        {"gpt-4",            false, "исключённое поколение"},
        {"gpt-4o",           false, "содержит «gpt-4»"},
        {"gpt-4.1",          false, "содержит «gpt-4»"},
        {"gpt-oss-120b",     false, "открытые веса печатают патчи хуже"},
        {"gpt-5-oss",        false, "oss в любом месте выключает"},
        {"qwen3-coder-30b",  false, "не gpt"},
        {"deepseek-chat",    false, "не gpt"},
        {"",                 false, "имени нет — выбирать нечего"},
    };
    for (const Case& c : cases) {
        const bool got = SystemPrompt::prefers_patch_editing(c.model_id);
        if (got != c.expected) {
            std::cerr << "  " << c.model_id << ": ожидалось "
                      << (c.expected ? "подмену" : "без подмены")
                      << " (" << c.why << ")" << std::endl;
            ASSERT_TRUE(false);
        }
    }
}

TEST(подмена_убирает_точечных_редакторов_и_сохраняет_остальное) {
    std::vector<std::string> visible = {"read_file", "write_file",
                                        "search_replace", "edit_file",
                                        "apply_patch", "bash"};
    const std::vector<std::string> out = substitute_patch_editors(visible);
    ASSERT_EQ(out.size(), (size_t)4);
    /* Убраны именно edit и write нашего словаря. */
    for (const std::string& n : out) {
        ASSERT_TRUE(n != "write_file");
        ASSERT_TRUE(n != "search_replace");
    }
    /* apply_patch ОСТАЛСЯ: подмена, убирающая и его, оставила бы модель
     * вообще без способа изменить файл, и проверка «список не пуст» была
     * бы зелёной на полностью бессмысленном наборе. */
    for (const std::string& want : {"read_file", "apply_patch", "bash"}) {
        bool found = false;
        for (const std::string& n : out) found = found || (n == want);
        ASSERT_TRUE(found);
    }
    /* Порядок сохранён: каталог уходит в промпт целиком, и перестановка
     * меняла бы его содержимое без единой смены правил. */
    ASSERT_EQ(out[0], std::string("read_file"));
    ASSERT_EQ(out[1], std::string("edit_file"));
    ASSERT_EQ(out[2], std::string("apply_patch"));
    ASSERT_EQ(out[3], std::string("bash"));
}

TEST(подмена_идемпотентна_и_переживает_пустой_список) {
    /* Повторная подмена обязана быть холостой: иначе функция, вызванная
     * дважды по дороге, тихо убрала бы что-то ещё. */
    const std::vector<std::string> once =
        substitute_patch_editors({"write_file", "apply_patch", "bash"});
    const std::vector<std::string> twice = substitute_patch_editors(once);
    ASSERT_EQ(twice.size(), once.size());
    for (size_t i = 0; i < once.size(); ++i) ASSERT_EQ(twice[i], once[i]);
    ASSERT_EQ(substitute_patch_editors({}).size(), (size_t)0);
}

TEST(промпт_gpt_советует_патч_и_не_советует_его_исключённым) {
    struct Case {
        const char* model_id;
        bool advice;
    };
    const Case cases[] = {
        {"gpt-5",        true},
        {"gpt-4o-mini",  false},
        {"gpt-oss-120b", false},
        {"qwen3-coder",  false},
        {"",             false},
    };
    for (const Case& c : cases) {
        /* ОБА протокола: при нативном вызове каталога в промпте нет вовсе,
         * и совет про apply_patch остаётся единственным местом, где
         * модели сказано, как править файл. */
        for (SystemPrompt::Protocol p :
             {SystemPrompt::Protocol::Text, SystemPrompt::Protocol::Native}) {
            const std::string prompt = SystemPrompt::provider(c.model_id, p);
            const bool has =
                prompt.find(SystemPrompt::kGptPatchEditingPrompt) != std::string::npos;
            if (has != c.advice) {
                std::cerr << "  " << c.model_id << ": совет про apply_patch "
                          << (has ? "есть" : "нет") << ", ожидалось "
                          << (c.advice ? "есть" : "нет") << std::endl;
                ASSERT_TRUE(false);
            }
        }
    }
}

TEST(совет_про_патч_не_появляется_у_не_gpt_семейств) {
    /* Отдельная проверка от таблицы выше: там модель ОТКЛЮЧЕНА исключением
     * (это gpt, но не подменяем), а здесь семейство другое вовсе. Подмена
     * и совет — две разные вещи, и сойтись они могут только если оба
     * написаны по одному условию; если условие расползлось, поймает эта. */
    for (const char* model : {"qwen3-coder-30b", "llama-3", "gemma-3-1b",
                              "deepseek-chat", "claude-3", ""}) {
        const std::string prompt = SystemPrompt::provider(model);
        if (prompt.find(SystemPrompt::kGptPatchEditingPrompt)
                != std::string::npos) {
            std::cerr << "  " << model
                      << " получил совет про apply_patch" << std::endl;
            ASSERT_TRUE(false);
        }
    }
}

TEST(набор_инструментов_агента_gpt_лишён_точечных_редакторов) {
    /* Привязка: решение о подмене обязано ДОЕЗЖАТЬ до набора, который
     * видит модель. Проверка только prefers_patch_editing была бы зелёной
     * при коде, который решает правильно и ничего с решением не делает —
     * ровно класс мёртвого executeShortcut. */
    g_settings.clear();
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(fake_callbacks());
    eng.load_settings();
    register_base_tools();

    struct Case {
        const char* model_id;
        bool substituted;
    };
    const Case cases[] = {
        {"gpt-5",         true},
        {"gpt-5-mini",    true},
        {"gpt-4o-mini",   false},
        {"gpt-oss-120b",  false},
        {"qwen3-coder",   false},
    };
    for (const Case& c : cases) {
        {
            std::lock_guard<std::mutex> lk(eng.state().mtx);
            eng.state().scope = RunScope();
        }
        RunScope child;
        child.agent = "wp_test";
        child.info = agent_with_model(c.model_id);
        ScopedAgentScope scope(eng, child);

        const std::vector<std::string> visible = eng.visible_tools();
        for (const std::string& n : visible) {
            if (!c.substituted) continue;
            if (n == "write_file" || n == "search_replace") {
                std::cerr << "  " << c.model_id
                          << ": в наборе остался " << n << std::endl;
                ASSERT_TRUE(false);
            }
        }
        if (c.substituted) {
            bool has_patch = false;
            for (const std::string& n : visible) has_patch = has_patch ||
                (n == "apply_patch");
            ASSERT_TRUE(has_patch);
        }
        /* БЕЗ подмены набор не тронут вовсе — и это тоже утверждение:
         * проверка «apply_patch на месте» прошла бы и у модели, у которой
         * ничего не убирали, то есть ничего бы не доказывала. */
        if (!c.substituted) {
            bool has_write = false;
            for (const std::string& n : visible) has_write = has_write ||
                (n == "write_file");
            ASSERT_TRUE(has_write);
        }
    }
}

TEST(каталог_и_нативный_список_инструментов_совпадают_для_gpt) {
    /* ГЛАВНАЯ ПРОВЕРКА ЗАДАЧИ: модель не должна увидеть один набор, а
     * звать другой. Каталог промпта и поле tools строятся из ОДНОГО
     * Engine::visible_tools(); проверка, что обе стороны одинаковы, ловит
     * возврат D2 наоборот — и поймала бы его, если бы подмена появилась в
     * одной из двух сборок. */
    g_settings.clear();
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(fake_callbacks());
    eng.load_settings();
    register_base_tools();

    RunScope child;
    child.agent = "wp_test";
    child.info = agent_with_model("gpt-5");
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().scope = RunScope();
    }
    ScopedAgentScope scope(eng, child);

    const std::vector<std::string> visible = eng.visible_tools();
    const std::string cat =
        ToolsRegistry::instance().build_tool_catalogue(visible);
    const std::string specs =
        ToolsRegistry::instance().build_tool_specs_json(visible);

    /* В каталоге их нет — и именем, и по тому, что описания убраны. */
    ASSERT_TRUE(cat.find("write_file") == std::string::npos);
    ASSERT_TRUE(cat.find("search_replace") == std::string::npos);
    ASSERT_TRUE(cat.find("apply_patch") != std::string::npos);
    /* В нативном списке — то же самое, поимённо: обе стороны обязаны быть
     * вырезаны ОДНОЙ функцией, и «\"write_file\"» ищется в JSON, а не в
     * тексте, где это имя могло бы встретиться в описании другого
     * инструмента. */
    ASSERT_TRUE(specs.find("\"write_file\"") == std::string::npos);
    ASSERT_TRUE(specs.find("\"search_replace\"") == std::string::npos);
    ASSERT_TRUE(specs.find("\"apply_patch\"") != std::string::npos);
}

TEST(инструмент_подмены_остаётся_рабочим_в_реестре) {
    /* Подмена убирает инструмент из НАБОРА, а не из реестра: модель может
     * позвать write_file по инерции (или по памяти о прошлом ходе), и она
     * обязана получить результат, а не отказ. Скрытие самим реестром
     * сделало бы инструмент нерабочим, и это была бы дыра, а не правка
     * промпта. */
    g_settings.clear();
    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(fake_callbacks());
    eng.load_settings();
    register_base_tools();
    ASSERT_TRUE(ToolsRegistry::instance().find("write_file") != nullptr);
    ASSERT_TRUE(ToolsRegistry::instance().find("apply_patch") != nullptr);
    /* И вызов проходит: write_file без обязательных аргументов даёт
     * сообщение валидации от реестра, то есть инструмент жив и отвечает. */
    const std::string r = ToolsRegistry::instance().run(
        "write_file", json::JsonValue::object());
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
}

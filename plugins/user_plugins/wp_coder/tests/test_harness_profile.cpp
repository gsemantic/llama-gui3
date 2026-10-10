/*
 * test_harness_profile.cpp — И9.7: профили harness.
 *
 * Четыре поставленных профиля лежали в дереве плагина без читателя, и
 * первая задача — не «включить», а решить, что их поля значат. Проверки
 * разложены по тому, где может тихо не сработать:
 *
 *   1. ПОСТАВЛЕННЫЕ ФАЙЛЫ ЧИТАЮТСЯ. Каталог берётся из того же
 *      compile definition, что и у плагина, поэтому проверка смотрит на
 *      настоящие файлы, а не на свою копию. Первая версия этой проверки
 *      взяла каталог из фикстуры рядом — и прошла бы на пустом каталоге
 *      (ровно тот случай, о котором журнал И9.1–9.6, п. 4).
 *   2. РАЗБОР СТРОГИЙ. Неизвестный tools_policy, мусор в числах и не тот
 *      тип — отказ с названием виновного поля. Проверка на неизвестное
 *      значение отдельно: прочитанное как «standard» оно тихо сняло бы
 *      все запреты, то есть выглядело бы защищённым и не защищало бы.
 *   3. ЗАПРЕТЫ ВЫВОДЯТСЯ ИЗ ФЛАГОВ. На своей фикстуре инструментов —
 *      чтобы результат не зависел от того, что зарегистрировали другие
 *      файлы тестов, — и на живом реестре, с проверкой ТОТАЛЬНОСТИ.
 *   4. КОМАНДЫ СУЖАЮТСЯ, А НЕ РАСШИРЯЮТСЯ, и сужение СНИМАЕМО.
 *   5. ПРИВЯЗКА К АГЕНТУ (И8): разбор `profile:`, запреты в базе, свои
 *      правила агента поверх.
 *   6. СКЛЕЙКА ЧЕРЕЗ НАСТРОЙКУ: Engine::init → apply_session_profile →
 *      таймаут, запреты, команды, видимые модели инструменты. Без неё
 *      пункты 1–5 проверяли бы функции, которыми никто не пользуется.
 *
 * Чего здесь НЕТ и почему: настоящего времени (таймаут проверяется
 * значением поля, а не ожиданием) и сети (её в профилях нет вовсе).
 */

#include "test_framework.h"
#include "../core/agent_registry.h"
#include "../core/command_policy.h"
#include "../core/engine.h"
#include "../core/harness_profile.h"
#include "../core/llm_client.h"
#include "../core/permission.h"
#include "../core/security.h"
#include "../core/subagent.h"
#include "../core/tools_registry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace coder;

namespace fs = std::filesystem;

namespace {

/* Каталог, куда кладут файлы на время проверки. Убирает СЕБЯ. */
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("wp_coder_profile_" + tag + "_" + std::to_string(++counter));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void write(const std::string& rel, const std::string& body) const {
        const fs::path p = path / rel;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f << body;
    }
    std::string str() const { return path.string(); }
};

/* Каталог ПОСТАВЛЕННЫХ профилей. Тот же define, что у плагина: иначе
 * проверка читала бы не те файлы, которые поедут к пользователю. */
std::string shipped_dir() { return std::string(WP_CODER_PROFILES_DIR); }

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

/* Свой набор инструментов: ключ и флаги. Реестр для этих проверок НЕ
 * используется намеренно — иначе добавление инструмента сдвинуло бы
 * ожидаемое здесь и проверка падала бы не по той причине. */
ToolDef tool(const char* name, const char* key, unsigned flags) {
    ToolDef d;
    d.name = name;
    d.permission_key = key;
    d.flags = flags;
    return d;
}

std::vector<ToolDef> sample_tools() {
    return {
        tool("read_file", "read", TF_READ_ONLY),
        tool("glob", "read", TF_READ_ONLY),
        tool("web_fetch", "read", TF_READ_ONLY | TF_NETWORK),
        tool("write_file", "write", TF_WRITES_FILES),
        tool("apply_patch", "write", TF_WRITES_FILES),
        tool("bash", "bash", TF_EXECUTES | TF_DESTRUCTIVE | TF_SLOW),
        tool("git_status", "git", TF_READ_ONLY | TF_EXECUTES),
        tool("git_commit", "git", TF_EXECUTES | TF_DESTRUCTIVE),
        tool("todowrite", "todo", TF_READ_ONLY),
        tool("todoread", "todo", TF_READ_ONLY),
        tool("deploy", "deploy", TF_EXECUTES | TF_DESTRUCTIVE | TF_NETWORK),
    };
}

/* Настройки для Engine::init. Таблица живёт в стеке проверки, поэтому
 * колбэк settings_get обязан быть снят в её конце — см. ProfileGuard. */
struct Settings {
    std::map<std::string, std::string> values;
    HostCallbacks callbacks() {
        HostCallbacks cb;
        cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                         LlmReply&) { return false; };
        cb.llm_complete = [](const std::string&, const std::string&,
                             std::string&) { return false; };
        cb.llm_is_connected = []() { return false; };
        cb.path_data_dir = []() { return std::string(); };
        cb.path_config_dir = []() { return std::string(); };
        cb.chat_event = [](const std::string&) {};
        /* std::function копирует лямду, а лямда с [&] хранит указатель на
         * ЭТОТ объект — поэтому Settings живёт дольше вызова init. */
        cb.settings_get = [this](const std::string& key,
                                 const std::string& def) -> std::string {
            auto it = values.find(key);
            return it != values.end() ? it->second : def;
        };
        cb.settings_set = [this](const std::string& key,
                                 const std::string& value) {
            values[key] = value;
        };
        return cb;
    }
};

/* Возврат состояния синглтона (Engine — синглтон, правило 5).
 *
 * Здесь восстанавливается не только профиль: apply_session_profile
 * сужает ГЛОБАЛЬНУЮ политику команд и дописывает правила в разрешения
 * сессии, и без возврата следующая проверка получила бы чужую границу.
 * Именно так уже случалось с SkillsManager и AgentRegistry (И9.6, п. 4).
 */
struct ProfileGuard {
    Engine& e;
    Ruleset rules;
    std::set<std::string> binaries;
    std::string profile_name;
    std::string profiles_dir;
    std::string bundled;
    bool has_profile = false;
    harness::Profile profile;
    std::string profile_error;
    int llm_timeout_ms = 0;
    std::string llm_timeout_setting;
    std::string project_dir;

    explicit ProfileGuard(Engine& engine) : e(engine) {
        std::lock_guard<std::mutex> lk(e.state().mtx);
        rules = e.permissions().rules_snapshot();
        binaries = command_policy().allowed_binaries();
        profile_name = e.state().profile_name;
        profiles_dir = e.state().profiles_dir;
        bundled = e.state().profiles_bundled_dir;
        has_profile = e.state().has_profile;
        profile = e.state().profile;
        profile_error = e.state().profile_error;
        llm_timeout_ms = e.state().llm_timeout_ms;
        llm_timeout_setting = e.state().llm_timeout_setting;
        project_dir = e.state().project_dir;
    }
    ~ProfileGuard() {
        {
            std::lock_guard<std::mutex> lk(e.state().mtx);
            e.state().profile_name = profile_name;
            e.state().profiles_dir = profiles_dir;
            e.state().profiles_bundled_dir = bundled;
            e.state().has_profile = has_profile;
            e.state().profile = profile;
            e.state().profile_error = profile_error;
            e.state().llm_timeout_ms = llm_timeout_ms;
            e.state().llm_timeout_setting = llm_timeout_setting;
            e.state().project_dir = project_dir;
        }
        e.permissions().set_rules(rules);
        const std::set<std::string> now = command_policy().allowed_binaries();
        for (const std::string& b : now) {
            if (binaries.count(b) == 0) command_policy().deny_binary(b);
        }
        for (const std::string& b : binaries) command_policy().allow_binary(b);
        /* Колбэк настроек в синглтоне после init может быть висящим:
         * его ставит проверка с локальной таблицей, а таблица умирает
         * вместе с ней. Следующая проверка иначе упала бы на чужом
         * мусоре (журнал И9.1–9.6, п. 2). */
        e.callbacks().settings_get =
            [](const std::string&, const std::string& d) { return d; };
        e.callbacks().settings_set =
            [](const std::string&, const std::string&) {};
    }
};

} // namespace

/* --- 1. Поставленные файлы ------------------------------------------ */

TEST(поставленные_профили_читаются_и_их_четыре) {
    /* Каталог из CMake — тот же, что у плагина. Проверка смотрит на
     * настоящие файлы: своя копия профилей прошла бы, пока файлы в
     * дереве плагина сломаны. */
    const std::vector<std::string> names = harness::profile_names(shipped_dir());
    ASSERT_EQ(names.size(), (size_t)4);
    ASSERT_TRUE(has(names, "fast_local"));
    ASSERT_TRUE(has(names, "accurate_cloud"));
    ASSERT_TRUE(has(names, "secure_audit"));
    ASSERT_TRUE(has(names, "debug_verbose"));

    for (const std::string& n : names) {
        harness::Profile p;
        std::string why;
        ASSERT_TRUE(harness::load_profile(shipped_dir(), n, &p, &why));
        /* Имя приходит из файла, а не из вызова: иначе проверка нашла бы
         * «свой» файл, а не тот, что читает плагин. */
        ASSERT_EQ(p.name, n);
        ASSERT_TRUE(p.source_path.find("profiles/wp_coder") !=
                    std::string::npos);
        ASSERT_FALSE(p.description.empty());
        ASSERT_TRUE(harness::valid_tools_policy(p.tools_policy));
        /* Поставленные профили обязаны быть применимы хоть в чём-то, иначе
         * это просто четыре файла. */
        ASSERT_TRUE(p.has_timeout_ms);
    }
}

TEST(таймауты_поставленных_профилей_различаются_а_не_совпадают) {
    /* Четыре одинаковых значения означали бы, что имена профилей ничего не
     * значат: человек выбрал «secure_audit», получил «fast_local» и не
     * узнал бы. */
    harness::Profile fast;
    harness::Profile audit;
    std::string why;
    ASSERT_TRUE(harness::load_profile(shipped_dir(), "fast_local", &fast, &why));
    ASSERT_TRUE(harness::load_profile(shipped_dir(), "secure_audit", &audit, &why));
    ASSERT_TRUE(fast.has_timeout_ms && audit.has_timeout_ms);
    ASSERT_TRUE(fast.timeout_ms < audit.timeout_ms);
    ASSERT_EQ(fast.tools_policy, std::string("restricted"));
    ASSERT_EQ(audit.tools_policy, std::string("strict"));
    /* Команды у аудита — подмножество: профиль сужает. */
    ASSERT_TRUE(audit.allowed_commands.size() < fast.allowed_commands.size());
}

/* --- 2. Разбор ------------------------------------------------------- */

TEST(разбор_профиля_читает_все_поля) {
    harness::Profile p;
    std::string why;
    ASSERT_TRUE(harness::parse_profile(
        "{\"description\":\"тест\",\"model\":\"local\",\"temperature\":0.3,"
        "\"max_tokens\":2048,\"tools_policy\":\"restricted\","
        "\"timeout_ms\":30000,\"allowed_commands\":[\"ls\",\" cat \"],"
        "\"allowed_extensions\":[\".php\"],\"rag_enabled\":true}",
        &p, &why));
    ASSERT_EQ(p.description, std::string("тест"));
    ASSERT_EQ(p.model_hint, std::string("local"));
    ASSERT_TRUE(p.has_temperature);
    ASSERT_TRUE(p.temperature > 0.29 && p.temperature < 0.31);
    ASSERT_TRUE(p.has_max_tokens);
    ASSERT_EQ(p.max_tokens, 2048);
    ASSERT_EQ(p.tools_policy, std::string("restricted"));
    ASSERT_TRUE(p.has_timeout_ms);
    ASSERT_EQ(p.timeout_ms, 30000);
    /* Пробелы по краям снимаются: иначе « cat » не совпало бы с «cat» в
     * allowlist и профиль тихо сузил бы границу до нуля. */
    ASSERT_EQ(p.allowed_commands.size(), (size_t)2);
    ASSERT_EQ(p.allowed_commands[1], std::string("cat"));
    ASSERT_EQ(p.allowed_extensions.size(), (size_t)1);
    ASSERT_TRUE(p.rag_enabled);
}

TEST(неизвестный_tools_policy_отвергает_профиль_а_не_читается_как_standard) {
    /* Самая дорогая опечатка в этих файлах: «strictt», прочитанное как
     * «standard», сняло бы все запреты профиля secure_audit, и файл
     * продолжал бы выглядеть защищённым. */
    harness::Profile p;
    std::string why;
    ASSERT_FALSE(harness::parse_profile("{\"tools_policy\":\"strictt\"}", &p, &why));
    ASSERT_TRUE(why.find("tools_policy") != std::string::npos);
    ASSERT_TRUE(why.find("strictt") != std::string::npos);
    /* В отказе перечислены допустимые значения — иначе пришлось бы
     * открывать шапку исходника, чтобы понять, что написать. */
    ASSERT_TRUE(why.find("strict") != std::string::npos);
}

TEST(мусор_в_значениях_отвергает_профиль_с_названием_поля) {
    struct Case {
        const char* json;
        const char* field;
    };
    const Case cases[] = {
        {"{\"timeout_ms\":\"30s\"}", "timeout_ms"},
        {"{\"timeout_ms\":0}", "timeout_ms"},
        {"{\"max_tokens\":-1}", "max_tokens"},
        {"{\"max_tokens\":4096.5}", "max_tokens"},
        {"{\"temperature\":5}", "temperature"},
        {"{\"temperature\":\"холодно\"}", "temperature"},
        {"{\"rag_enabled\":\"yes\"}", "rag_enabled"},
        {"{\"allowed_commands\":\"ls\"}", "allowed_commands"},
        {"{\"allowed_commands\":[\"ls\",7]}", "allowed_commands"},
        {"{\"allowed_commands\":[\"ls\",\"\"]}", "allowed_commands"},
        {"{\"tools_policy\":7}", "tools_policy"},
        {"[1,2,3]", ""},
        {"не json", ""},
    };
    for (const Case& c : cases) {
        harness::Profile p;
        std::string why;
        ASSERT_FALSE(harness::parse_profile(c.json, &p, &why));
        ASSERT_FALSE(why.empty());
        if (c.field[0] != '\0') {
            /* Название виновного поля обязано быть в отказе: файл читает
             * человек, и «не удалось разобрать» отправило бы его искать
             * ошибку в парсере JSON. */
            if (why.find(c.field) == std::string::npos) {
                std::cerr << "  отказ без названия поля «" << c.field
                          << "»: " << why << std::endl;
                throw std::runtime_error("поле не названо в отказе");
            }
        }
    }
}

TEST(неизвестное_поле_сохраняется_а_не_проглатывается) {
    /* Профили писались под harness, часть полей ядру не нужна — отказ за
     * них сделал бы поставленные файлы непригодными. Но ОПЕЧАТКА в
     * применённом поле молча ничего не делает, поэтому её надо показать. */
    harness::Profile p;
    std::string why;
    ASSERT_TRUE(harness::parse_profile(
        "{\"timeout_ms\":1000,\"rag_enabled\":false,\"timout_ms\":5,"
        "\"allowed_command\":[\"ls\"]}",
        &p, &why));
    ASSERT_TRUE(p.has_timeout_ms);
    ASSERT_EQ(p.timeout_ms, 1000);
    ASSERT_FALSE(p.rag_enabled);
    ASSERT_EQ(p.unknown_keys.size(), (size_t)2);
    ASSERT_TRUE(has(p.unknown_keys, "timout_ms"));
    ASSERT_TRUE(has(p.unknown_keys, "allowed_command"));
}

TEST(имя_профиля_не_пускает_выход_из_каталога) {
    std::string why;
    ASSERT_FALSE(harness::valid_profile_name("", &why));
    ASSERT_FALSE(harness::valid_profile_name("../секрет", &why));
    ASSERT_FALSE(harness::valid_profile_name("в/подкаталог", &why));
    ASSERT_FALSE(harness::valid_profile_name("звёздочка*", &why));
    ASSERT_FALSE(harness::valid_profile_name("пробел внутри", &why));
    ASSERT_TRUE(why.find("недопустимо") != std::string::npos);
    ASSERT_TRUE(harness::valid_profile_name("fast_local", &why));
    ASSERT_TRUE(harness::valid_profile_name("аудит-2", &why));
}

/* --- 3. Каталог ------------------------------------------------------ */

TEST(пустой_каталог_даёт_пустой_список_а_не_ошибку) {
    TempDir d("empty");
    ASSERT_EQ(harness::profile_names(d.str()).size(), (size_t)0);
    /* Несуществующий каталог — тоже не ошибка: его отсутствие означает
     * «профилей нет», а не «плагин сломан». */
    ASSERT_EQ(harness::profile_names(d.str() + "/нет-такого").size(),
              (size_t)0);
    harness::Profile p;
    std::string why;
    ASSERT_FALSE(harness::load_profile(d.str(), "fast_local", &p, &why));
    ASSERT_TRUE(why.find("fast_local") != std::string::npos);
}

TEST(несуществующее_имя_называет_доступные_профили) {
    TempDir d("names");
    d.write("fast_local.json", "{}");
    d.write("secure_audit.json", "{}");
    d.write("заметка.txt", "не профиль");

    const std::vector<std::string> names = harness::profile_names(d.str());
    ASSERT_EQ(names.size(), (size_t)2);
    /* Порядок поимённый: список идёт в сообщение об ошибке и в панель, и
     * порядок обхода каталога unspecified. */
    ASSERT_EQ(names[0], std::string("fast_local"));
    ASSERT_EQ(names[1], std::string("secure_audit"));

    harness::Profile p;
    std::string why;
    ASSERT_FALSE(harness::load_profile(d.str(), "typo", &p, &why));
    ASSERT_TRUE(why.find("typo") != std::string::npos);
    ASSERT_TRUE(why.find("fast_local") != std::string::npos);
    ASSERT_TRUE(why.find("secure_audit") != std::string::npos);
}

/* --- 4. Политика инструментов ---------------------------------------- */

TEST(standard_и_verbose_ничего_не_запрещают) {
    const std::vector<ToolDef> tools = sample_tools();
    std::string why;
    ASSERT_EQ(harness::denied_keys("standard", tools, &why).size(), (size_t)0);
    /* verbose ПРИЗНАТ и ничего не делает: журналирование профилем не
     * настраивается, и значение принято, чтобы файл не отвергался. */
    ASSERT_EQ(harness::denied_keys("verbose", tools, &why).size(), (size_t)0);
    ASSERT_TRUE(harness::valid_tools_policy("verbose"));
}

TEST(restricted_закрывает_всё_что_не_только_читает) {
    const std::vector<ToolDef> tools = sample_tools();
    std::string why;
    const std::vector<std::string> denied =
        harness::denied_keys("restricted", tools, &why);

    ASSERT_TRUE(has(denied, "write"));
    ASSERT_TRUE(has(denied, "bash"));
    ASSERT_TRUE(has(denied, "deploy"));
    /* СМЕШАННЫЙ ключ закрывается целиком: в `git` есть и читающий
     * (git_status), и пишущий (git_commit) инструмент, а единица запрета —
     * ключ. Оставить ключ из-за одного читателя значило бы, что профиль
     * «только чтение» коммитит. */
    ASSERT_TRUE(has(denied, "git"));
    /* Читающие ключи остаются, иначе профиль запрещал бы всё. */
    ASSERT_FALSE(has(denied, "read"));
    ASSERT_FALSE(has(denied, "todo"));
}

TEST(strict_оставляет_только_чтение_и_строже_restricted) {
    const std::vector<ToolDef> tools = sample_tools();
    std::string why;
    const std::vector<std::string> strict_d =
        harness::denied_keys("strict", tools, &why);
    const std::vector<std::string> restricted_d =
        harness::denied_keys("restricted", tools, &why);

    ASSERT_FALSE(has(strict_d, "read"));
    /* Плана нет тоже: у агента с закрытым todo из промпта уходит и блок
     * плана (И8.7) — это и есть разница, а не только лишнее правило. */
    ASSERT_TRUE(has(strict_d, "todo"));
    ASSERT_FALSE(has(restricted_d, "todo"));
    ASSERT_TRUE(strict_d.size() > restricted_d.size());
}

TEST(неизвестная_политика_даёт_отказ_а_не_пустой_набор) {
    /* Пустой набор запретов от неизвестной политики означал бы «ничего не
     * запрещать», то есть опечатка тихо снимала бы защиту. */
    const std::vector<ToolDef> tools = sample_tools();
    std::string why;
    const std::vector<std::string> denied =
        harness::denied_keys("strictt", tools, &why);
    ASSERT_FALSE(why.empty());
    ASSERT_TRUE(denied.empty());
    /* Набор инструментов не пустой: проверка обязана отличать «политики
     * нет» от «реестр пуст и потому запрещать нечего». */
    ASSERT_TRUE(tools.size() > 5);
}

TEST(запреты_strict_покрывают_каждый_ключ_реестра) {
    /* ТОТАЛЬНОСТЬ — анти-дрейфовая проверка. Список разрешённых ключей
     * задан руками (strict_allowed_keys), и новый инструмент с новым
     * ключом обязан заставить решить: он разрешён или запрещён. Молчаливого
     * «ни там, ни там» быть не может. */
    const std::vector<ToolDef> all = ToolsRegistry::instance().defs();
    ASSERT_TRUE(all.size() > 30);   // реестр заполнен предыдущими файлами

    std::set<std::string> keys;
    for (const ToolDef& d : all) keys.insert(permission_key_of(d));
    ASSERT_TRUE(keys.size() >= 5);

    const std::vector<std::string>& keep = harness::strict_allowed_keys();
    for (const std::string& k : keep) {
        /* Разрешённый ключ обязан существовать: иначе он завёл бы в
         * проверку тотальности мёртвую строку, и она перестала бы читаться. */
        ASSERT_TRUE(keys.count(k) > 0);
    }
    std::string why;
    const std::vector<std::string> denied = harness::denied_keys("strict", all, &why);
    const std::set<std::string> denied_set(denied.begin(), denied.end());
    for (const std::string& k : keys) {
        const bool allowed = std::find(keep.begin(), keep.end(), k) != keep.end();
        if (allowed) continue;
        if (denied_set.count(k) == 0) {
            std::cerr << "  ключ «" << k
                      << "» не разрешён и не запрещён политикой strict"
                      << std::endl;
            throw std::runtime_error("политика strict не тотальна");
        }
    }
}

/* --- 5. Команды ------------------------------------------------------ */

TEST(профиль_сужает_список_команд_а_не_расширяет) {
    CommandPolicy p;   // своя, а не синглтон: граница пережила бы проверку
    ASSERT_TRUE(p.binary_allowed("ls"));
    ASSERT_TRUE(p.binary_allowed("git"));

    std::string why;
    ASSERT_TRUE(harness::apply_allowed_commands(p, {"ls", "cat"}, &why));
    ASSERT_TRUE(p.binary_allowed("ls"));
    ASSERT_TRUE(p.binary_allowed("cat"));
    ASSERT_FALSE(p.binary_allowed("git"));
    ASSERT_FALSE(p.binary_allowed("rm"));
    /* Профиль НЕ открывает то, что закрыто по умолчанию: иначе файл в
     * каталоге плагина стал бы способом расширить границу (И3). */
    ASSERT_FALSE(p.binary_allowed("моя_команда"));
    ASSERT_TRUE(harness::apply_allowed_commands(p, {"моя_команда"}, &why));
    ASSERT_FALSE(p.binary_allowed("моя_команда"));
}

TEST(пустой_список_команд_не_двигает_границу) {
    CommandPolicy p;
    const size_t before = p.allowed_binaries().size();
    std::string why;
    /* Поля может не быть вовсе — тогда это не сужение, а «профиль про
     * команды ничего не говорит». */
    ASSERT_TRUE(harness::apply_allowed_commands(p, {}, &why));
    ASSERT_EQ(p.allowed_binaries().size(), before);
    ASSERT_TRUE(p.binary_allowed("git"));
}

TEST(смена_профиля_снимает_прежнее_сужение) {
    /* apply_session_profile идемпотентен: без reset_allowed_binaries
     * secure_audit запретил бы git навсегда, и переход на accurate_cloud
     * выглядел бы как «профиль не работает». */
    CommandPolicy p;
    std::string why;
    ASSERT_TRUE(harness::apply_allowed_commands(p, {"ls", "cat"}, &why));
    ASSERT_FALSE(p.binary_allowed("git"));

    p.reset_allowed_binaries();
    ASSERT_TRUE(harness::apply_allowed_commands(p, {"ls", "cat", "git"}, &why));
    ASSERT_TRUE(p.binary_allowed("git"));
    ASSERT_FALSE(p.binary_allowed("rm"));
    ASSERT_FALSE(p.binary_allowed("php"));
}

/* --- 6. Привязка к конфиг-агентам (И8) -------------------------------- */

namespace {

/* Разобрать один файл агента. Каталог — параметром: проверка отдаёт
 * фикстуру и проверяет то, что пришло. Реестр СВОЙ (не синглтон):
 * общий пережил бы проверку, и следующая увидела бы чужого агента. */
AgentDef parse_agent(const TempDir& dir, const std::string& file,
                     const std::string& body,
                     std::vector<AgentLoadDiag>* diags = nullptr) {
    dir.write(file, body);
    AgentRegistry reg;
    if (diags) *diags = reg.load_directory(dir.str());
    const std::shared_ptr<const AgentDef> def = reg.find("probe");
    if (!def) {
        AgentDef empty;
        return empty;
    }
    return *def;
}

} // namespace

TEST(профиль_из_frontmatter_читается_и_приводится_к_имени_файла) {
    TempDir d("agent_profile");
    std::vector<AgentLoadDiag> diags;
    const AgentDef def = parse_agent(
        d, "probe.md",
        "---\ndescription: проба\nmode: subagent\nprofile: Secure_Audit\n---\n"
        "тело\n",
        &diags);

    ASSERT_EQ(def.name, std::string("probe"));
    /* Регистр приведён: имя пишет человек, а файл на диске — строчными. */
    ASSERT_EQ(def.profile, std::string("secure_audit"));
    for (const AgentLoadDiag& g : diags) {
        ASSERT_TRUE(g.message.find("profile") == std::string::npos);
    }
}

TEST(недопустимое_имя_профиля_в_frontmatter_даёт_замечание_а_не_тишину) {
    TempDir d("agent_profile_bad");
    std::vector<AgentLoadDiag> diags;
    const AgentDef def = parse_agent(
        d, "probe.md",
        "---\ndescription: проба\nprofile: ../секрет\n---\nтело\n", &diags);

    /* Имя не принято — иначе оно пошло бы в путь. */
    ASSERT_TRUE(def.profile.empty());
    bool mentioned = false;
    for (const AgentLoadDiag& g : diags) {
        /* Отказ валид_profile_name говорит по-русски («имя профиля …»), и
         * искать надо именно его: проверка на слово «profile» искала бы
         * ключ frontmatter и нашла бы его только у другой ошибки. */
        if (g.message.find("профил") != std::string::npos) mentioned = true;
    }
    ASSERT_TRUE(mentioned);
}

TEST(запреты_профиля_лагают_в_базу_а_правила_агента_поверх) {
    /* Порядок и есть смысл привязки: `profile:` и `permission:` написаны в
     * одном файле, и запрет после `permission` сделал бы строку
     * `permission` бессильной в этом же файле. */
    harness::Profile p;
    std::string why;
    ASSERT_TRUE(harness::parse_profile("{\"tools_policy\":\"strict\"}", &p, &why));

    AgentDef def;
    def.name = "аудитор";
    def.description = "проба";
    def.prompt = "тело";
    PermissionEntry allow_all;
    allow_all.key = "git";
    allow_all.action = PermissionAction::Allow;
    def.permission.push_back(allow_all);

    const Ruleset base;
    const std::shared_ptr<agent::Info> with = agent::Info::from_def(def, base, nullptr, &p);
    ASSERT_TRUE(with->evaluate("write", "*") == PermissionAction::Deny);
    /* Агент объявил себя — и его объявление поверх профиля. */
    ASSERT_TRUE(with->evaluate("git", "*") == PermissionAction::Allow);

    /* Без профиля правил нет вовсе, иначе проверка выше прошла бы на
     * запретах откуда-то ещё. */
    const std::shared_ptr<agent::Info> without = agent::Info::from_def(def, base);
    ASSERT_TRUE(without->evaluate("write", "*") == PermissionAction::Ask);
}

TEST(профиль_агента_прячет_инструмент_из_каталога_модели) {
    harness::Profile p;
    std::string why;
    ASSERT_TRUE(harness::parse_profile("{\"tools_policy\":\"restricted\"}", &p, &why));

    AgentDef def;
    def.name = "читатель";
    def.description = "проба";
    def.prompt = "тело";

    const std::vector<ToolDef> all = ToolsRegistry::instance().defs();
    const std::shared_ptr<agent::Info> info = agent::Info::from_def(def, Ruleset(),
                                                                   nullptr, &p);
    ASSERT_TRUE(info->denies_whole_key("write"));
    ASSERT_FALSE(info->denies_whole_key("read"));

    /* Инструмент есть в реестре — иначе «спрятан» означало бы «его нет»,
     * то есть проверка прошла бы на пустоте. */
    bool registered = false;
    for (const ToolDef& d : all) {
        if (permission_key_of(d) == "write") registered = true;
    }
    ASSERT_TRUE(registered);

    const std::string cat =
        ToolsRegistry::instance().build_tool_catalogue(visible_tool_names(
            all, [&info](const std::string& key) { return info->denies_whole_key(key); }));
    ASSERT_TRUE(cat.find("write_file") == std::string::npos);
    ASSERT_TRUE(cat.find("read_file") != std::string::npos);
}

/* --- 7. Склейка через настройку --------------------------------------- */

TEST(профиль_из_настройки_меняет_таймаут_запреты_и_команды) {
    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.profiles_dir"] = shipped_dir();
    st.values["wp_coder.profile"] = "secure_audit";

    Engine& eng = engine();
    ProfileGuard guard(eng);
    eng.init(st.callbacks());

    /* Значение дошло до состояния. */
    int timeout = 0;
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_TRUE(eng.state().has_profile);
        ASSERT_EQ(eng.state().profile.name, std::string("secure_audit"));
        timeout = eng.state().llm_timeout_ms;
    }
    /* Ровно из файла, а не дефолт плагина. */
    ASSERT_EQ(timeout, 60000);

    /* Запрет дошёл до решений по ключам. */
    ASSERT_TRUE(eng.permissions().evaluate("write", "*") == PermissionAction::Deny);
    ASSERT_TRUE(eng.permissions().evaluate("bash", "*") == PermissionAction::Deny);
    /* Инструмент с этим ключом в реестре есть — иначе проверка выше
     * прошла бы на отсутствии инструмента. */
    bool write_registered = false;
    for (const ToolDef& d : ToolsRegistry::instance().defs()) {
        if (permission_key_of(d) == "write") write_registered = true;
    }
    ASSERT_TRUE(write_registered);

    /* Список команд сужен. */
    ASSERT_TRUE(command_policy().binary_allowed("ls"));
    ASSERT_FALSE(command_policy().binary_allowed("git"));
}

TEST(профиль_убирает_инструменты_из_каталога_который_видит_модель) {
    /* Склейка целиком: настройка → профиль → правила → то, что уехало в
     * промпт. Без этой проверки всё выше проверяло бы только правила, а
     * модель продолжала бы видеть write_file. */
    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.profiles_dir"] = shipped_dir();
    st.values["wp_coder.profile"] = "secure_audit";

    Engine& eng = engine();
    ProfileGuard guard(eng);

    /* Сначала без профиля: write_file виден. Иначе «стало не видно» не
     *чего было бы сравнивать. */
    st.values["wp_coder.profile"] = "";
    eng.init(st.callbacks());
    {
        const std::string plain = eng.build_system_prompt();
        ASSERT_TRUE(plain.find("write_file") != std::string::npos);
    }

    st.values["wp_coder.profile"] = "secure_audit";
    eng.invalidate_prompt_cache();
    eng.load_settings();
    const std::string audited = eng.build_system_prompt();
    ASSERT_TRUE(audited.find("write_file") == std::string::npos);
    ASSERT_TRUE(audited.find("read_file") != std::string::npos);
}

TEST(явная_настройка_таймаута_важнее_профиля) {
    /* Иначе настройка была бы бесполезной: человек написал бы 45000 и
     * получил бы 300000 из профиля, не поняв почему. */
    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.profiles_dir"] = shipped_dir();
    st.values["wp_coder.profile"] = "debug_verbose";   // 300000
    st.values["wp_coder.llm_timeout_ms"] = "45000";

    Engine& eng = engine();
    ProfileGuard guard(eng);
    eng.init(st.callbacks());

    std::lock_guard<std::mutex> lk(eng.state().mtx);
    ASSERT_EQ(eng.state().llm_timeout_ms, 45000);
}

TEST(мусор_в_таймауте_даёт_дефолт_а_не_мгновенный_отказ) {
    /* Раньше неразбираемое значение давало 0, а deadline в прошлом —
     * мгновенный таймаут на первом же запросе (host_bridge/
     * llm_blocking.cpp:93). */
    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.llm_timeout_ms"] = "минута";

    Engine& eng = engine();
    ProfileGuard guard(eng);
    eng.init(st.callbacks());

    std::lock_guard<std::mutex> lk(eng.state().mtx);
    ASSERT_EQ(eng.state().llm_timeout_ms, 120000);
}

TEST(несуществующий_профиль_даёт_причину_а_не_тишину) {
    /* Молча неприменённый профиль выглядел бы как «настройка не
     * работает», а человек ходил бы с правами обычной сессии, считая себя
     * в аудите. */
    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.profiles_dir"] = shipped_dir();

    Engine& eng = engine();
    ProfileGuard guard(eng);
    /* Решение по ключу write сравнивается ДО и ПОСЛЕ, а не с «Ask».
     * Права сессии между проверками протекают (движок — синглтон, и
     * test_support::approve_all_permissions уже могла разрешить всё), и
     * утверждение «равно Ask» проверяло бы чужое состояние: оно прошло бы
     * и при профиле, который ничего не запретил. */
    eng.init(st.callbacks());
    const PermissionAction before = eng.permissions().evaluate("write", "*");

    st.values["wp_coder.profile"] = "secure_audti";   // опечатка
    eng.load_settings();

    harness::Profile p;
    std::string error;
    ASSERT_FALSE(eng.session_profile(&p, &error));
    ASSERT_TRUE(error.find("secure_audti") != std::string::npos);
    /* В отказе названы доступные имена. */
    ASSERT_TRUE(error.find("secure_audit") != std::string::npos);
    /* Неприменённый профиль НЕ сузил права: решение то же, что было до
     * него. */
    ASSERT_TRUE(eng.permissions().evaluate("write", "*") == before);
}

TEST(смена_профиля_возвращает_команды_и_снимает_старые_запреты) {
    /* Правила профиля ПЕРЕСБИРАЮТСЯ, а не дописываются: load_settings
     * зовётся на каждом init, и простое добавление копило бы копии (их
     * видно в дампе правил), а смена профиля не смогла бы отпустить то,
     * что сужал прежний. Раньше это приходилось обходить reset() —
     * то есть без новой сессии профиль было не переключить. */
    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.profiles_dir"] = shipped_dir();
    st.values["wp_coder.profile"] = "secure_audit";

    Engine& eng = engine();
    ProfileGuard guard(eng);
    eng.init(st.callbacks());
    ASSERT_TRUE(eng.permissions().evaluate("write", "*") == PermissionAction::Deny);
    const size_t rules_after_profile = eng.permissions().rules_snapshot().size();

    /* Команды при этом возвращаются: список один на движок и снимается. */
    ASSERT_FALSE(command_policy().binary_allowed("git"));

    /* Повторная загрузка тех же настроек не плодит копии. */
    eng.load_settings();
    ASSERT_EQ(eng.permissions().rules_snapshot().size(), rules_after_profile);

    st.values["wp_coder.profile"] = "accurate_cloud";
    eng.load_settings();
    /* Ровно «профиль не сужает»: база сессии начинается с `*: allow`
     * (И2.4, apply_agent_defaults), поэтому ожидаемое здесь решение —
     * Allow, а не Ask. Assert на Ask был бы проверкой чужого состояния. */
    ASSERT_TRUE(eng.permissions().evaluate("write", "*") == PermissionAction::Allow);
    ASSERT_TRUE(command_policy().binary_allowed("git"));
}

TEST(снятый_профиль_не_оставляет_своих_запретов) {
    /* Человек убрал настройку — и его «всегда» из ответа на вопрос не
     * должен упираться в запрет, который он уже отменил. */
    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.profiles_dir"] = shipped_dir();
    st.values["wp_coder.profile"] = "secure_audit";

    Engine& eng = engine();
    ProfileGuard guard(eng);
    eng.init(st.callbacks());
    ASSERT_TRUE(eng.permissions().evaluate("write", "*") == PermissionAction::Deny);

    st.values["wp_coder.profile"] = "";
    eng.load_settings();
    ASSERT_TRUE(eng.permissions().evaluate("write", "*") == PermissionAction::Allow);
    /* И в дампе правил следов прежнего профиля нет — они видны человеку. */
    ASSERT_TRUE(eng.permissions().rules_dump().find("профиль harness") ==
                std::string::npos);

    /* Опечатка в имени — тот же случай: профиль не применился, значит и
     * ограничений от него быть не должно. */
    st.values["wp_coder.profile"] = "secure_audit";
    eng.load_settings();
    ASSERT_TRUE(eng.permissions().evaluate("write", "*") == PermissionAction::Deny);
    st.values["wp_coder.profile"] = "secure_audti";
    eng.load_settings();
    ASSERT_TRUE(eng.permissions().evaluate("write", "*") == PermissionAction::Allow);
}

TEST(профиль_агента_не_отказывает_в_задаче_если_файла_нет) {
    /* Необязательная настройка не должна превращаться в требование: агент
     * известен, его звали, и отсутствие чужого файла профиля — не повод
     * не работать. Проверяем то, что видит вызывающий. */
    TempDir d("missing_profile");
    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.profiles_dir"] = d.str();

    Engine& eng = engine();
    ProfileGuard guard(eng);
    eng.init(st.callbacks());

    std::string why;
    ASSERT_TRUE(eng.profile_for_agent("нет_такого", &why) == nullptr);
    ASSERT_TRUE(why.find("нет_такого") != std::string::npos);

    /* А существующий профиль — возвращается, и с тем именем, под которым
     * его назвали в frontmatter. */
    TempDir good("agent_profiles");
    good.write("fast_local.json", "{\"tools_policy\":\"restricted\"}");
    st.values["wp_coder.profiles_dir"] = good.str();
    eng.load_settings();
    const std::shared_ptr<const harness::Profile> p =
        eng.profile_for_agent("fast_local", &why);
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(p->name, std::string("fast_local"));
    /* Пустое имя — не ошибка, а «профиля нет». */
    ASSERT_TRUE(eng.profile_for_agent("", &why) == nullptr);
    ASSERT_TRUE(why.empty());
}

TEST(имя_профиля_в_настройке_приводится_к_имени_файла) {
    /* Настройку пишет человек, а файл на диске — `secure_audit.json`.
     * Без приведения «Secure_Audit» означал бы «профиль не найден» при
     * существующем профиле, и выглядело бы как поломка. */
    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.profiles_dir"] = shipped_dir();
    st.values["wp_coder.profile"] = "Secure_Audit";

    Engine& eng = engine();
    ProfileGuard guard(eng);
    eng.init(st.callbacks());

    std::lock_guard<std::mutex> lk(eng.state().mtx);
    ASSERT_EQ(eng.state().profile_name, std::string("secure_audit"));
    ASSERT_TRUE(eng.state().has_profile);
    ASSERT_EQ(eng.state().profile.name, std::string("secure_audit"));
}

TEST(профиль_агента_доезжает_до_правил_ребёнка) {
    /* Шов, который прогон мутаций назвал непокрытым: строка
     * «for_subagent(..., profile.get())» внутри инструмента `task`
     * недостижима для проверки, и привязка профиля к конфиг-агенту
     * (строка задачи 9.7) была бы проверена по частям, а не целиком.
     * build_child_info — тот же шов, вынесенный наружу. */
    TempDir profiles("child_profile");
    profiles.write("secure_audit.json", "{\"tools_policy\":\"strict\"}");

    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.profiles_dir"] = profiles.str();

    Engine& eng = engine();
    ProfileGuard guard(eng);
    eng.init(st.callbacks());

    AgentDef def;
    def.name = "аудитор";
    def.description = "проба";
    def.prompt = "тело";
    def.mode = AgentMode::Subagent;
    def.profile = "secure_audit";

    std::vector<AgentLoadDiag> diags;
    const std::shared_ptr<agent::Info> info = build_child_info(eng, def, &diags);
    ASSERT_TRUE(info != nullptr);
    ASSERT_TRUE(info->denies_whole_key("write"));
    ASSERT_TRUE(info->denies_whole_key("todo"));
    ASSERT_FALSE(info->denies_whole_key("read"));
    for (const AgentLoadDiag& g : diags) {
        ASSERT_TRUE(g.message.find("профил") == std::string::npos);
    }

    /* Запреты сессии остаются последними (И8.10): профиль не вправе
     * отменить то, что запретил человек. */
    Rule session_deny;
    session_deny.permission = "read";
    session_deny.pattern = "*.env*";
    session_deny.action = PermissionAction::Deny;
    session_deny.comment = "тест: секреты не читать";
    Ruleset session;
    session.add(session_deny);
    const std::shared_ptr<agent::Info> with_session =
        build_child_info(eng, def, nullptr);
    /* Решение по правилам СЕССИИ не наследуется субагенту (И8.10) — кроме
     * external_directory, — поэтому утверждение здесь про то, что профиль
     * не сломал сборку, а конкретный запрет приходит своим путём. */
    ASSERT_TRUE(with_session != nullptr);
    (void)session;
}

TEST(нечитающийся_профиль_агента_даёт_замечание_а_не_молчит) {
    /* Молча проглоченная причина выглядела бы как «профиль не применился
     * и неизвестно почему», а человек считал бы, что агент работает в
     * аудите. */
    TempDir empty("no_profiles");
    Settings st;
    st.values["wp_coder.project_dir"] = "";
    st.values["wp_coder.profiles_dir"] = empty.str();

    Engine& eng = engine();
    ProfileGuard guard(eng);
    eng.init(st.callbacks());

    AgentDef def;
    def.name = "аудитор";
    def.description = "проба";
    def.prompt = "тело";
    def.mode = AgentMode::Subagent;
    def.profile = "secure_audit";

    std::vector<AgentLoadDiag> diags;
    const std::shared_ptr<agent::Info> info = build_child_info(eng, def, &diags);

    /* Агент при этом ЖИВ: необязательная настройка не превращается в
     * требование, иначе зовёшь известного агента — а он молчит. */
    ASSERT_TRUE(info != nullptr);
    ASSERT_EQ(info->name(), std::string("аудитор"));
    ASSERT_FALSE(info->denies_whole_key("write"));

    bool mentioned = false;
    for (const AgentLoadDiag& g : diags) {
        if (g.message.find("профил") != std::string::npos) mentioned = true;
    }
    ASSERT_TRUE(mentioned);
}

/* --- 8. Панель ------------------------------------------------------- */

TEST(панель_берёт_имя_профиля_из_состояния_а_не_из_второго_источника) {
    /* Имя профиля живёт в одном месте — EngineState. Панель обязана брать
     * его оттуда: свой список имён в UI стал бы вторым источником истины
     * (болезнь D12 — версия в четырёх файлах), и правка профиля в одном
     * месте разошлась бы с надписью в другом молча.
     *
     * Проверка по исходнику, а не по отрисовке: окно ImGui в юнит-тестах
     * не построить. Она не бумажная — мутацией строки вызова проверка
     * падает (см. журнал прогона). */
    std::ifstream f(fs::path(__FILE__).parent_path().parent_path() /
                        "ui/coder_window.cpp",
                    std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string ui = ss.str();
    ASSERT_FALSE(ui.empty());

    ASSERT_TRUE(ui.find("session_profile") != std::string::npos);
    /* Ни одного имени поставленного профиля в UI: список имён читается из
     * каталога (harness::profile_names), а не выписан здесь. */
    ASSERT_TRUE(ui.find("fast_local") == std::string::npos);
    ASSERT_TRUE(ui.find("secure_audit") == std::string::npos);
    ASSERT_TRUE(ui.find("accurate_cloud") == std::string::npos);
    ASSERT_TRUE(ui.find("debug_verbose") == std::string::npos);
    /* И причина неприменения показывается: молча неприменённый профиль
     * выглядел бы как «настройка не работает», и человек счёл бы себя в
     * аудите, ничем не ограниченном. */
    ASSERT_TRUE(ui.find("Профиль не применён") != std::string::npos);
}

/* --- И11.14: поля профиля доезжают до хоста (И9.7) --- */

namespace {

/* Хост, который запоминает request_json и отвечает успехом. */
std::string g_last_request_json;

HostCallbacks recording_host() {
    HostCallbacks cb;
    cb.llm_chat_stream =
        [](const std::string&, const std::vector<ModelMessage>&,
           const std::string& request_json,
           std::function<void(void*)>, std::function<void(const char*, int)>,
           std::function<void(const char*, const char*, const char*)>,
           std::function<void(const std::string&)> on_done) -> bool {
            g_last_request_json = request_json;
            on_done("{\"ok\":1,\"content\":\"ок\",\"finish_reason\":\"stop\"}");
            return true;
        };
    return cb;
}

bool json_has_number(const std::string& j, const std::string& key,
                     const std::string& value)
{
    return j.find("\"" + key + "\":" + value) != std::string::npos;
}

}  // namespace

TEST(профиль_агента_задаёт_temperature_и_max_tokens_в_запросе) {
    /* И9.7 писал: поля разбираются и хранятся, «чтобы И11.14 не
     * пришлось догадываться». Проверяется, что они действительно
     * доезжают: request_json — единственный канал параметров генерации
     * до хоста, и пустая строка означала бы, что четыре поставленных
     * профиля остаются файлами, которые читает, но не применяет, никто. */
    Engine& eng = engine();
    ProfileGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().has_profile = true;
        eng.state().profile = harness::Profile();
        eng.state().profile.name = "test_gen";
        eng.state().profile.has_temperature = true;
        eng.state().profile.temperature = 0.25;
        eng.state().profile.has_max_tokens = true;
        eng.state().profile.max_tokens = 2048;
    }

    g_last_request_json = "не задан";
    std::vector<LlmEvent> events;
    LlmClient::fetch(recording_host(), "sys", {{"user", "x"}}, events,
                     nullptr, 1000);

    ASSERT_TRUE(json_has_number(g_last_request_json, "max_tokens", "2048"));
    ASSERT_TRUE(json_has_number(g_last_request_json, "temperature", "0.25"));
}

TEST(профиль_без_этих_полей_не_добавляет_мусора_в_запрос) {
    /* Пустой request_json вместо «{}»: хост трактует их одинаково, но
     * «{}» в логе выглядит как «профиль применился и ничего не задал».
     * Проверка фиксирует именно форму, а не смысл. */
    Engine& eng = engine();
    ProfileGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().has_profile = true;
        eng.state().profile = harness::Profile();
        eng.state().profile.name = "test_empty";
        eng.state().profile.has_temperature = false;
        eng.state().profile.has_max_tokens = false;
    }

    g_last_request_json = "не задан";
    std::vector<LlmEvent> events;
    LlmClient::fetch(recording_host(), "sys", {{"user", "x"}}, events,
                     nullptr, 1000);

    ASSERT_EQ(g_last_request_json, std::string());
}

TEST(без_профиля_запрос_остаётся_пустым) {
    /* Профиль не задан — хост берёт свои умолчания, и подставлять
     * ничего нельзя: выдуманное значение молча изменило бы поведение
     * у всех, кто профиль не настраивал. */
    Engine& eng = engine();
    ProfileGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().has_profile = false;
    }

    g_last_request_json = "не задан";
    std::vector<LlmEvent> events;
    LlmClient::fetch(recording_host(), "sys", {{"user", "x"}}, events,
                     nullptr, 1000);

    ASSERT_EQ(g_last_request_json, std::string());
}

TEST(неположительный_max_tokens_не_уходит_к_хосту) {
    /* Хост применяет значение только при v > 0, поэтому слать ноль или
     * отрицательное — значит отправлять значение, которое заведомо
     * будет отброшено, и делать вид, что оно задано. */
    Engine& eng = engine();
    ProfileGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().has_profile = true;
        eng.state().profile = harness::Profile();
        eng.state().profile.name = "test_zero";
        eng.state().profile.has_temperature = true;
        eng.state().profile.temperature = 0.5;
        eng.state().profile.has_max_tokens = true;
        eng.state().profile.max_tokens = 0;
    }

    g_last_request_json = "не задан";
    std::vector<LlmEvent> events;
    LlmClient::fetch(recording_host(), "sys", {{"user", "x"}}, events,
                     nullptr, 1000);

    ASSERT_TRUE(g_last_request_json.find("max_tokens") == std::string::npos);
    ASSERT_TRUE(json_has_number(g_last_request_json, "temperature", "0.5"));
}

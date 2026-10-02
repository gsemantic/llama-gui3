/*
 * test_instruction.cpp — И9.1: загрузка инструкций проекта и пользователя.
 *
 * Проверяется не «функция существует», а семь контрактов, на которых
 * потом стоит И9.2–9.3:
 *
 *   1. ПРИОРИТЕТ И ПОРЯДОК. Глобальный AGENTS.md, потом проектные
 *      AGENTS.md и CLAUDE.md, потом записи настройки — в том порядке, в
 *      котором они попадают в промпт.
 *   2. ОДИН БЛОК НА ИСТОЧНИК, с заголовком. Склеенные инструкции
 *      неотличимы от требований автора промпта.
 *   3. НЕЗАГРУЖЕННЫЙ ИСТОЧНИК НЕ ПОПАДАЕТ В ПРОМПТ. Текст ошибки curl
 *      в блоке «Instructions from:» читался бы модели как инструкция.
 *   4. ДЕДУПЛИКАЦИЯ. Тот же файл из настройки и из корня проекта — один
 *      блок, иначе правила тихо удваиваются.
 *   5. РАЗБОР НАСТРОЙКИ. Только JSON-массив строк; мусор в одной записи
 *      не лишает остальных.
 *   6. ШАБЛОН. Порядок результата — по пути, а не по времени изменения:
 *      иначе кэш промпта зависел бы от того, когда файлы трогали.
 *   7. ЖИВОЙ ПРОМПТ. Собранный Engine::build_system_prompt содержит
 *      инструкции — иначе все пункты выше проверяли бы функцию, которой
 *      никто не пользуется.
 *
 * Чего здесь НЕТ по уважительной причине: сети. URL проверяется
 * подменённым загрузчиком, и настоящий fetch_url() не вызывается ни
 * разу — проверка, зависящая от внешнего сервера, через год становится
 * проверкой сети, и её чинит уже никто.
 */

#include "test_framework.h"
#include "../core/engine.h"
#include "../core/instruction.h"
#include "../core/limits.h"
#include "../core/skills_manager.h"
#include "../core/prompts.h"
#include "../core/base_tools.h"
#include "../core/security.h"
#include "../core/tools_registry.h"
#include "test_support.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace coder;

namespace fs = std::filesystem;

namespace {

/* Каталог на время теста. Убирается в деструкторе, и убирает он СЕБЯ,
 * а не переданный путь: проверка, которая оставляет мусор, хуже
 * проверки, которой нет. */
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("wp_coder_instr_" + tag + "_" + std::to_string(++counter));
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
    std::string at(const std::string& rel) const {
        return (path / rel).string();
    }
};

/* Сколько источников с такой подписью. */
size_t count_label(const std::vector<Instruction>& items,
                   const std::string& label) {
    size_t n = 0;
    for (const Instruction& in : items) {
        if (in.label == label) ++n;
    }
    return n;
}

/* Аргументы skill_detail: единственный обязательный — QUERY. */
json::JsonValue skill_args(const std::string& query) {
    json::JsonValue a = json::JsonValue::object();
    a.set("query", query);
    return a;
}

size_t count(const std::vector<Instruction>& items, const std::string& needle) {
    size_t n = 0;
    for (const Instruction& in : items) {
        if (in.body.find(needle) != std::string::npos) ++n;
    }
    return n;
}

/* Загрузчик URL, который не ходит в сеть и запоминает просьбы.
 *
 * std::function КОПИРУЕТ то, во что его завернули, поэтому состояние
 * лежит в разделяемом указателе, а не в самом объекте: состояние в
 * объекте осталось бы в локальной копии, и проверка «загрузчик вообще
 * спросили» спрашивала бы не ту копию. Первая версия теста именно так и
 * проходила — при провале URL число просьб было нулём у обеих сторон. */
struct FakeFetcher {
    struct State {
        std::vector<std::string> asked;
        std::string body = "правила из сети\n";
        bool ok = true;
    };
    std::shared_ptr<State> st = std::make_shared<State>();
    bool operator()(const std::string& url, std::string& out) const {
        st->asked.push_back(url);
        if (!st->ok) return false;
        out = st->body;
        return true;
    }
    size_t asked_count() const { return st->asked.size(); }
    void set_body(const std::string& b) { st->body = b; }
    void set_ok(bool v) { st->ok = v; }
};

/* Подменить HOME на время теста. Глобальный AGENTS.md — единственный
 * источник вне проекта, и без подмены проверка зависела бы от того,
 * что лежит у того, кто её запускает. */
struct FakeHome {
    std::string saved;
    bool had = false;
    explicit FakeHome(const std::string& dir) {
        const char* h = std::getenv("HOME");
        had = (h != nullptr);
        saved = had ? std::string(h) : std::string();
        ::setenv("HOME", dir.c_str(), 1);
    }
    ~FakeHome() {
        if (had) {
            ::setenv("HOME", saved.c_str(), 1);
        } else {
            ::unsetenv("HOME");
        }
    }
};

/* Состояние движка протекает между тестами (Engine — синглтон,
 * правило 5 SESSION_START.md), поэтому каждая проверка, которая его
 * трогает, обязана вернуть как было. Иначе инструкции одного проекта
 * остались бы в промпте следующего теста, и упавший тест указывал бы
 * не туда. */
struct EngineStateGuard {
    Engine& e;
    std::string project_dir;
    std::vector<Instruction> instructions;
    bool loaded = false;
    std::string config;
    std::string cached;
    bool dirty = true;
    RunScope scope;

    explicit EngineStateGuard(Engine& engine)
        : e(engine), scope(engine.state().scope) {
        std::lock_guard<std::mutex> lk(e.state().mtx);
        project_dir = e.state().project_dir;
        instructions = e.state().instructions;
        loaded = e.state().instructions_loaded;
        config = e.state().instructions_config;
        cached = e.state().cached_system_prompt;
        dirty = e.state().prompt_dirty;
    }
    ~EngineStateGuard() {
        std::lock_guard<std::mutex> lk(e.state().mtx);
        e.state().project_dir = project_dir;
        e.state().instructions = instructions;
        e.state().instructions_loaded = loaded;
        e.state().instructions_config = config;
        e.state().cached_system_prompt = cached;
        e.state().prompt_dirty = dirty;
        e.state().scope = scope;
        /* Колбэк настроек в синглтоне после init может быть висящим:
         * его ставит проверка с локальной таблицей, а таблица умирает вместе
         * с ней. Поэтому после себя оставляем безобидный, чтобы следующая
         * проверка не Segmentation fault'нулась на чужом мусоре. */
        e.callbacks().settings_get =
            [](const std::string&, const std::string& d) { return d; };
    }
};

} // namespace

/* --- 1. Порядок и приоритет имён ----------------------------------- */

TEST(инструкции_читаются_в_порядке_глобальный_проектные_настройка) {
    TempDir home("home");
    home.write(".config/wp_coder/AGENTS.md", "ГЛОБАЛЬНОЕ\n");
    TempDir proj("proj");
    proj.write("AGENTS.md", "ПРОЕКТНОЕ AGENTS\n");
    proj.write("CLAUDE.md", "ПРОЕКТНЫЙ CLAUDE\n");
    /* Запись настройки указывает на СУЩЕСТВУЮЩИЙ файл: иначе проверка
     * порядка прошла бы на отсутствии источника, а это ровно тот
     * случай, когда проверка ничего не проверяет. */
    proj.write("правила.txt", "ИЗ НАСТРОЙКИ\n");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items = instruction::load(
        proj.path.string(), "[\"правила.txt\"]", fetch);
    ASSERT_EQ(items.size(), (size_t)4);
    ASSERT_EQ(items[0].label, std::string("~/.config/wp_coder/AGENTS.md"));
    ASSERT_EQ(items[0].body, std::string("ГЛОБАЛЬНОЕ\n"));
    ASSERT_EQ(items[1].label, std::string("AGENTS.md"));
    ASSERT_EQ(items[2].label, std::string("CLAUDE.md"));
    ASSERT_EQ(items[3].label, std::string("правила.txt"));
    ASSERT_EQ(items[3].body, std::string("ИЗ НАСТРОЙКИ\n"));
}

TEST(имя_файла_инструкций_даёт_приоритет_а_не_выбор_одного_из_двух) {
    /* AGENTS.md и CLAUDE.md — не альтернативы: если CLAUDE.md молча
     * выпадал бы при наличии AGENTS.md, пользователь потерял бы написанное
     * им правило и не узнал бы об этом. Приоритет — это ПОРЯДОК. */
    TempDir home("home2");
    TempDir proj("proj2");
    proj.write("AGENTS.md", "A\n");
    proj.write("CLAUDE.md", "C\n");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items = instruction::load(proj.path.string(), "", fetch);
    ASSERT_EQ(items.size(), (size_t)2);
    ASSERT_EQ(items[0].label, std::string("AGENTS.md"));
    ASSERT_EQ(items[1].label, std::string("CLAUDE.md"));
}

TEST(без_корня_проекта_читается_только_глобальная_инструкция) {
    TempDir home("home3");
    home.write(".config/wp_coder/AGENTS.md", "ГЛОБАЛЬНОЕ\n");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items = instruction::load("", "", fetch);
    ASSERT_EQ(items.size(), (size_t)1);
    ASSERT_EQ(items[0].label, std::string("~/.config/wp_coder/AGENTS.md"));
}

/* --- 2. Блок на источник ------------------------------------------- */

TEST(каждый_источник_даёт_свой_заголовок_а_не_склейку) {
    TempDir home("home4");
    home.write(".config/wp_coder/AGENTS.md", "ПРАВИЛО А\n");
    TempDir proj("proj4");
    proj.write("AGENTS.md", "ПРАВИЛО Б\n");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items = instruction::load(proj.path.string(), "", fetch);
    ASSERT_EQ(items.size(), (size_t)2);
    const std::string text = instruction::render(items);

    /* Заголовок формата назван в kBaseSystemPrompt: строка, о которой
     * модель не знает, ею и не пользуется (общее правило D2). */
    ASSERT_TRUE(text.find("## Instructions from: ~/.config/wp_coder/AGENTS.md")
                != std::string::npos);
    ASSERT_TRUE(text.find("## Instructions from: AGENTS.md")
                != std::string::npos);
    /* И тело каждого источника — рядом со своим заголовком, а не одно
     * склеенное. */
    ASSERT_TRUE(text.find("ПРАВИЛО А") != std::string::npos);
    ASSERT_TRUE(text.find("ПРАВИЛО Б") != std::string::npos);
    const size_t head_a = text.find("Instructions from: ~/.config");
    const size_t body_a = text.find("ПРАВИЛО А");
    const size_t head_b = text.find("Instructions from: AGENTS.md");
    const size_t body_b = text.find("ПРАВИЛО Б");
    ASSERT_TRUE(head_a < body_a && body_a < head_b && head_b < body_b);
}

TEST(пустой_список_инструкций_не_даёт_пустых_заголовков) {
    const std::vector<Instruction> none;
    ASSERT_EQ(instruction::render(none), std::string(""));
    Instruction empty;
    empty.label = "AGENTS.md";
    ASSERT_EQ(instruction::render({empty}), std::string(""));
}

/* --- 3. Незагруженный источник не попадает в промпт ----------------- */

TEST(недоступный_url_не_становится_инструкцией) {
    /* Ответ curl об ошибке — это текст ПРОВЫЙДЕРА, а не инструкция.
     * В промпте он выглядел бы как требование автора, и модель
     * выполняла бы его всю сессию. */
    TempDir home("home5");
    TempDir proj("proj5");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;
    fetch.set_ok(false);

    const auto items =
        instruction::load(proj.path.string(), "[\"https://example.invalid/r.md\"]",
                          fetch);
    ASSERT_EQ(fetch.asked_count(), (size_t)1);
    ASSERT_EQ(items.size(), (size_t)0);
    ASSERT_EQ(instruction::render(items), std::string(""));
}

TEST(несуществующий_файл_не_даёт_блока) {
    TempDir home("home6");
    TempDir proj("proj6");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items =
        instruction::load(proj.path.string(), "[\"нет-такого.md\"]", fetch);
    ASSERT_EQ(items.size(), (size_t)0);
}

/* --- 4. Дедупликация ------------------------------------------------ */

TEST(файл_названный_дважды_грузится_один_раз) {
    /* Настройка, повторяющая AGENTS.md, иначе тихо удваивала бы
     * правила в каждом запросе — и модель получила бы два источника,
     * которые выглядят независимыми, а означают одно и то же. */
    TempDir home("home7");
    TempDir proj("proj7");
    proj.write("AGENTS.md", "ПРАВИЛО\n");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items = instruction::load(
        proj.path.string(), "[\"AGENTS.md\", \"./AGENTS.md\"]", fetch);
    ASSERT_EQ(items.size(), (size_t)1);
    ASSERT_EQ(items[0].label, std::string("AGENTS.md"));
    ASSERT_EQ(count(items, "ПРАВИЛО"), (size_t)1);
}

TEST(глобальный_и_проектный_файлы_это_разные_источники) {
    /* Обратная сторона дедупликации: дедуплицировать по ИМЕНИ нельзя,
     * иначе глобальные правила исчезли бы у любого проекта с
     * AGENTS.md. */
    TempDir home("home8");
    home.write(".config/wp_coder/AGENTS.md", "ГЛОБАЛЬНОЕ\n");
    TempDir proj("proj8");
    proj.write("AGENTS.md", "ПРОЕКТНОЕ\n");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items = instruction::load(proj.path.string(), "", fetch);
    ASSERT_EQ(items.size(), (size_t)2);
    ASSERT_EQ(count(items, "ГЛОБАЛЬНОЕ"), (size_t)1);
    ASSERT_EQ(count(items, "ПРОЕКТНОЕ"), (size_t)1);
}

/* --- 5. Разбор настройки -------------------------------------------- */

TEST(настройка_читается_как_json_массив_строк) {
    const auto v = instruction::parse_config_list(
        "[\"a.md\", \"docs/*.md\", \"https://example.com/r.md\"]");
    ASSERT_EQ(v.size(), (size_t)3);
    ASSERT_EQ(v[0], std::string("a.md"));
    ASSERT_EQ(v[1], std::string("docs/*.md"));
    ASSERT_EQ(v[2], std::string("https://example.com/r.md"));
}

TEST(мусор_в_одной_записи_не_лишает_остальных) {
    const auto v = instruction::parse_config_list(
        "[\"хорошая.md\", 42, \"\", \"   \", null, \"вторая.md\"]");
    ASSERT_EQ(v.size(), (size_t)2);
    ASSERT_EQ(v[0], std::string("хорошая.md"));
    ASSERT_EQ(v[1], std::string("вторая.md"));
}

TEST(настройка_не_json_и_пустая_дают_пустой_список) {
    ASSERT_EQ(instruction::parse_config_list("").size(), (size_t)0);
    ASSERT_EQ(instruction::parse_config_list("   \n ").size(), (size_t)0);
    ASSERT_EQ(instruction::parse_config_list("{\"a\": 1}").size(), (size_t)0);
    ASSERT_EQ(instruction::parse_config_list("AGENTS.md").size(), (size_t)0);
}

/* --- 6. Шаблон ------------------------------------------------------ */

TEST(шаблон_из_настройки_находит_файлы_порядком_по_пути) {
    /* Порядок результата задаётся путём, а не временем изменения: инструкции
     * попадают в кэш промпта, и «свежие сверху» сделало бы его
     * содержимое зависящим от того, когда файлы трогали. */
    TempDir home("home9");
    TempDir proj("proj9");
    proj.write("docs/a.md", "A\n");
    proj.write("docs/b.md", "B\n");
    proj.write("docs/c.md", "C\n");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items = instruction::load(proj.path.string(),
                                         "[\"docs/*.md\"]", fetch);
    ASSERT_EQ(items.size(), (size_t)3);
    ASSERT_EQ(items[0].label, std::string("docs/a.md"));
    ASSERT_EQ(items[1].label, std::string("docs/b.md"));
    ASSERT_EQ(items[2].label, std::string("docs/c.md"));
}

TEST(тильда_в_настройке_раскрывается) {
    TempDir home("home10");
    home.write("правила.md", "ДОМАШНИЕ\n");
    TempDir proj("proj10");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items = instruction::load(proj.path.string(),
                                         "[\"~/правила.md\"]", fetch);
    ASSERT_EQ(items.size(), (size_t)1);
    ASSERT_EQ(items[0].body, std::string("ДОМАШНИЕ\n"));
}

TEST(содержимое_инструкции_идёт_без_bom_и_с_lf) {
    /* BOM в промпте невидим и приклеивается к слову первой строки, то
     * есть правило начинается с мусора. */
    TempDir home("home11");
    TempDir proj("proj11");
    proj.write("AGENTS.md", "\xEF\xBB\xBFПРАВИЛО\r\nВТОРОЕ\r\n");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items = instruction::load(proj.path.string(), "", fetch);
    ASSERT_EQ(items.size(), (size_t)1);
    ASSERT_TRUE(items[0].body.find("\xEF\xBB\xBF") == std::string::npos);
    ASSERT_TRUE(items[0].body.find("\r") == std::string::npos);
    ASSERT_EQ(items[0].body, std::string("ПРАВИЛО\nВТОРОЕ\n"));
}

TEST(длинный_источник_обрезается_с_явной_пометкой) {
    /* Молчаливое усечение выглядело бы как полное правило, и модель
     * применяла бы половину требования, не зная об этом. */
    TempDir home("home12");
    TempDir proj("proj12");
    proj.write("AGENTS.md",
               std::string(limits::kMaxInstructionChars + 500, 'x'));
    FakeHome fh(home.path.string());
    FakeFetcher fetch;

    const auto items = instruction::load(proj.path.string(), "", fetch);
    ASSERT_EQ(items.size(), (size_t)1);
    ASSERT_TRUE(items[0].body.find("обрезано") != std::string::npos);
    ASSERT_TRUE(items[0].body.size() <=
                limits::kMaxInstructionChars + 256u);
}

TEST(содержимое_url_идёт_отдельным_источником) {
    TempDir home("home13");
    TempDir proj("proj13");
    FakeHome fh(home.path.string());
    FakeFetcher fetch;
    fetch.set_body("ПРАВИЛА ИЗ СЕТИ\n");

    const auto items = instruction::load(
        proj.path.string(), "[\"https://example.com/rules.md\"]", fetch);
    ASSERT_EQ(items.size(), (size_t)1);
    ASSERT_EQ(items[0].label, std::string("https://example.com/rules.md"));
    ASSERT_EQ(items[0].body, std::string("ПРАВИЛА ИЗ СЕТИ\n"));
    ASSERT_EQ(fetch.asked_count(), (size_t)1);
}

/* --- 7. Живой промпт ------------------------------------------------ */

TEST(собранный_промпт_содержит_инструкции_проекта) {
    /* Все проверки выше смотрят на instruction::load. Эта — на то, что
     * инструкции действительно доезжают до модели, то есть на склейку
     * Engine::build_system_prompt. Без неё загрузка была бы функцией,
     * которой никто не пользуется, и все проверки выше — зелёные вхолостую. */
    TempDir home("home14");
    home.write(".config/wp_coder/AGENTS.md", "ГЛОБАЛЬНОЕ ПРАВИЛО\n");
    TempDir proj("proj14");
    proj.write("AGENTS.md", "ПРАВИЛО ПРОЕКТА\n");
    FakeHome fh(home.path.string());

    Engine& eng = engine();
    EngineStateGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().project_dir = proj.path.string();
        eng.state().instructions.clear();
        eng.state().instructions_loaded = false;
        eng.state().scope = RunScope();
    }
    eng.reload_instructions();

    const std::string prompt = eng.build_system_prompt();
    ASSERT_TRUE(prompt.find("Instructions from: AGENTS.md")
                != std::string::npos);
    ASSERT_TRUE(prompt.find("ПРАВИЛО ПРОЕКТА") != std::string::npos);
    ASSERT_TRUE(prompt.find("Instructions from: ~/.config/wp_coder/AGENTS.md")
                != std::string::npos);
    ASSERT_TRUE(prompt.find("ГЛОБАЛЬНОЕ ПРАВИЛО") != std::string::npos);
    /* Формат блока назван в базовом промпте (D2), иначе модель не знает,
     * что это за заголовки. */
    ASSERT_TRUE(std::string(kBaseSystemPrompt).find("## Instructions from: <")
                != std::string::npos);
}

TEST(инструкции_читаются_один_раз_и_не_перечитываются_на_каждой_сборке) {
    /* invalidate_prompt_cache() зовётся на каждом todowrite. Если бы он
     * перечитывал инструкции, каждый план задачи означал бы сетевой
     * запрос к URL из настройки — посреди хода. */
    TempDir home("home15");
    TempDir proj("proj15");
    proj.write("AGENTS.md", "ПРАВИЛО\n");
    FakeHome fh(home.path.string());

    Engine& eng = engine();
    EngineStateGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().project_dir = proj.path.string();
        eng.state().instructions.clear();
        eng.state().instructions_loaded = false;
        eng.state().scope = RunScope();
    }
    eng.reload_instructions();

    const std::string first = eng.build_system_prompt();
    ASSERT_TRUE(first.find("ПРАВИЛО") != std::string::npos);
    /* Смена проекта без reload_instructions: правила прежнего проекта
     * обязаны остаться — иначе смена корня молча убрала бы правила
     * посреди сессии. */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().project_dir = home.path.string();
    }
    eng.invalidate_prompt_cache();
    const std::string second = eng.build_system_prompt();
    ASSERT_TRUE(second.find("ПРАВИЛО") != std::string::npos);
    ASSERT_EQ(eng.instructions_for_test().size(), (size_t)1);
}
TEST(настройка_wp_coder_instructions_доходит_до_промпта) {
    /* Ключ настройки — это контракт с пользователем: он пишет
     * «wp_coder.instructions» в настройках профиля, и если ключ renamed или
     * опечатан, инструкции просто перестанут приходить — МОЛЧА, потому что
     * дефолт настройки пуст и список останется пустым.
     *
     * Поэтому ключ проверяется здесь, через единственную дверь, которой у
     * настроек есть: Engine::init → load_settings. Все проверки выше
     * подставляют значение напрямую в состояние и ключ не видят. */
    TempDir home("home16");
    TempDir proj("proj16");
    proj.write("AGENTS.md", "ПРАВИЛО ПРОЕКТА\n");
    proj.write("extra.md", "ПРАВИЛО ИЗ НАСТРОЙКИ\n");
    FakeHome fh(home.path.string());

    std::map<std::string, std::string> settings;
    settings["wp_coder.project_dir"] = proj.path.string();
    settings["wp_coder.instructions"] = "[\"extra.md\"]";
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&,
                         std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.path_data_dir = []() { return std::string(); };
    cb.path_config_dir = []() { return std::string(); };
    cb.chat_event = [](const std::string&) {};
    cb.settings_get = [&settings](const std::string& key,
                                  const std::string& def) -> std::string {
        auto it = settings.find(key);
        return it != settings.end() ? it->second : def;
    };
    cb.settings_set = [&settings](const std::string& key,
                                  const std::string& value) {
        settings[key] = value;
    };

    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(cb);
    eng.reload_instructions();

    /* Значение дошло до состояния — иначе дальше и проверять нечего. */
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().instructions_config,
                  std::string("[\"extra.md\"]"));
    }
    const std::string prompt = eng.build_system_prompt();
    ASSERT_TRUE(prompt.find("ПРАВИЛО ПРОЕКТА") != std::string::npos);
    ASSERT_TRUE(prompt.find("ПРАВИЛО ИЗ НАСТРОЙКИ") != std::string::npos);
    ASSERT_TRUE(prompt.find("Instructions from: extra.md")
                != std::string::npos);

    /* Возврат состояния: проект и инструкции следующей проверки не должны
     * унаследовать этот каталог (Engine — синглтон, правило 5). */
    settings.erase("wp_coder.project_dir");
    eng.load_settings();
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().project_dir, std::string(""));
    }
}

/* --- 8. И9.2: proximity-attach --------------------------------------- */

TEST(инструкции_рядом_с_файлом_находятся_ближайшей_первой) {
    /* Порядок — от каталога файла вверх: правила каталога относятся к
     * коду рядом и в промпте читаются раньше общих. Наоборящий порядок
     * не опасен текстом, но опасен при конфликте: общее правило перекрыло бы
     * частное, а частное и есть более точное. */
    TempDir home("home17");
    home.write("AGENTS.md", "ВНЕ ПРОЕКТА\n");
    TempDir proj("proj17");
    proj.write("AGENTS.md", "ОБЩИЕ\n");
    proj.write("wp-content/AGENTS.md", "МОДУЛЬНЫЕ\n");
    proj.write("wp-content/themes/style.css", "body{}\n");
    FakeHome fh(home.path.string());

    const auto items = instruction::resolve(
        proj.at("wp-content/themes/style.css"), proj.path.string());
    ASSERT_EQ(items.size(), (size_t)2);
    ASSERT_EQ(items[0].label, std::string("wp-content/AGENTS.md"));
    ASSERT_EQ(items[0].body, std::string("МОДУЛЬНЫЕ\n"));
    ASSERT_EQ(items[1].label, std::string("AGENTS.md"));
    ASSERT_EQ(items[1].body, std::string("ОБЩИЕ\n"));
}

TEST(подъём_останавливается_на_корне_проекта) {
    /* AGENTS.md ВЫШЕ корня проекта — не наши правила. Без остановки они
     * попали бы в промпт каждого проекта, открытого из этого каталога.
     *
     * ЧУЖИЙ AGENTS.md лежит именно В ПРЕДКЕ проекта, а не рядом с ним:
     * первая версия проверки положила его в соседний каталог /tmp, и подъём
     * его просто не встречал — проверка проходила на отсутствии файла, то
     * *желаемое* поведение не проверялось ничем (поймала только мутация
     * подъёма). Каталог проекта вложен в каталог с чужими правилами. */
    TempDir outer("outer18");
    outer.write("AGENTS.md", "ВЫШЕ ПРОЕКТА\n");
    outer.write("proj/code/a.css", "body{}\n");
    TempDir home("home18");
    FakeHome fh(home.path.string());

    const auto items =
        instruction::resolve(outer.at("proj/code/a.css"),
                             outer.at("proj"));
    ASSERT_EQ(items.size(), (size_t)0);
}

TEST(файл_вне_проекта_не_приносит_чужих_правил) {
    /* Файл, прочитанный вне проекта (а это разрешено с разрешения
     * пользователя), не должен приносить правила чужого дерева: подъём
     * вверх оттуда собрал бы AGENTS.md какого-нибудь родительского
     * каталога — и выглядел бы в промпте как требование нашего проекта. */
    TempDir home("home19");
    home.write(" чужие /AGENTS.md", "ЧУЖИЕ\n");
    TempDir proj("proj19");
    proj.write("code/a.css", "body{}\n");
    TempDir outside("outside19");
    outside.write("rules/AGENTS.md", "ЧУЖИЕ\n");
    outside.write("rules/a.css", "body{}\n");
    FakeHome fh(home.path.string());

    const auto items = instruction::resolve(
        outside.at("rules/a.css"), proj.path.string());
    ASSERT_EQ(items.size(), (size_t)0);
}

TEST(имя_файла_рядом_с_файлом_тоже_имеет_приоритет_имён) {
    TempDir home("home20");
    TempDir proj("proj20");
    proj.write("AGENTS.md", "A\n");
    proj.write("CLAUDE.md", "C\n");
    proj.write("a.css", "body{}\n");
    FakeHome fh(home.path.string());

    const auto items =
        instruction::resolve(proj.at("a.css"), proj.path.string());
    ASSERT_EQ(items.size(), (size_t)2);
    ASSERT_EQ(items[0].label, std::string("AGENTS.md"));
    ASSERT_EQ(items[1].label, std::string("CLAUDE.md"));
}

TEST(прикрепление_не_дублирует_уже_загруженное) {
    /* Корень проекта уже загружен в 9.1, и повторное прикрепление того же
     * AGENTS.md не должно ни появиться в промпте второй раз, ни сбросить
     * его кэш. Оба правила — про дедупликацию, а не про красоту. */
    TempDir home("home21");
    TempDir proj("proj21");
    proj.write("AGENTS.md", "ОБЩИЕ\n");
    proj.write("code/a.css", "body{}\n");
    FakeHome fh(home.path.string());

    Engine& eng = engine();
    EngineStateGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().project_dir = proj.path.string();
        eng.state().instructions.clear();
        eng.state().instructions_loaded = false;
        eng.state().scope = RunScope();
    }
    eng.reload_instructions();
    const std::string first = eng.build_system_prompt();
    ASSERT_TRUE(first.find("ОБЩИЕ") != std::string::npos);

    const auto nearby =
        instruction::resolve(proj.at("code/a.css"), proj.path.string());
    ASSERT_EQ(nearby.size(), (size_t)1);
    std::vector<Instruction> again = nearby;
    eng.attach_instructions(nearby);
    /* Ничего не добавилось — состояние не изменилось вовсе. */
    ASSERT_EQ(eng.instructions_for_test().size(), (size_t)1);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        ASSERT_EQ(eng.state().prompt_dirty, false);
    }
    /* И append_new на чистой копии ведёт себя так же — это тот же ключ. */
    std::vector<Instruction> target;
    ASSERT_EQ(instruction::append_new(target, std::move(again)), (size_t)1);
    ASSERT_EQ(instruction::append_new(target, std::move(again)), (size_t)0);
    ASSERT_EQ(target.size(), (size_t)1);
}

TEST(read_прикрепляет_инструкции_каталога_в_следующий_промпт) {
    /* Склейка целиком: прочитанный файл → прикреплённые правила каталога →
     * они видны в СЛЕДУЮЩем собранном промпте. Без этого проверки выше
     * смотрели бы на функцию, которой никто не пользуется. */
    TempDir home("home22");
    home.write(".config/wp_coder/AGENTS.md", "ГЛОБАЛЬНОЕ\n");
    TempDir proj("proj22");
    proj.write("AGENTS.md", "ОБЩИЕ\n");
    proj.write("wp-content/themes/AGENTS.md", "ПРАВИЛА ТЕМЫ\n");
    proj.write("wp-content/themes/style.css", "body{}\n");
    FakeHome fh(home.path.string());

    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&,
                     LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&,
                         std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.path_data_dir = []() { return std::string(); };
    cb.path_config_dir = []() { return std::string(); };
    cb.chat_event = [](const std::string&) {};

    Engine& eng = engine();
    EngineStateGuard guard(eng);
    eng.init(cb);
    test_support::approve_all_permissions();
    register_base_tools();
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().project_dir = proj.path.string();
        eng.state().allowed_external_paths.clear();
    }
    eng.reload_instructions();

    /* До чтения файла правил темы в промпте нет — иначе проверка «появились
     * после чтения» прошла бы на пустом месте. */
    const std::string before = eng.build_system_prompt();
    ASSERT_TRUE(before.find("ОБЩИЕ") != std::string::npos);
    ASSERT_TRUE(before.find("ПРАВИЛА ТЕМЫ") == std::string::npos);

    json::JsonValue args = json::JsonValue::object();
    args.set("path", "wp-content/themes/style.css");
    const ToolOutput out =
        ToolsRegistry::instance().run_output("read_file", args);
    ASSERT_TRUE(out.output.find("body{}") != std::string::npos);
    /* И metadata.loaded (И4.7) называет те же файлы, что прикреплены. */
    ASSERT_TRUE(out.metadata.has("loaded"));
    bool listed = false;
    for (size_t i = 0; i < out.metadata.get("loaded").size(); ++i) {
        if (out.metadata.get("loaded").at(i).get_string("path") ==
            "wp-content/themes/AGENTS.md") listed = true;
    }
    ASSERT_TRUE(listed);

    const std::string after = eng.build_system_prompt();
    ASSERT_TRUE(after.find("ПРАВИЛА ТЕМЫ") != std::string::npos);
    ASSERT_TRUE(after.find("Instructions from: wp-content/themes/AGENTS.md")
                != std::string::npos);
    /* Общие правила не задвоились: один источник — один блок. */
    ASSERT_EQ(count_label(eng.instructions_for_test(), "AGENTS.md"), (size_t)1);
}

/* --- 9. И9.3: порядок блоков системного промпта ---------------------- */

TEST(порядок_блоков_системного_промпта_соответствует_плану) {
    /* Порядок из плана (9.3): базовый промпт → env-блок → AGENTS.md →
     * инструкции модулей → каталог навыков → переопределение запроса.
     *
     * Проверяется СОБРАННЫЙ промпт, а не порядок вызовов в коде: перестановка
     * двух += даст тот же код и другой промпт, то есть проверить её иначе
     * нечем. И каждая метка проверяется на ОТСУТСТВИЕ отдельно — иначе
     * «не нашлось» и «блока нет» дали бы один и тот же зелёный результат,
     * то есть проверка прошла бы вхолостую ровно на тех данных, где блока
     * действительно нет. */
    TempDir home("home23");
    home.write(".config/wp_coder/AGENTS.md", "ГЛОБАЛЬНОЕ\n");
    TempDir proj("proj23");
    proj.write("AGENTS.md", "ПРАВИЛА ПРОЕКТА\n");
    FakeHome fh(home.path.string());

    static CoderModule test_mod = {
        "test_module_order", "Test Order", "Модуль для проверки порядка",
        nullptr,
        []() -> std::vector<ToolInfo> { return {}; },
        []() -> std::vector<Skill> { return {}; },
        []() -> const char* { return "PROMPT: модуль за правилами проекта"; },
        nullptr, nullptr, nullptr, nullptr
    };
    ModuleRegistry::instance().register_module(&test_mod);

    Engine& eng = engine();
    EngineStateGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().project_dir = proj.path.string();
        eng.state().active_module = "test_module_order";
        eng.state().mode = 1;               /* Research: переопределение */
        eng.state().instructions.clear();
        eng.state().instructions_loaded = false;
        eng.state().scope = RunScope();
    }
    eng.reload_instructions();

    const std::string prompt = eng.build_system_prompt();

    const size_t base = prompt.find("## РЕЖИМ РАБОТЫ");
    const size_t agents = prompt.find("## Instructions from: AGENTS.md");
    const size_t mod = prompt.find("PROMPT: модуль за правилами проекта");
    /* Метка каталога навыков — СРАЗУ С переводом строки. Без него искалось
     * подстрока «## НАВЫКИ», а она совпадала ещё и с заголовком раздела о
     * формате ответа skill_detail в базовом промпте, и проверка порядка
     * сравнивала индексы не тех блоков. */
    const size_t skills = prompt.find("## НАВЫКИ\n");
    const size_t mode = prompt.find("[РЕЖИМ: Research]");

    /* Ни одна метка не должна отсутствовать: иначе сравнение индексов
     * ниже шло бы по npos и оказывалось бы верным при любом порядке. */
    ASSERT_TRUE(base != std::string::npos);
    ASSERT_TRUE(agents != std::string::npos);
    ASSERT_TRUE(mod != std::string::npos);
    ASSERT_TRUE(skills != std::string::npos);
    ASSERT_TRUE(mode != std::string::npos);

    ASSERT_TRUE(base < agents);
    ASSERT_TRUE(agents < mod);
    ASSERT_TRUE(mod < skills);
    /* Переопределение запроса — последнее, чем читается промпт. */
    ASSERT_TRUE(skills < mode);
}

/* --- 10. И9.4: env-блок ---------------------------------------------- */

TEST(env_блок_называет_каталог_и_признак_git_репозитория) {
    TempDir home("home24");
    TempDir proj("proj24");
    proj.write("AGENTS.md", "ПРАВИЛО\n");
    FakeHome fh(home.path.string());

    Engine& eng = engine();
    EngineStateGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().project_dir = proj.path.string();
        eng.state().instructions.clear();
        eng.state().instructions_loaded = false;
        eng.state().scope = RunScope();
    }
    eng.reload_instructions();
    const std::string prompt = eng.build_system_prompt();

    ASSERT_TRUE(prompt.find("## ОКРУЖЕНИЕ") != std::string::npos);
    ASSERT_TRUE(prompt.find("Working directory: " + proj.path.string())
                != std::string::npos);
    ASSERT_TRUE(prompt.find("Workspace root folder: " + proj.path.string())
                != std::string::npos);
    /* Каталога .git нет — и «нет» должно быть сказано прямо, а не
     * отсутствовать: отсутствие строки читалось бы как «не проверяли». */
    ASSERT_TRUE(prompt.find("Is directory a git repo: нет")
                != std::string::npos);
    ASSERT_TRUE(prompt.find("Platform: ") != std::string::npos);
    ASSERT_TRUE(prompt.find("Date: ") != std::string::npos);
}

TEST(env_блок_узнаёт_git_репозиторий_и_файловый_dot_git) {
    /* Файл .git — это worktree и submodule. Проверка «каталог .git
     * существует» сказала бы про них «нет», а это как раз тот случай,
     * в котором человек работает над фича-веткой. */
    TempDir home("home25");
    TempDir plain("proj25");
    plain.write("AGENTS.md", "ПРАВИЛО\n");
    FakeHome fh(home.path.string());

    Engine& eng = engine();
    EngineStateGuard guard(eng);

    struct Case { const char* tag; const char* body; bool want; };
    const Case cases[] = {
        {"dir", "", true},
        {"file", "gitdir: /tmp/где-то/.git/worktrees/wt\n", true},
        {"нет", nullptr, false},
    };
    for (const Case& c : cases) {
        TempDir proj(c.tag);
        if (c.body) proj.write(".git", c.body);
        {
            std::lock_guard<std::mutex> lk(eng.state().mtx);
            eng.state().project_dir = proj.path.string();
            eng.state().instructions.clear();
            eng.state().instructions_loaded = false;
            eng.state().scope = RunScope();
        }
        eng.reload_instructions();
        const std::string prompt = eng.build_system_prompt();
        const std::string want = std::string("Is directory a git repo: ") +
                                 (c.want ? "да" : "нет");
        if (prompt.find(want) == std::string::npos) {
            std::cerr << "  .git как " << c.tag << ": не найдено «" << want
                      << "»" << std::endl;
        }
        ASSERT_TRUE(prompt.find(want) != std::string::npos);
    }
}

TEST(env_блок_говорит_что_каталог_не_задан_а_не_молчит) {
    /* Без project_dir агент не знает, где код. Молчание выглядело бы как
     * «где-то», а это ровно то состояние, которое И0.1 уже назвал дефектом. */
    TempDir home("home26");
    FakeHome fh(home.path.string());

    Engine& eng = engine();
    EngineStateGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().project_dir.clear();
        eng.state().instructions.clear();
        eng.state().instructions_loaded = false;
        eng.state().scope = RunScope();
    }
    eng.reload_instructions();
    const std::string prompt = eng.build_system_prompt();
    ASSERT_TRUE(prompt.find("Working directory: не задан")
                != std::string::npos);
    ASSERT_TRUE(prompt.find("Is directory a git repo: неизвестно")
                != std::string::npos);
}

TEST(env_блок_идёт_после_базового_промпта_и_до_правил_проекта) {
    /* Порядок из 9.3: базовый → env → AGENTS.md. Проверяется здесь, а не
     * только в общей проверке порядка, потому что env-блок появился после
     * неё и обязан встать на названное место, а не в любое свободное. */
    TempDir home("home27");
    home.write(".config/wp_coder/AGENTS.md", "ГЛОБАЛЬНОЕ\n");
    TempDir proj("proj27");
    proj.write("AGENTS.md", "ПРАВИЛО\n");
    FakeHome fh(home.path.string());

    Engine& eng = engine();
    EngineStateGuard guard(eng);
    {
        std::lock_guard<std::mutex> lk(eng.state().mtx);
        eng.state().project_dir = proj.path.string();
        eng.state().instructions.clear();
        eng.state().instructions_loaded = false;
        eng.state().scope = RunScope();
    }
    eng.reload_instructions();
    const std::string prompt = eng.build_system_prompt();
    const size_t base = prompt.find("## РЕЖИМ РАБОТЫ");
    const size_t env = prompt.find("## ОКРУЖЕНИЕ");
    const size_t agents = prompt.find("## Instructions from: AGENTS.md");
    ASSERT_TRUE(base != std::string::npos);
    ASSERT_TRUE(env != std::string::npos);
    ASSERT_TRUE(agents != std::string::npos);
    ASSERT_TRUE(base < env);
    ASSERT_TRUE(env < agents);
}

/* --- 11. И9.5: skill_content и skill_files --------------------------- */

TEST(тело_навыка_приходит_в_обёртке_skill_content) {
    /* Обёртка нужна модели, чтобы отличить текст навыка от текста
     * инструмента, и она обязана быть НАЗВАНА в базовом промпте: строка
     * формата, о которой модель не знает, ею и не пользуется (D2). */
    TempDir dir("skills28");
    dir.write("wp_test_skill.md",
              "# Тестовый навык\nописание\nтело навыка\n");
    SkillsManager::instance().load_from_directory(dir.path.string());
    register_base_tools();

    const ToolOutput out = ToolsRegistry::instance().run_output(
        "skill_detail", skill_args("wp_test_skill"));
    ASSERT_TRUE(out.output.find("<skill_content name=\"wp_test_skill\">")
                != std::string::npos);
    ASSERT_TRUE(out.output.find("тело навыка") != std::string::npos);
    ASSERT_TRUE(out.output.find("</skill_content>") != std::string::npos);
    ASSERT_TRUE(std::string(kBaseSystemPrompt).find("<skill_content name=")
                != std::string::npos);
    /* Старый заголовок «### НАВЫК:» больше не используется: он не был
     * назван в промпте и потому ничего не различал. */
    ASSERT_TRUE(out.output.find("### НАВЫК:") == std::string::npos);
}

TEST(ресурсы_каталога_навыка_попадают_в_skill_files) {
    /* Каталог навыка — это навык (.md) плюс всё, что рядом: шаблоны,
     * примеры, справочники. Модель должна знать о них ДО того, как потратит
     * шаг на догадку, поэтому они перечисляются явно. */
    TempDir dir("skills29");
    dir.write("wp_packaged_skill.md", "# Навык\nописание\nтело\n");
    dir.write("template.php", "<?php\n");
    dir.write("references/checklist.md", "- пункт\n");
    dir.write("другой_навык.md", "# Другой\nописание\nтело\n");
    SkillsManager::instance().load_from_directory(dir.path.string());
    register_base_tools();

    const ToolOutput out = ToolsRegistry::instance().run_output(
        "skill_detail", skill_args("wp_packaged_skill"));
    ASSERT_TRUE(out.output.find("<skill_files>") != std::string::npos);
    ASSERT_TRUE(out.output.find("template.php") != std::string::npos);
    ASSERT_TRUE(out.output.find("references/") != std::string::npos);
    /* ЧУЖОЙ навык — не ресурс этого навыка: перечисление соседей должно
     * отличать .md от остального, иначе модель решит, что это файлы навыка. */
    ASSERT_TRUE(out.output.find("другой_навык.md") == std::string::npos);
    ASSERT_TRUE(out.output.find("wp_packaged_skill.md") == std::string::npos);
}

TEST(пустой_список_ресурсов_не_печатается) {
    /* Пустой <skill_files> читался бы как «ресурсов нет» там, где проверка
     * их просто не делала. Отсутствие раздела — тоже неоднозначно, поэтому
     * сказано прямо в базовом промпте: нет списка — нет и файлов. */
    TempDir dir("skills30");
    dir.write("wp_bare_skill.md", "# Навык\nописание\nтело\n");
    SkillsManager::instance().load_from_directory(dir.path.string());
    register_base_tools();

    const ToolOutput out = ToolsRegistry::instance().run_output(
        "skill_detail", skill_args("wp_bare_skill"));
    ASSERT_TRUE(out.output.find("<skill_content name=\"wp_bare_skill\">")
                != std::string::npos);
    ASSERT_TRUE(out.output.find("тело") != std::string::npos);
    ASSERT_TRUE(out.output.find("<skill_files>") == std::string::npos);
}

TEST(навык_без_тела_не_притворяется_что_тело_есть) {
    TempDir dir("skills31");
    dir.write("wp_descr_only.md", "# Навык\nтолько описание\n");
    SkillsManager::instance().load_from_directory(dir.path.string());
    register_base_tools();

    const ToolOutput out = ToolsRegistry::instance().run_output(
        "skill_detail", skill_args("wp_descr_only"));
    ASSERT_TRUE(out.output.find("<skill_content name=\"wp_descr_only\">")
                != std::string::npos);
    ASSERT_TRUE(out.output.find("нет подробной инструкции")
                != std::string::npos);
}

/* --- 12. И9.6: двухуровневый каталог навыков -------------------------- */

TEST(имя_навыка_известно_без_вызова_инструмента) {
    /* Краткий уровень: описание skill_detail перечисляет имена. Модель не
     * тратит шаг на list_skills ради того, чтобы узнать, что навык есть, и
     * не вызывает skill_detail с именем, которого она не видела. */
    TempDir dir("skills32");
    dir.write("wp_first_skill.md", "# Первый\nописание\nтело\n");
    dir.write("wp_second_skill.md", "# Второй\nописание\nтело\n");
    SkillsManager::instance().load_from_directory(dir.path.string());
    register_base_tools();

    const ToolDef* d = ToolsRegistry::instance().find("skill_detail");
    ASSERT_TRUE(d != nullptr);
    const std::string description =
        ToolsRegistry::instance().build_tool_catalogue({"skill_detail"});
    ASSERT_TRUE(description.find("wp_first_skill") != std::string::npos);
    ASSERT_TRUE(description.find("wp_second_skill") != std::string::npos);
    /* Подробный уровень — в системном промпте: там имя и описание рядом. */
    ASSERT_TRUE(SkillsManager::instance().build_skills_prompt()
                    .find("wp_first_skill") != std::string::npos);
}

TEST(при_пустом_каталоге_навыков_описание_не_врёт) {
    /* Пустой перечень без слов про «пока пусто» выглядел бы так, будто
     * перечислять нечего, — и модель не попробовала бы list_skills после
     * установки первого навыка прямо в этой сессии.
     *
     * Каталог передаётся явно, а не берётся из SkillsManager: тот —
     * синглтон, копящий навыки между проверками, и «навыков нет» там не
     * наступает никогда. Первая версия проверки полагалась на пустой
     * синглтон и потому проходила по причине, противоположной своей. */
    const std::string empty = skill_detail_description({});
    ASSERT_TRUE(empty.find("Доступные навыки") != std::string::npos);
    ASSERT_TRUE(empty.find("пока пусто") != std::string::npos);
    ASSERT_TRUE(empty.find("QUERY") != std::string::npos);
    /* Имя навыка-заглушки в тексте не возникает: перечень пуст, и в
     * перечислении не должно быть ни одной подставленной строки. */
    ASSERT_TRUE(empty.find("wp_") == std::string::npos);

    std::vector<Skill> two;
    Skill a; a.name = "wp_alpha";
    Skill b; b.name = "wp_beta";
    two.push_back(a);
    two.push_back(b);
    const std::string full = skill_detail_description(two);
    ASSERT_TRUE(full.find("wp_alpha") != std::string::npos);
    ASSERT_TRUE(full.find("wp_beta") != std::string::npos);
    ASSERT_TRUE(full.find("wp_alpha, wp_beta") != std::string::npos);
    /* «Пока пусто» при непустом каталоге — ложь в том же предложении. */
    ASSERT_TRUE(full.find("пока пусто") == std::string::npos);
}

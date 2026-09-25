/*
 * test_base_tools.cpp — И0: регрессии для exec_command.
 *
 * Исторически exec_command был ЗАРЕГИСТРИРОВАН, но не описан ни в одном
 * промпте — модель о нём не знала. И0.3 добавил его в core/prompts.h
 * вместе с правилом «крайняя мера» и гейтом на пути за пределами проекта.
 *
 * Проверяем здесь обе стороны: инструмент виден модели, и при этом
 * команда, трогающая внешний путь, всё равно упирается в разрешение
 * пользователя, а не выполняется молча.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/base_tools.h"
#include "../core/prompts.h"
#include "../core/project.h"

#include <string>

using namespace coder;

static void init_engine_and_base() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ChatMsg>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    register_base_tools();
}

static std::string run_exec(const std::string& cli) {
    ToolArgs a;
    a.cli = cli;
    return ToolsRegistry::instance().run("exec_command", a);
}

static std::string run_exec_json(const json::JsonValue& args) {
    return ToolsRegistry::instance().run("exec_command", args);
}

/* --- И0.3: инструмент описан для модели (без этого модель его не зовёт) ---
 *
 * И1.5: описание инструментов больше не живёт в ручном списке
 * core/prompts.h, а генерируется из JSON-схем параметров. Проверяем
 * оба слоя: правило «крайняя мера» осталось в базовом промпте, а сам
 * инструмент присутствует в генерируемом каталоге вместе с параметром
 * CLI — иначе модель не знает, что ему передавать. */

TEST(exec_command_is_documented_in_prompt) {
    std::string p = kBaseSystemPrompt;
    /* Правило «крайняя мера» — текст промпта, он остался на месте. */
    ASSERT_TRUE(p.find("exec_command — КРАЙНЯЯ мера") != std::string::npos);
    /* Ручного списка инструментов в промпте больше нет — он генерируется. */
    ASSERT_TRUE(p.find("## БАЗОВЫЕ ИНСТРУМЕНТЫ") == std::string::npos);

    init_engine_and_base();
    std::string cat = ToolsRegistry::instance().build_tool_catalogue();
    size_t at = cat.find("\n- exec_command");
    ASSERT_TRUE(at != std::string::npos);
    size_t end = cat.find("\n- ", at + 1);
    std::string block = cat.substr(at + 1, (end == std::string::npos
                                           ? cat.size() : end) - at - 1);
    /* И имя инструмента, и имя параметра, и его назначение. */
    ASSERT_TRUE(block.find("exec_command") != std::string::npos);
    ASSERT_TRUE(block.find("cli") != std::string::npos);
    ASSERT_TRUE(block.find("обязательно") != std::string::npos);
}

/* --- И0.3: гейт на пути вне проекта --- */

TEST(exec_command_empty_rejected) {
    init_engine_and_base();
    /* И1.6: cli — обязательный параметр, и пустой вызов отсекается
     * валидацией схемы, до входа в обработчик. */
    std::string r = run_exec("");
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(r.find("cli") != std::string::npos);
    /* Явно переданный пустой cli проходит схему и отсекается уже
     * самим инструментом — две разные защиты, обе нужны. */
    json::JsonValue args = json::JsonValue::object();
    args.set("cli", "");
    std::string r2 = run_exec_json(args);
    ASSERT_TRUE(r2.find("пустая команда") != std::string::npos);
}

TEST(exec_command_blocklist_still_applies) {
    init_engine_and_base();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    std::string r = run_exec("rm -rf /");
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

/* Команда с путём ЗА пределами проекта → требуется разрешение пользователя.
 * Раньше гейта не было вовсе: exec_command был единственным инструментом,
 * исполняющим произвольный код, и единственным без проверки пути. */
TEST(exec_command_outside_path_requires_permission) {
    init_engine_and_base();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
        engine_state().allowed_external_paths.clear();
    }
    std::string r = run_exec("cat /etc/shadow");
    /* Либо отказ с требованием разрешения, либо (если движок не смог войти
     * в режим ожидания) явный запрет — но НЕ вывод файла. */
    ASSERT_TRUE(r.find("Доступ") != std::string::npos ||
                r.find("разрешен") != std::string::npos);
    ASSERT_TRUE(r.find("root:") == std::string::npos);
}

/* Путь внутри проекта — гейт не должен срабатывать. */
TEST(exec_command_inside_path_not_gated) {
    init_engine_and_base();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
        engine_state().allowed_external_paths.clear();
    }
    /* Команда безобидная и не выходит за пределы проекта: её выполнение
     * допустимо, значит отказа в разрешении быть не должно. */
    std::string r = run_exec("echo hi > /tmp/wp-test/probe.txt");
    ASSERT_TRUE(r.find("Доступ") == std::string::npos);
}

/* Аргумент вида --path=/outside тоже должен ловиться. */
TEST(exec_command_flag_path_outside_gated) {
    init_engine_and_base();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
        engine_state().allowed_external_paths.clear();
    }
    std::string r = run_exec("wp --path=/var/www/html core version");
    ASSERT_TRUE(r.find("Доступ") != std::string::npos ||
                r.find("разрешен") != std::string::npos);
}

/* ======================================================================
 * И0.9 — project_resolve: выход из корня проекта через двоеточие.
 *
 * Проверка была `rel.find(":") == 1`, то есть ЛЮБОЙ путь с двоеточием
 * на второй позиции считался абсолютным и возвращался как есть.
 * project_resolve — единственная реализация разрешения путей для
 * файловых инструментов и модуля Python, поэтому такой обход миновал
 * и is_path_outside, и PermissionGate.
 * ====================================================================== */

static void set_project_root(const std::string& dir) {
    std::lock_guard<std::mutex> lk(engine_state().mtx);
    engine_state().project_dir = dir;
}

TEST(project_resolve_joins_relative) {
    init_engine_and_base();
    set_project_root("/var/www/site");
    ASSERT_EQ(project_resolve("wp-config.php"),
              std::string("/var/www/site/wp-config.php"));
    ASSERT_EQ(project_resolve("wp-content/themes/x/style.css"),
              std::string("/var/www/site/wp-content/themes/x/style.css"));
    /* Короткая форма пути, оканчивающаяся на '/', не даёт двойного слэша. */
    set_project_root("/var/www/site/");
    ASSERT_EQ(project_resolve("a.php"), std::string("/var/www/site/a.php"));
    /* rel == "" -> сам корень. */
    ASSERT_EQ(project_resolve(""), std::string("/var/www/site/"));
}

TEST(project_resolve_keeps_posix_and_windows_absolute) {
    init_engine_and_base();
    set_project_root("/var/www/site");
    ASSERT_EQ(project_resolve("/etc/passwd"), std::string("/etc/passwd"));
    ASSERT_EQ(project_resolve("C:/Users/x"), std::string("C:/Users/x"));
    ASSERT_EQ(project_resolve("C:\\Users\\x"), std::string("C:\\Users\\x"));
    /* Голый диск "C:" тоже абсолютен. */
    ASSERT_EQ(project_resolve("C:"), std::string("C:"));
}

TEST(project_resolve_does_not_treat_colon_as_drive_letter) {
    init_engine_and_base();
    set_project_root("/var/www/site");
    /* Двоеточие на второй позиции, но за ним НЕ разделитель — это не
     * Windows-диск, путь относительный и обязан быть привязан к корню. */
    ASSERT_EQ(project_resolve("a:b/c"),
              std::string("/var/www/site/a:b/c"));
    ASSERT_EQ(project_resolve("x:1"),
              std::string("/var/www/site/x:1"));
    /* Цифра вместо буквы — тоже не диск. */
    ASSERT_EQ(project_resolve("1:2/x"),
              std::string("/var/www/site/1:2/x"));
    /* Двоеточие не на второй позиции. */
    ASSERT_EQ(project_resolve("ab:cd/x"),
              std::string("/var/www/site/ab:cd/x"));
}

TEST(project_resolve_without_project_root_is_passthrough) {
    init_engine_and_base();
    set_project_root("");
    /* Корень не задан — привязать не к чему, путь уходит как есть. */
    ASSERT_EQ(project_resolve("a.php"), std::string("a.php"));
    set_project_root("/var/www/site");
}

/*
 * test_wp_tools.cpp — безопасность и валидация WP-инструментов (Фаза 4.4).
 *
 * Тестируем ветки ВАЛИДАЦИИ (до запуска реальных команд): shell-инъекции,
 * DDL-запрет, невалидные аргументы. Функции, которые выполняют команды
 * (deploy, verify, php_lint, wp_check_deps), здесь не вызываются.
 */

#include "test_framework.h"
#include "test_support.h"
#include "../core/json.h"
#include "../core/tools_registry.h"
#include "../core/engine.h"
#include "../core/agent_registry.h"
#include "../core/base_tools.h"
#include "../core/git_tools.h"
#include "../core/prompts.h"
#include "../core/command_policy.h"
#include "../core/shell.h"
#include "../modules/wordpress/wp_sql.h"
#include "../modules/wordpress/wp_tools.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace coder;

static void init_engine_and_wp() {
    HostCallbacks cb;
    cb.llm_chat = [](const std::string&, const std::vector<ModelMessage>&, LlmReply&) { return false; };
    cb.llm_complete = [](const std::string&, const std::string&, std::string&) { return false; };
    cb.llm_is_connected = []() { return false; };
    cb.chat_event = [](const std::string&) {};
    Engine::instance().init(cb);
    test_support::approve_all_permissions();
    wp::register_wp_tools();
}

static std::string run_tool(const std::string& name, ToolArgs& a) {
    return ToolsRegistry::instance().run(name, a);
}

/* И1.6: обязательный параметр пуст — отказ приходит из валидации схемы,
 * а не из проверки внутри инструмента. Обе защиты нужны, поэтому
 * проверяем обе: вторую — явной передачей пустой строки. */
static std::string run_with_arg(const std::string& name, const char* key) {
    json::JsonValue args = json::JsonValue::object();
    args.set(key, "");
    return ToolsRegistry::instance().run(name, args);
}

/* --- wp_cli: shell-инъекция --- */

TEST(wp_cli_empty_rejected) {
    init_engine_and_wp();
    ToolArgs a;  // cli пуст
    std::string r = run_tool("wp_cli", a);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(run_with_arg("wp_cli", "cli").find("пустая команда") != std::string::npos);
}

TEST(wp_cli_semicolon_injection_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.cli = "plugin list; rm -rf /";
    std::string r = run_tool("wp_cli", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

TEST(wp_cli_subshell_injection_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.cli = "plugin list $(rm -rf /)";
    std::string r = run_tool("wp_cli", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

TEST(wp_cli_backtick_injection_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.cli = "plugin list `id`";
    std::string r = run_tool("wp_cli", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

/* --- wp_db: DDL/ACL запрет --- */

TEST(wp_db_empty_query_rejected) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;  // query пуст
    std::string r = run_tool("wp_db", a);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(run_with_arg("wp_db", "query").find("пустой SQL-запрос") != std::string::npos);
}

TEST(wp_db_drop_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.query = "DROP TABLE wp_options";
    std::string r = run_tool("wp_db", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

TEST(wp_db_grant_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.query = "GRANT ALL ON *.* TO 'hacker'";
    std::string r = run_tool("wp_db", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

TEST(wp_db_select_passes_validation) {
    /* Безопасный SELECT проходит валидацию; дальше команда не выполняется,
     * потому что проект не существует — важен сам факт отсутствия «запрещено». */
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    a.query = "SELECT * FROM wp_options LIMIT 1";
    std::string r = run_tool("wp_db", a);
    ASSERT_TRUE(r.find("запрещено") == std::string::npos);
}

/* --- wp_rest: валидация endpoint --- */

TEST(wp_rest_invalid_endpoint_blocked) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().wp_site_url = "http://localhost";
        engine_state().wp_app_password = "secret";
    }
    ToolArgs a;
    a.query = "posts/;rm -rf /";
    std::string r = run_tool("wp_rest", a);
    ASSERT_TRUE(r.find("запрещено") != std::string::npos);
}

/* --- wp_option --- */

TEST(wp_option_empty_name_rejected) {
    init_engine_and_wp();
    {
        std::lock_guard<std::mutex> lk(engine_state().mtx);
        engine_state().project_dir = "/tmp/wp-test";
    }
    ToolArgs a;
    std::string r = run_tool("wp_option", a);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(run_with_arg("wp_option", "query").find("пустое имя опции") != std::string::npos);
}

/* --- headless_render: валидация URL --- */

TEST(headless_render_empty_url_rejected) {
    init_engine_and_wp();
    ToolArgs a;
    std::string r = run_tool("headless_render", a);
    ASSERT_TRUE(r.find("invalid arguments") != std::string::npos);
    ASSERT_TRUE(run_with_arg("headless_render", "url").find("пустой URL") != std::string::npos);
}

TEST(headless_render_without_scheme_rejected) {
    init_engine_and_wp();
    ToolArgs a;
    a.url = "localhost/wordpress";
    std::string r = run_tool("headless_render", a);
    ASSERT_TRUE(r.find("схему") != std::string::npos);
}

/* ======================================================================
 * И13.3: SQL для wp_create_site — параметрами, а не снятием
 * экранирования (D8)
 *
 * Прежняя строка брала у shell::shell_quote() внешние кавычки — то есть
 * СНИМАЛА экранирование, — и клала остаток внутрь двойных кавычек bash.
 * Это не «битый SQL на пароле с кавычкой» (так D8 был записан), а
 * выполнение команды: пароль `x"; echo INJECTED-RAN; echo "` превращался
 * в ОТДЕЛЬНУЮ команду оболочки. Проверено на живом bash и на разборе
 * команды политикой (см. wp_sql_the_old_string_form_ran_the_password_as_a_command).
 * ====================================================================== */

namespace {

/* Материал проверки — пароль, который старая строка превращала в
 * команду: двойная кавычка закрывает аргумент, «;» начинает новую
 * команду, вторая двойная кавычка возвращает нас в строку, поэтому
 * синтаксис bash остаётся целым и инъекция не выглядит ошибкой. */
const char* kInjectingPassword = "x\"; echo INJECTED-RAN; echo \"";

/* Пароль с кавычкой, обратным слэшем, долларом и точкой с запятой —
 * такие ломали выход, оставаясь внутри SQL. */
const char* kAwkwardPassword = "a'\\ \"$K; id -u0,";

/* Шестнадцатеричная запись СНЯТА С ЖИВОГО MariaDB 11.8: в такой литерал
 * превратился именно этот пароль, и созданный им пользователь пускал
 * вход ровно с этим паролем. Эталон вычислен не здесь — здесь он
 * сверяется с тем, что сервер принял на самом деле. */
const char* kAwkwardPasswordHex = "61275c2022244b3b206964202d75302c";

size_t count_of(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

/* Строка снимает экранирование у shell_quote(), если сразу за её
 * закрывающей скобкой идёт .substr( — именно этим был D8. Проверяется по
 * форме, а не по слову: «shell_quote(» встречается и там, где его
 * результат используют правильно (аргумент wp config create), и ловить
 * само упоминание означало бы запретить правильное применение.
 *
 * Скобки считаются, а не ищется первая: аргумент shell_quote может быть
 * выражением в скобках. */
bool strips_a_shell_quote(const std::string& line) {
    static const std::string kStrip = ".substr(";
    for (size_t at = line.find("shell_quote("); at != std::string::npos;
         at = line.find("shell_quote(", at + 1)) {
        size_t i = at + std::strlen("shell_quote(");
        int depth = 1;
        for (; i < line.size() && depth > 0; ++i) {
            if (line[i] == '(') ++depth;
            else if (line[i] == ')') --depth;
        }
        if (depth != 0) continue;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (line.compare(i, kStrip.size(), kStrip) == 0) return true;
    }
    return false;
}

std::string source_of(const std::string& relative) {
    const std::filesystem::path root =
        std::filesystem::path(__FILE__).parent_path().parent_path();
    std::ifstream f(root / relative, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* Строки файла, которые не комментарий.
 *
 * Проверки ниже ищут форму В КОДЕ, а описание формы в комментарии (в том
 * числе в шапке wp_sql.h, где дефект описан) не должно считаться её
 * вхождением: иначе описание дефекта запрещало бы само это описание.
 * Строки, начинающиеся с «//» или «*», отбрасываются; строка кода с
 * хвостовым комментарием остаётся проверяемой. */
std::vector<std::string> code_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line = text.substr(
            start, (end == std::string::npos ? text.size() : end) - start);
        const size_t first = line.find_first_not_of(" \t");
        const bool comment =
            first == std::string::npos || line.compare(first, 2, "//") == 0 ||
            line.compare(first, 1, "*") == 0;
        if (!comment) out.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

} // namespace

TEST(wp_sql_passes_values_as_parameters_and_never_as_literals) {
    std::string sql, err;
    ASSERT_TRUE(wp::sql::provision_sql("wp_site", "wp_user", kAwkwardPassword,
                                       sql, err));
    /* Пароль уходит параметром: одна шестнадцатеричная запись и ссылка на
     * неё. Эталон — тот литерал, которым на живой MariaDB 11.8 создали
     * пользователя, под которым вход проходит с этим паролем. */
    ASSERT_TRUE(sql.find(std::string("SET @wp_pass = 0x") +
                         kAwkwardPasswordHex + ";") != std::string::npos);
    /* SQL-литерал пароля не пишется руками: IDENTIFIED BY получает
     * QUOTE(@wp_pass), то есть строит его сервер. */
    ASSERT_TRUE(sql.find("IDENTIFIED BY ', QUOTE(@wp_pass))") !=
                std::string::npos);
    /* Ни одного байта пароля в тексте SQL — иначе проверка выше была бы
     * проверкой одной строки, а не свойства. */
    ASSERT_TRUE(sql.find(kAwkwardPassword) == std::string::npos);
    /* Имя БД — тоже параметр, а не текст между обратными кавычками. */
    ASSERT_TRUE(sql.find("wp_site") == std::string::npos);
    ASSERT_TRUE(sql.find("SET @wp_db = 0x") != std::string::npos);
    ASSERT_TRUE(sql.find("CREATE DATABASE IF NOT EXISTS `', @wp_db, '`')") !=
                std::string::npos);
    /* По одному подготовленному запросу на оператор: PREPARE берёт ровно
     * один оператор, склейка нескольких через «;» — синтаксис (MariaDB
     * 11.8, ошибка 1064). Число проверяется, а не «есть ли». */
    ASSERT_EQ(count_of(sql, "PREPARE wp_stmt FROM @wp_sql;"), 3u);
    ASSERT_EQ(count_of(sql, "EXECUTE wp_stmt;"), 3u);
    ASSERT_EQ(count_of(sql, "DEALLOCATE PREPARE wp_stmt;"), 3u);
    ASSERT_EQ(count_of(sql, "FLUSH PRIVILEGES;"), 1u);

    /* КАКОЙ параметр в КАКОЙ оператор: форма «всё параметрами» этого не
     * ловит, а перепутанные @wp_user/@wp_pass дали бы пользователя с
     * паролем от базы и наоборот. */
    const size_t create_user = sql.find("CREATE USER IF NOT EXISTS");
    const size_t grant = sql.find("GRANT ALL ON");
    ASSERT_TRUE(create_user != std::string::npos);
    ASSERT_TRUE(grant != std::string::npos);
    ASSERT_TRUE(grant > create_user);
    const std::string user_stmt = sql.substr(create_user, grant - create_user);
    const std::string grant_stmt = sql.substr(grant);
    ASSERT_TRUE(user_stmt.find("QUOTE(@wp_user)") != std::string::npos);
    ASSERT_TRUE(user_stmt.find("IDENTIFIED BY ', QUOTE(@wp_pass))") !=
                std::string::npos);
    ASSERT_TRUE(grant_stmt.find("@wp_pass") == std::string::npos);
    /* Имя БД обязано быть в обоих операторах — иначе GRANT ушёл бы не туда. */
    ASSERT_TRUE(user_stmt.find("@wp_db") == std::string::npos);
    ASSERT_TRUE(grant_stmt.find("ON `', @wp_db, '`.* TO ") != std::string::npos);
}

TEST(wp_sql_empty_password_is_a_literal_and_not_a_broken_hex_prefix) {
    /* «0x» без цифр — синтаксическая ошибка, то есть пустой пароль уронил
     * бы шаг создания БД целиком. */
    ASSERT_EQ(wp::sql::hex_literal(""), std::string("''"));
    std::string sql, err;
    ASSERT_TRUE(wp::sql::provision_sql("wp_site", "wp_user", "", sql, err));
    ASSERT_TRUE(sql.find("SET @wp_pass = '';") != std::string::npos);
}

TEST(wp_sql_refuses_a_database_name_it_cannot_quote) {
    /* Имя БД — идентификатор: подставить его в обратные кавычки можно
     * только текстом, поэтому оно проверяется целиком. Материал — то,
     * что обратными кавычками не закрывается. */
    const char* bad[] = {"", "`", "wp`x", "wp x", "wp'x", "wp\\x",
                         "wp;DROP TABLE t", "wp\x01"};
    for (const char* name : bad) {
        std::string sql = "текст не должен собираться";
        std::string err;
        if (wp::sql::provision_sql(name, "wp_user", "p", sql, err)) {
            std::cerr << "  имя БД «" << name << "» принято" << std::endl;
        }
        ASSERT_FALSE(wp::sql::provision_sql(name, "wp_user", "p", sql, err));
        /* Отказ ДО сборки текста: собранный SQL остаётся нетронутым, и
         * вызывающий код не может по ошибке выполнить старый. */
        ASSERT_EQ(sql, std::string("текст не должен собираться"));
        ASSERT_TRUE(!err.empty());
    }
    /* Пустое имя пользователя: CREATE USER ''@'localhost' — это учётная
     * запись, которой нельзя войти, то есть шаг прошёл бы, а сайт потом
     * не поднялся бы. Отказ обязазан быть до БД. */
    {
        std::string sql = "текст не должен собираться";
        std::string err;
        ASSERT_FALSE(wp::sql::provision_sql("wp_site", "", "p", sql, err));
        ASSERT_EQ(sql, std::string("текст не должен собираться"));
        ASSERT_TRUE(!err.empty());
    }

    /* Положительный контроль на той же форме: годные имена проходят,
     * иначе проверка была бы зелёной и на «отказ всегда». */
    for (const char* name : {"wp_site", "wp-site", "wp.site", "SITE2"}) {
        std::string sql, err;
        if (!wp::sql::provision_sql(name, "wp_user", "p", sql, err)) {
            std::cerr << "  годное имя БД «" << name << "» отвергнуто: "
                      << err << std::endl;
        }
        ASSERT_TRUE(wp::sql::provision_sql(name, "wp_user", "p", sql, err));
    }
}

TEST(wp_sql_command_carries_no_sql_and_passes_the_policy) {
    const std::string cmd = wp::sql::run_command("/tmp/wp_coder_sql_4242_0.sql");
    /* В команде только путь: ни пароля, ни самого SQL — иначе значение
     * снова оказалось бы в строке команды, которую читает политика. */
    ASSERT_TRUE(cmd.find(kAwkwardPassword) == std::string::npos);
    ASSERT_TRUE(cmd.find("CREATE") == std::string::npos);
    ASSERT_TRUE(cmd.find("0x") == std::string::npos);
    ASSERT_EQ(cmd, std::string("sudo mariadb < '/tmp/wp_coder_sql_4242_0.sql'"));
    /* Собранную плагином команду политика обязана пропустить: иначе
     * инструмент не работает ни с одним паролем. */
    CommandPolicy p;
    const std::string verdict = p.check_assembled(cmd);
    if (!verdict.empty()) std::cerr << "  отказ политики: " << verdict << std::endl;
    ASSERT_EQ(verdict, std::string(""));
}

TEST(wp_sql_the_script_file_holds_the_password_only_for_the_owner_and_is_removed) {
    /* Здесь пароль БД физически лежит на диске, и это единственное место
     * в задаче, где права файла — не стилистика. */
    std::string sql, err;
    ASSERT_TRUE(wp::sql::provision_sql("wp_site", "wp_user", kAwkwardPassword,
                                       sql, err));
    std::string path;
    {
        wp::sql::TempScript file(sql);
        if (!file.ok()) std::cerr << "  " << file.error() << std::endl;
        ASSERT_TRUE(file.ok());
        path = file.path();
        ASSERT_TRUE(std::filesystem::exists(path));

        /* Содержимое — РОВНО собранный скрипт, дописанного «\0» нет:
         * проверка на «файл непустой» была бы зелёной на мусоре. */
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        ASSERT_EQ(ss.str(), sql);

        /* 0600 и никаких битов для группы и остальных. Сравнение по
         * маске, а не «не 0777»: лишние биты владельца тоже лишние.
         * perms печатать нечем (у него нет operator<<), поэтому права
         * сравниваются числами. */
        using perms = std::filesystem::perms;
        const unsigned mode = static_cast<unsigned>(std::filesystem::status(path).permissions());
        const unsigned others =
            mode & static_cast<unsigned>(perms::group_all | perms::others_all);
        const unsigned owner =
            mode & static_cast<unsigned>(perms::owner_read | perms::owner_write);
        if (others != 0 || owner != 0600u) {
            std::cerr << "  права файла " << std::oct << mode << std::dec
                      << " вместо 0600" << std::endl;
        }
        ASSERT_EQ(others, 0u);
        ASSERT_EQ(owner, 0600u);
    }
    /* Снят на ЛЮБОМ выходе — в том числе когда шаг упал и функция вернула
     * результат пораньше. */
    if (std::filesystem::exists(path)) {
        std::cerr << "  временный файл остался: " << path << std::endl;
    }
    ASSERT_FALSE(std::filesystem::exists(path));
}

TEST(wp_sql_two_scripts_never_share_one_file_name) {
    /* O_EXCL: если бы имя выдавалось счётчиком без проверки, второй файл
     * перезаписал бы первый — и SQL ушёл бы в сервер от прошлого шага. */
    std::string sql, err;
    ASSERT_TRUE(wp::sql::provision_sql("wp_site", "wp_user", "p", sql, err));
    wp::sql::TempScript a(sql);
    wp::sql::TempScript b(sql);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(a.path() != b.path());
    ASSERT_TRUE(std::filesystem::exists(a.path()));
    ASSERT_TRUE(std::filesystem::exists(b.path()));
}

TEST(wp_sql_the_script_file_is_never_written_over_an_existing_name) {
    /* O_EXCL. Имя предсказуемо: pid и счётчик, — а значит, его может
     * занять кто-то ещё (симлинк, оставшийся файл после убитого процесса).
     * Сценарий воспроизводится буквально: следующее имя выводится из
     * наблюдаемого, занимается чужим содержимым, и класс обязан его
     * пропустить. Без O_EXCL файл был бы перезаписан — молча, и на
     * сервер ушёл бы не тот SQL. */
    std::string sql, err;
    ASSERT_TRUE(wp::sql::provision_sql("wp_site", "wp_user", "p", sql, err));

    wp::sql::TempScript first(sql);
    ASSERT_TRUE(first.ok());
    /* Порядковый номер из имени первого файла. Имя устроено как
     * «wp_coder_sql_<pid>_<n>.sql», поэтому номер — цифры ПОСЛЕДНЕЙ
     * черты, а не первой: в первой лежит pid, и «номер + 1» указало бы
     * на имя, которого не существует, — проверка была бы зелёной всегда
     * (это и случилось: мутация O_EXCL → O_TRUNC выжила). */
    static const std::string prefix = "wp_coder_sql_";
    static const std::string suffix = ".sql";
    const std::string base = std::filesystem::path(first.path()).filename().string();
    const size_t at = base.rfind(prefix);
    const size_t dot = base.rfind(suffix);
    const size_t underscore = base.rfind('_');
    if (at == std::string::npos || dot == std::string::npos ||
        underscore == std::string::npos || underscore < at || underscore + 1 >= dot) {
        std::cerr << "  неожиданное имя временного файла: " << base << std::endl;
    }
    ASSERT_TRUE(at != std::string::npos && dot != std::string::npos);
    ASSERT_TRUE(underscore != std::string::npos && underscore + 1 < dot);
    const std::string counter = base.substr(underscore + 1, dot - underscore - 1);
    const std::filesystem::path dir =
        std::filesystem::path(first.path()).parent_path();
    const std::filesystem::path decoy =
        dir / (prefix + base.substr(at + prefix.size(),
                                    underscore - at - prefix.size()) +
               "_" + std::to_string(std::stoul(counter) + 1) + suffix);
    {
        std::ofstream d(decoy, std::ios::binary | std::ios::trunc);
        d << "ЧУЖОЕ СОДЕРЖИМОЕ";
    }
    const std::string decoy_before = decoy.string();

    wp::sql::TempScript second(sql);
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(second.path() != decoy_before);
    std::ifstream d(decoy, std::ios::binary);
    std::stringstream ss;
    ss << d.rdbuf();
    if (ss.str() != "ЧУЖОЕ СОДЕРЖИМОЕ") {
        std::cerr << "  временный файл перезаписан: " << decoy.string()
                  << std::endl;
    }
    ASSERT_EQ(ss.str(), std::string("ЧУЖОЕ СОДЕРЖИМОЕ"));
    std::error_code ec;
    std::filesystem::remove(decoy, ec);
}

TEST(wp_sql_the_old_string_form_ran_the_password_as_a_command) {
    /* Утверждение о ДЕФЕКТЕ, а не о текущем коде: старая сборка
     * воспроизведена здесь целиком, и она обязана оставаться инъекцией.
     * Если разбор перестанет видеть выполненную команду, проверка
     * упадёт — иначе она станет зелёной на «инъекции больше нет»,
     * то есть на пустом месте. */
    const std::string quoted = shell::shell_quote(kInjectingPassword);
    const std::string old_cmd =
        "sudo mariadb -e \"CREATE USER IF NOT EXISTS 'wp_user'@'localhost'"
        " IDENTIFIED BY '" + quoted.substr(1, quoted.size() - 2) + "';\"";
    const std::vector<ParsedCommand> stages = parse_command_line(old_cmd);
    /* Команда стала не одной, а тремя: sudo, echo INJECTED-RAN, echo. */
    ASSERT_EQ(stages.size(), 3u);
    bool ran_payload = false;
    for (const ParsedCommand& c : stages) {
        if (c.program() == "echo" && !c.args.empty() &&
            c.args[0] == "INJECTED-RAN") {
            ran_payload = true;
        }
    }
    if (!ran_payload) {
        std::cerr << "  пароль перестал стать командой: «" << old_cmd
                  << "» разобран как " << stages.size() << " стадии —"
                  << " материал проверки протух" << std::endl;
    }
    ASSERT_TRUE(ran_payload);
    /* И новая форма на том же пароле: команда ровно одна, и пароля в
     * ней нет вообще — поэтому становиться командой ему не из чего. */
    const std::string new_cmd = wp::sql::run_command("/tmp/wp_coder_sql_1_0.sql");
    ASSERT_EQ(parse_command_line(new_cmd).size(), 1u);
    ASSERT_TRUE(new_cmd.find(kInjectingPassword) == std::string::npos);
}

TEST(wp_create_site_builds_its_sql_through_the_parameterized_builder) {
    /* Потребитель, а не объявление (девятый урок граблей SESSION_START):
     * пока зовёт сборщик — D8 закрыт, а если кто-то соберёт строку руками
     * снова, проверка поймает это здесь, а не отказом на живом пароле. */
    const std::string src = source_of("modules/wordpress/wp_tools.cpp");
    ASSERT_TRUE(!src.empty());
    ASSERT_TRUE(src.find("sql::provision_sql(") != std::string::npos);
    ASSERT_TRUE(src.find("sql::run_command(") != std::string::npos);
    /* SQL-операторов в инструменте нет: собирает их wp_sql.h, и там они
     * приходят параметрами. Появление «CREATE DATABASE» здесь означало бы
     * возврат к строке, собранной руками. */
    for (const std::string& line : code_lines(src)) {
        if (line.find("CREATE DATABASE") != std::string::npos ||
            line.find("CREATE USER") != std::string::npos ||
            line.find("IDENTIFIED BY") != std::string::npos) {
            std::cerr << "  SQL собирается руками в wp_tools.cpp:" << line
                      << std::endl;
        }
        ASSERT_TRUE(line.find("CREATE DATABASE") == std::string::npos);
        ASSERT_TRUE(line.find("CREATE USER") == std::string::npos);
        ASSERT_TRUE(line.find("IDENTIFIED BY") == std::string::npos);
    }

    /* По всему дереву модулей — снятие экранирования, то есть исходная
     * форма D8: в соседнем инструменте она выглядела бы невинно и
     * закрыла бы обход одной правкой. */
    const std::filesystem::path root =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "modules";
    std::vector<std::filesystem::path> checked;
    size_t lines_checked = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        checked.push_back(e.path());
        for (const std::string& line : code_lines(ss.str())) {
            ++lines_checked;
            if (strips_a_shell_quote(line)) {
                std::cerr << "  снятие экранирования в " << e.path().string()
                          << ":" << line << std::endl;
                ASSERT_TRUE(false);
            }
        }
    }
    /* Проверка, которая не посмотрела ни на одну строку, ничего не
     * проверяет: пустой список файлов или строк обязан её уронить. */
    if (checked.empty()) {
        std::cerr << "  ни одного файла модулей не проверено" << std::endl;
    }
    ASSERT_TRUE(!checked.empty());
    if (lines_checked < 100) {
        std::cerr << "  проверено всего " << lines_checked
                  << " строк модулей — похоже, что чтение файлов не сработало"
                  << std::endl;
    }
    ASSERT_TRUE(lines_checked >= 100);
}

/* ======================================================================
 * И8.15: промпты WP-субагентов против настоящих инструментов
 * ====================================================================== */

/* Имена из раздела промпта: строки вида «- имя — …» внутри секций
 * ИНСТРУМЕНТЫ и НАВЫКИ. Формат строки — ТОТ ЖЕ, что у каталога
 * инструментов (tools_registry.cpp: describe_tool печатает «- имя — …»):
 * свой формат в промпте означал бы, что модель видит два вида списков и
 * проверка цепляется за один из них.
 *
 * Берутся только строки раздела, а не вхождения подстрок по всему
 * промпту: в тексте есть и пути (style.css), и имена функций WordPress
 * (add_action), и проверка подстрок была бы проверкой строки, а не
 * объявления. */
std::vector<std::string> declared_names(const std::string& prompt,
                                        const std::string& section) {
    std::vector<std::string> out;
    const size_t at = prompt.find("\n" + section + "\n");
    if (at == std::string::npos) return out;
    size_t pos = at + section.size() + 2;
    /* Раздел — до ближайшей пустой строки: так он заканчивается там же,
     * где и читается человеком. */
    while (pos < prompt.size()) {
        const size_t eol = prompt.find("\n", pos);
        const std::string line = prompt.substr(
            pos, (eol == std::string::npos ? prompt.size() : eol) - pos);
        if (line.rfind("- ", 0) != 0) break;
        const size_t dash = line.find(" — ");
        /* Имя — до тире. Строка без тире («- имя») тоже берётся: это
         * всё ещё объявленное имя, просто без пояснения. */
        const std::string name =
            dash == std::string::npos ? line.substr(2) : line.substr(2, dash - 2);
        if (!name.empty()) out.push_back(name);
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

TEST(the_names_a_wp_prompt_declares_all_exist_in_the_registry_and_skills) {
    register_base_tools();
    wp::register_wp_tools();
    register_git_tools();
    AgentRegistry reg;
    register_builtin_agents(reg);

    /* Переименование инструмента или навыка обязано ломать здесь, а не
     * молча оставлять в промпте имя, которого больше нет: модель
     * получила бы инструкцию «вызови то, чего нет» и тратила бы шаг
     * на заведомо неисполнимый вызов (Д2 — дрейф между промптом и
     * реестром). Поэтому проверяется ОБЪЯВЛЕННОЕ: то, что перечислено в
     * секциях ИНСТРУМЕНТЫ/НАВЫКИ, а не вхождения подстрок по всему
     * тексту. */
    const std::vector<std::string> want = {"wp_theme", "wp_plugin", "wp_hook",
                                           "wp_deploy"};
    for (const std::string& name : want) {
        const auto def = reg.find(name);
        ASSERT_TRUE(def != nullptr);
        const std::string& prompt = def->prompt;

        const std::vector<std::string> tools =
            declared_names(prompt, "ИНСТРУМЕНТЫ");
        if (tools.empty()) {
            std::cerr << "  в промпте " << name
                      << " нет объявленных инструментов" << std::endl;
        }
        ASSERT_FALSE(tools.empty());
        for (const std::string& tool : tools) {
            if (ToolsRegistry::instance().find(tool) == nullptr) {
                std::cerr << "  " << name << " объявляет несуществующий "
                          << "инструмент " << tool << std::endl;
            }
            ASSERT_TRUE(ToolsRegistry::instance().find(tool) != nullptr);
        }

        /* Навыки сверяются с тем, что реально отдаёт модуль WordPress:
         * список навыков — это get_wp_skills, и промпт обязан ссылаться
         * на то, что есть. */
        const std::vector<std::string> skills =
            declared_names(prompt, "НАВЫКИ");
        if (skills.empty()) {
            std::cerr << "  в промпте " << name
                      << " нет объявленных навыков" << std::endl;
        }
        ASSERT_FALSE(skills.empty());
        const std::vector<Skill> wp_skills = wp::get_wp_skills();
        for (const std::string& skill : skills) {
            bool found = false;
            for (const Skill& s : wp_skills) {
                if (s.name == skill) found = true;
            }
            if (!found) {
                std::cerr << "  " << name << " ссылается на навык " << skill
                          << ", которого нет в модуле WordPress" << std::endl;
            }
            ASSERT_TRUE(found);
        }
    }
}

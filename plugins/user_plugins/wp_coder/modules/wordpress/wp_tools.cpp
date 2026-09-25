#include "wp_tools.h"
#include "../../core/tool.h"
#include "../../core/tools_registry.h"
#include "../../core/engine.h"
#include "../../core/skills_manager.h"
#include "../../core/shell.h"
#include "../../core/security.h"
#include "../../core/limits.h"
#include "../../core/file_utils.h"
#include "../../core/json_utils.h"

#include <headless_browser/headless_browser.h>

#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <cstdio>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;
namespace coder {
namespace wp {

namespace {

const std::vector<std::string> kSkipDirs = {".git", "node_modules", "vendor",
                                            "wp-includes", "wp-admin",
                                            "__pycache__"};

/* Лимит вывода модуля — единый источник: core/limits.h (4.5). */
using limits::kModuleMaxOutput;

/* Обход PHP-файлов — общая реализация: core/file_utils.h (4.2). */

/* ===== WP-CLI =====
 * Аргументы wp-cli от модели передаются с базовой валидацией:
 * запрещаем shell-метасимволы ; | & ` $ и перенаправления > <. */
std::string wp_cli(const std::string& args) {
    const auto& st = engine_state();
    if (args.empty()) return "[ошибка] пустая команда wp-cli";
    if (st.project_dir.empty()) return "[ошибка] не задан project_dir";

    /* Базовая валидация: запрет shell-инъекций. */
    for (char c : args) {
        if (c == ';' || c == '|' || c == '&' || c == '`' ||
            c == '>' || c == '<' || c == '$') {
            return "[запрещено] shell-инъекция в wp_cli: символ '" +
                   std::string(1, c) + "' не разрешён";
        }
    }

    std::string cmd = "wp --path=" + shell::shell_quote(st.project_dir) + " "
                      + args + " --no-color";
    std::string out = shell::run_capture(cmd, 60);
    return out.empty() ? "[wp-cli: нет вывода]" : shell::cap(out, kModuleMaxOutput);
}

/* ===== wp_db ===== */
std::string wp_db(const std::string& query) {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return "[ошибка] не задан project_dir";
    if (query.empty()) return "[ошибка] пустой SQL-запрос";

    /* Запрет DDL-операций без явного подтверждения:
     * DROP, ALTER, TRUNCATE, GRANT, REVOKE. */
    std::string upper = query;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper.find("DROP ") != std::string::npos ||
        upper.find("TRUNCATE ") != std::string::npos ||
        upper.find("GRANT ") != std::string::npos ||
        upper.find("REVOKE ") != std::string::npos) {
        return "[запрещено] DDL/ACL операция в wp_db. Используй exec_command "
               "с явным wp db query для таких операций.";
    }

    std::string cmd = "wp --path=" + shell::shell_quote(st.project_dir)
                      + " db query " + shell::shell_quote(query) + " --no-color";
    std::string out = shell::run_capture(cmd, 60);
    return out.empty() ? "[wp db query: нет вывода]" : shell::cap(out, kModuleMaxOutput);
}

/* ===== wp_media ===== */
std::string wp_media(int count) {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return "[ошибка] не задан project_dir";
    if (count <= 0) count = 20;
    std::string cmd = "wp --path=" + shell::shell_quote(st.project_dir)
                      + " media list --posts_per_page=" + std::to_string(count)
                      + " --no-color";
    std::string out = shell::run_capture(cmd, 60);
    return out.empty() ? "[wp media list: нет медиафайлов]" : shell::cap(out, kModuleMaxOutput);
}

/* ===== wp_option ===== */
std::string wp_option(const std::string& name) {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return "[ошибка] не задан project_dir";
    if (name.empty()) return "[ошибка] пустое имя опции";
    std::string safe_name = security::sanitize_ident(name);
    if (safe_name.empty()) return "[ошибка] невалидное имя опции";
    std::string cmd = "wp --path=" + shell::shell_quote(st.project_dir)
                      + " option get " + shell::shell_quote(safe_name) + " --no-color";
    std::string out = shell::run_capture(cmd, 60);
    return out.empty() ? "[wp option get: опция не найдена]" : shell::cap(out, kModuleMaxOutput);
}

/* ===== wp_rest ===== */
std::string wp_rest(const std::string& ep) {
    const auto& st = engine_state();
    if (st.wp_site_url.empty() || st.wp_app_password.empty())
        return "[ошибка] не заданы wp_site_url / app_password";
    if (ep.empty()) return "[ошибка] пустой endpoint (QUERY)";

    /* Валидация endpoint: только alnum, _, -, /. */
    for (char c : ep) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '_' && c != '-' && c != '/') {
            return "[запрещено] невалидный REST endpoint";
        }
    }

    std::string url = st.wp_site_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/wp-json/wp/v2/" + ep;
    std::string cmd = "curl -s -m 30 --fail -u " +
                      shell::shell_quote(st.wp_app_user + ":" + st.wp_app_password)
                      + " " + shell::shell_quote(url);
    std::string out = shell::run_capture(cmd, 40);
    return out.empty() ? "[wp_rest: пустой ответ]" : shell::cap(out, kModuleMaxOutput);
}

/* ===== wp_check_deps ===== */
std::string wp_check_deps() {
    std::stringstream s;
    s << "[Проверка зависимостей WordPress]\n\n";
    int ok = 0, fail = 0, warn = 0;

    auto check = [&](const std::string& name, const std::string& cmd, const std::string& hint) {
        std::string out; int rc;
        bool found = shell::run_capture_status(cmd, out, rc, 30) && !out.empty();
        if (found) {
            s << "OK " << name << ": " << out.substr(0, out.find('\n')) << "\n";
            ok++;
        } else {
            s << "FAIL " << name << ": НЕ НАЙДЕН\n   Установка: " << hint << "\n";
            fail++;
        }
    };

    check("PHP CLI", "php -v", "sudo apt install php-cli php-mysql php-xml php-mbstring php-curl php-zip php-gd");
    check("MySQL/MariaDB", "mariadb --version || mysql --version", "sudo apt install mariadb-server");
    check("WP-CLI", "wp --info --allow-root 2>/dev/null | head -1", "curl -O https://raw.githubusercontent.com/wp-cli/builds/gh-pages/phar/wp-cli.phar && sudo mv wp-cli.phar /usr/local/bin/wp");
    check("Git", "git --version", "sudo apt install git");
    check("Apache2", "apache2 -v 2>/dev/null | head -1", "sudo apt install apache2");
    check("curl", "curl --version | head -1", "sudo apt install curl");

    s << "\n[Модули PHP]\n";
    std::string php_out; int php_rc;
    shell::run_capture_status("php -m", php_out, php_rc, 30);
    auto check_mod = [&](const std::string& mod) {
        if (php_out.find(mod) != std::string::npos) { s << "  OK " << mod << "\n"; ok++; }
        else { s << "  FAIL " << mod << " — НУЖЕН (sudo apt install php-" + mod + ")\n"; fail++; }
    };
    check_mod("mysqli"); check_mod("xml"); check_mod("mbstring");
    check_mod("curl"); check_mod("zip"); check_mod("gd");

    s << "\n[Сервисы]\n";
    {
        std::string out; int rc;
        shell::run_capture_status("systemctl is-active mariadb 2>/dev/null || systemctl is-active mysql 2>/dev/null", out, rc, 30);
        bool db_running = (out.find("active") != std::string::npos);
        s << (db_running ? "OK" : "WARN") << " MariaDB/MySQL: " << (db_running ? "запущен" : "не запущен") << "\n";
        if (db_running) ok++; else warn++;
    }
    {
        std::string out; int rc;
        shell::run_capture_status("systemctl is-active apache2 2>/dev/null", out, rc, 30);
        bool web_running = (out.find("active") != std::string::npos);
        s << (web_running ? "OK" : "WARN") << " Apache2: " << (web_running ? "запущен" : "не запущен") << "\n";
        if (web_running) ok++; else warn++;
    }

    s << "\n[Итого] OK " << ok << " | FAIL " << fail << " | WARN " << warn << "\n";
    return s.str();
}

/* ===== wp_create_site ===== */
std::string wp_create_site(const std::string& site_name_in, const std::string& db_name_in,
                            const std::string& db_user_in, const std::string& db_pass_in,
                            const std::string& site_url) {
    std::string site_name = security::sanitize_ident(site_name_in);
    if (site_name.empty()) return "[ошибка] укажи имя сайта (латиницей)";
    std::string db_name = security::sanitize_ident(db_name_in.empty() ? "wp_" + site_name : db_name_in);
    std::string db_user = security::sanitize_ident(db_user_in.empty() ? "wp_" + site_name : db_user_in);
    std::string db_pass = db_pass_in.empty() ? "pass_" + site_name : db_pass_in;
    std::string docroot = "/var/www/" + site_name;

    std::stringstream s;
    s << "[Создание WordPress-сайта: " << site_name << "]\n\n";
    int step = 0;

    auto run_step = [&](const std::string& desc, const std::string& cmd) {
        s << ++step << ". " << desc << "... ";
        std::string out; int rc;
        bool ok = shell::run_capture_status(cmd, out, rc, 120);
        s << (ok ? "OK" : "FAIL: " + text::utf8_prefix(out, 500)) << "\n";
        return ok;
    };

    if (!run_step("Создание директории " + docroot,
                   "sudo mkdir -p " + shell::shell_quote(docroot) +
                   " && sudo chown -R www-data:www-data " + shell::shell_quote(docroot)))
        return s.str();
    {
        /* SQL с экранированными идентификаторами.
         *
         * И3: обратные кавычки экранируются. В двойных кавычках bash
         * считает ` командой — «CREATE DATABASE `wp_x`» выполняло wp_x
         * как программу и подставляла пустой вывод, то есть SQL
         * приходил битым: «CREATE DATABASE IF NOT EXISTS ;». Проверено
         * на bash: echo "a `id -u` b" печатает «a 0 b». Политика команд
         * (И3.6) теперь отказывает такой команде целиком, так что без
         * экранирования wp_create_site просто перестал бы работать — но
         * и до этого не работал.

         *
         * Экранирование пароля (D8) остаётся открытым до И13.3: пароль
         * с кавычкой или обратным слэшем по-прежнему ломает выход, и
         * теперь это видно как отказ политики, а не как молча битый SQL. */
        std::string create_db = "sudo mariadb -e \"CREATE DATABASE IF NOT EXISTS \\`" + db_name
            + "\\`; CREATE USER IF NOT EXISTS '" + db_user + "'@'localhost' IDENTIFIED BY '"
            + shell::shell_quote(db_pass).substr(1, shell::shell_quote(db_pass).size() - 2)
            + "'; GRANT ALL ON \\`" + db_name + "\\`.* TO '" + db_user + "'@'localhost'; FLUSH PRIVILEGES;\"";
        if (!run_step("Создание БД " + db_name, create_db)) return s.str();
    }
    if (!run_step("Скачивание WordPress",
                   "sudo -u www-data wp core download --path=" + shell::shell_quote(docroot) + " --locale=ru_RU --allow-root"))
        return s.str();
    {
        std::string wp_config = "sudo -u www-data wp config create --path=" + shell::shell_quote(docroot)
            + " --dbname=" + db_name + " --dbuser=" + db_user + " --dbpass=" + shell::shell_quote(db_pass)
            + " --allow-root 2>&1";
        if (!run_step("Создание wp-config.php", wp_config)) return s.str();
    }
    {
        std::string url = site_url.empty() ? "http://" + site_name + ".localhost" : site_url;
        std::string install = "sudo -u www-data wp core install --path=" + shell::shell_quote(docroot)
            + " --url=" + shell::shell_quote(url) + " --title=" + shell::shell_quote(site_name)
            + " --admin_user=admin --admin_password=admin --admin_email=admin@" + site_name
            + ".local --skip-email --allow-root 2>&1";
        if (!run_step("Установка WordPress", install)) return s.str();
    }

    auto& st = engine_state();
    st.project_dir = docroot;

    s << "\nГотово! WordPress-сайт создан.\n";
    s << "Директория: " << docroot << "\n";
    s << "Админка: " << (site_url.empty() ? "http://" + site_name + ".localhost" : site_url) + "/wp-admin/\n";
    s << "Логин: admin / Пароль: admin\n";
    s << "БД: " << db_name << "\n";
    return s.str();
}

/* ===== php_lint ===== */
std::string php_lint(const std::string& path) {
    const auto& st = engine_state();
    if (st.php_bin.empty()) return "[ошибка] php-cli не найден";
    std::string cmd = shell::shell_quote(st.php_bin) + " -l " + shell::shell_quote(path);
    std::string out = shell::run_capture(cmd, 60);
    return out.empty() ? "[php -l: нет ошибок]" : shell::cap(out, 4000);
}

/* ===== validate ===== */
std::string validate() {
    const auto& st = engine_state();
    std::vector<std::string> files;
    file_utils::walk_files(st.project_dir, files, 4000, kSkipDirs, ".php");
    if (files.empty()) return "[validate: php-файлы не найдены]";
    const size_t kMaxFiles = 200;
    bool truncated = files.size() > kMaxFiles;
    if (truncated) files.resize(kMaxFiles);
    size_t bad = 0;
    std::stringstream out;
    for (const auto& fp : files) {
        std::string r = php_lint(fp);
        if (r.find("No syntax errors") == std::string::npos
            && r.find("[php -l: нет вывода") == std::string::npos) {
            ++bad;
            out << fp << ":\n" << r << "\n";
        }
    }
    out << "[validate: проверено " << files.size() << " файлов"
        << (truncated ? " (лимит 200)" : "") << ", ошибок: " << bad << "]";
    return out.str();
}

/* ===== deploy ===== */
std::string deploy() {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return "[ошибка] не задан project_dir";
    if (st.deploy_proto == "rsync") {
        if (st.deploy_host.empty() || st.deploy_remote_dir.empty())
            return "[ошибка] не заданы deploy_host / deploy_remote_dir";
        std::string target = st.deploy_user.empty()
            ? shell::shell_quote(st.deploy_host)
            : (shell::shell_quote(st.deploy_user) + "@" + shell::shell_quote(st.deploy_host));
        std::string cmd = "rsync -az --delete --exclude=wp-config.php --exclude=.git --exclude=node_modules "
            + shell::shell_quote(st.project_dir + "/") + " "
            + target + ":" + shell::shell_quote(st.deploy_remote_dir) + "/";
        std::string out = shell::run_capture(cmd, 300);
        return "[deploy rsync] " + (out.empty() ? "успешно" : shell::cap(out, 4000));
    }
    fs::path script = fs::path(st.project_dir) / "deploy.sh";
    if (!fs::exists(script))
        return "[ошибка] proto=" + st.deploy_proto + ", но нет " + script.string();
    std::string out = shell::run_capture(shell::shell_quote(script.string()), 300);
    return "[deploy " + st.deploy_proto + "] " + (out.empty() ? "успешно" : shell::cap(out, 4000));
}

/* ===== verify ===== */
std::string verify() {
    const auto& st = engine_state();
    std::stringstream out;
    out << validate() << "\n";
    if (!st.wp_local_url.empty()) {
        std::string curl = "curl -s -o /dev/null -m 20 -w '%{http_code}' " + shell::shell_quote(st.wp_local_url);
        std::string code = shell::run_capture(curl, 30);
        out << "[HTTP " << (code.empty() ? "?" : code) << "] " << st.wp_local_url << "\n";
    } else {
        out << "[verify: wp_local_url не задан]\n";
    }
    out << "[verify: готово]";
    return out.str();
}

/* ===== headless_render ===== */
std::string headless_render(const std::string& url) {
    if (url.empty()) return "[ошибка] пустой URL";
    /* Валидация URL: базовая проверка. */
    if (url.find("://") == std::string::npos)
        return "[ошибка] URL должен содержать схему (http:// или https://)";

    headless_browser::RenderOptions opts;
    opts.timeout_ms = 30000;
    if (!headless_browser::available(opts)) {
        return "[ошибка] headless браузер (chromium) не найден в PATH; "
               "установи chromium или используй curl/wp_rest для HTTP-проверок.";
    }

    std::string err;
    std::string dom = headless_browser::render_dom(url, opts, &err);
    if (dom.empty()) {
        return "[ошибка] headless render не удался: "
               + (err.empty() ? "неизвестная причина" : err);
    }

    /* Диагностика пустого DOM: HTML есть, но почти нет видимого текста —
     * признак JS-ошибки («белый экран») или SPA-оболочки без рендера. */
    std::string out;
    if (headless_browser::is_thin_content(dom)) {
        out += "[важно] DOM почти пуст (видимых букв: "
             + std::to_string(headless_browser::visible_letter_count(dom))
             + ") — похоже на JS-ошибку или SPA-оболочку без рендера.\n";
    }
    out += "DOM (обрезан до " + std::to_string(kModuleMaxOutput) + " символов):\n";
    out += dom.size() > kModuleMaxOutput ? text::utf8_prefix(dom, kModuleMaxOutput) : dom;
    if (dom.size() > kModuleMaxOutput)
        out += "\n[...обрезано, всего " + std::to_string(dom.size()) + " символов]";
    return out;
}

} // anonymous namespace

/* ===== Регистрация инструментов ===== */

void register_wp_tools() {
    auto& reg = ToolsRegistry::instance();

    /* И1.3: чтение типизированных аргументов. */
    auto arg = [](const json::JsonValue& a, const char* key) {
        return a.get_string(key);
    };

    {
        ToolDef def;
        def.name = "wp_cli";
        def.description = "WP-CLI команда в каталоге проекта";
        /* WP-CLI умеет всё, включая удаление данных: необратимо. */
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE;
        def.permission_key = "wp-cli";
        SchemaBuilder b;
        b.str("cli", "аргументы wp-cli, например «plugin list --format=json»")
         .required("cli");
        def.parameters = b.build();
        def.handler = [arg](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "wp-cli";
            o.output = wp_cli(arg(a, "cli"));
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "wp_db";
        def.description = "SQL-запрос к базе WordPress (DDL-операции запрещены)";
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE;
        def.permission_key = "db";
        SchemaBuilder b;
        b.str("query", "SQL-запрос").required("query");
        def.parameters = b.build();
        def.handler = [arg](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "wp db";
            o.output = wp_db(arg(a, "query"));
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "wp_media";
        def.description = "Список медиафайлов библиотеки WordPress";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.integer_range("k", "сколько файлов показать (по умолчанию 20)", 1, 500);
        def.parameters = b.build();
        def.handler = [](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "wp media";
            o.output = wp_media(static_cast<int>(a.get_int("k", 20)));
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "wp_option";
        def.description = "Чтение опции WordPress (wp option get)";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("query", "имя опции, например blogname").required("query");
        def.parameters = b.build();
        def.handler = [arg](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "wp option";
            o.output = wp_option(arg(a, "query"));
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "wp_rest";
        def.description = "GET-запрос к REST API WordPress";
        /* Чтение через REST: сеть есть, изменения нет. */
        def.flags = TF_READ_ONLY | TF_NETWORK;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("query", "endpoint после /wp-json/wp/v2/, например posts")
         .required("query");
        def.parameters = b.build();
        def.handler = [arg](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "wp rest";
            o.output = wp_rest(arg(a, "query"));
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "wp_check_deps";
        def.description = "Проверка зависимостей WordPress";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        def.parameters = SchemaBuilder().build();
        def.handler = [](const json::JsonValue&, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "wp check-deps";
            o.output = wp_check_deps();
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "wp_create_site";
        def.description = "Создание нового WordPress-сайта (БД, файлы, установка)";
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE | TF_WRITES_FILES | TF_SLOW;
        def.permission_key = "db";
        SchemaBuilder b;
        b.str("query", "имя сайта латиницей")
         .str("pattern", "имя базы данных; пусто = wp_<сайт>")
         .str("content", "имя пользователя БД; пусто = wp_<сайт>")
         .str("cli", "пароль пользователя БД; пусто = pass_<сайт>")
         .str("url", "внешний URL сайта; пусто = http://<сайт>.localhost")
         .required("query");
        def.parameters = b.build();
        def.handler = [arg](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "wp create-site";
            o.output = wp_create_site(arg(a, "query"), arg(a, "pattern"),
                                      arg(a, "content"), arg(a, "cli"),
                                      arg(a, "url"));
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "deploy";
        def.description = "Деплой проекта на удалённый хостер";
        def.flags = TF_EXECUTES | TF_DESTRUCTIVE | TF_NETWORK | TF_SLOW;
        def.permission_key = "deploy";
        def.parameters = SchemaBuilder().build();
        def.handler = [](const json::JsonValue&, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "deploy";
            o.output = deploy();
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "verify";
        def.description = "Комплексная проверка проекта (синтаксис + HTTP)";
        def.flags = TF_READ_ONLY | TF_EXECUTES | TF_SLOW;
        def.permission_key = "read";
        def.parameters = SchemaBuilder().build();
        def.handler = [](const json::JsonValue&, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "verify";
            o.output = verify();
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "php_lint";
        def.description = "Проверка синтаксиса PHP-файла (php -l)";
        def.flags = TF_READ_ONLY | TF_EXECUTES;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("path", "путь к PHP-файлу").required("path");
        def.parameters = b.build();
        def.handler = [arg](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "php -l";
            o.output = php_lint(arg(a, "path"));
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "headless_render";
        def.description = "Рендер DOM страницы headless-браузером";
        def.flags = TF_READ_ONLY | TF_NETWORK | TF_SLOW;
        def.permission_key = "read";
        SchemaBuilder b;
        b.str("url", "адрес страницы").required("url");
        def.parameters = b.build();
        def.handler = [arg](const json::JsonValue& a, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "render";
            o.output = headless_render(arg(a, "url"));
            return o;
        };
        reg.register_def(std::move(def));
    }

    {
        ToolDef def;
        def.name = "validate";
        def.description = "Проверка синтаксиса всех PHP-файлов проекта";
        def.flags = TF_READ_ONLY | TF_EXECUTES | TF_SLOW;
        def.permission_key = "read";
        def.parameters = SchemaBuilder().build();
        def.handler = [](const json::JsonValue&, ToolContext&) -> ToolOutput {
            ToolOutput o;
            o.title = "validate";
            o.output = validate();
            return o;
        };
        reg.register_def(std::move(def));
    }
}

/* ===== WP-навыки ===== */

static const char* kWpThemeSkill =
    "# wp_theme\n"
    "Описание: иерархия шаблонов и безопасная вёрстка темы\n"
    "При правке темы WordPress:\n"
    "- Точка входа — style.css (заголовок темы обязателен) и index.php.\n"
    "- Подключай стили/скрипты только через wp_enqueue_scripts "
    "(wp_enqueue_style / wp_enqueue_script).\n"
    "- Используй цикл: if ( have_posts() ) : while ( have_posts() ) : the_post(); ... endwhile; endif;\n"
    "- Экранируй вывод: esc_html(), esc_attr(), esc_url(); перевод — __('...', 'textdomain').\n"
    "- Используй get_template_part( 'content', 'page' ) для переиспользуемых блоков.\n"
    "- Не правь wp-includes/wp-admin — только wp-content/themes/<theme> и wp-content/plugins.";

static const char* kWpHookSkill =
    "# wp_hook\n"
    "Описание: правильные хуки WordPress (action/filter/shortcode)\n"
    "- add_action( 'init', 'my_init' ): первый аргумент — имя хука, второй — коллбэк, "
    "третий — приоритет (int, по умолчанию 10), четвёртый — число аргументов.\n"
    "- add_filter( 'the_title', 'my_title_filter', 10, 1 ); — фильтр ДОЛЖЕН возвращать return.\n"
    "- Имя коллбэка уникально; префиксуй функции чтобы не конфликтовать.\n"
    "- Для shortcode: add_shortcode( 'mysc', 'my_shortcode_cb' ); "
    "коллбэк принимает $atts, $content, $tag и возвращает строку.\n"
    "- Хуки загрузки темы: after_setup_theme, init, wp_enqueue_scripts.\n"
    "- Никогда не выводи echo внутри фильтра — только return.";

static const char* kWpDatabaseSkill =
    "# wp_database\n"
    "Описание: работа с базой данных WordPress через wp-cli\n"
    "- Используй wp db query для SQL-запросов.\n"
    "- Всегда делай бэкап перед изменением: wp db export backup.sql.\n"
    "- Для поиска данных используй wp db search вместо ручных SQL-запросов.\n"
    "- Проверяй опции через wp option get / wp option update.\n"
    "- Для миграций используй wp db prefix для проверки префикса таблиц.\n"
    "- Используй wp db optimize для оптимизации таблиц после массовых изменений.\n"
    "- Не удаляй таблицы ядра без крайней необходимости.";

static const char* kWpMediaSkill =
    "# wp_media\n"
    "Описание: работа с медиафайлами WordPress\n"
    "- Загрузка: wp media import <file> --title='...' --featured_image.\n"
    "- Список: wp media list --posts_per_page=N.\n"
    "- Размеры: wp media image-size.\n"
    "- Мета: wp media meta get <id>.\n"
    "- Регенерация: wp media regenerate.\n"
    "- Удаление: wp media delete <id>.\n"
    "- Импорт из URL: wp media import <url>.\n"
    "- Для галерей используй shortcode [gallery ids='1,2,3'].";

static const char* kWpPluginBoilerplateSkill =
    "# wp_plugin_boilerplate\n"
    "Описание: каркас корректного плагина WordPress\n"
    "- Заголовок Plugin Name обязателен; добавь Description, Version, Author.\n"
    "- Проверяй ABSPATH: if ( ! defined( 'ABSPATH' ) ) exit;\n"
    "- Не используй префикс wp_ для своих функций/таблиц — это пространство ядра.\n"
    "- Хуки: register_activation_hook / register_deactivation_hook.\n"
    "- Инициализация: add_action( 'plugins_loaded', ... ) или add_action( 'init', ... ).\n"
    "- Настройки: get_option / update_option + register_setting / add_settings_section.\n"
    "- Логирование: error_log() для отладки; не выводи данные на экран в продакшене.\n"
    "- Админка: add_menu_page / add_submenu_page.\n"
    "- Не забывай nonce: wp_verify_nonce / wp_create_nonce.\n"
    "- Текстуризация: esc_html(), esc_attr(), esc_url(), sanitize_text_field().";

static const char* kWpGitSkill =
    "# wp_git\n"
    "Описание: работа с Git в проектах WordPress\n"
    "- Перед коммитом: git status + git diff для проверки изменений.\n"
    "- Осмысленные коммиты на английском с описанием что и почему.\n"
    "- git add <файл> для конкретных файлов, не git add -A без необходимости.\n"
    "- .gitignore: wp-config.php, .env, wp-content/uploads/, node_modules/, vendor/.\n"
    "- Проверяй .gitignore перед первым коммитом.\n"
    "- Ветки: main (продакшн), dev (разработка), feature/*.\n"
    "- Деплой через rsync или Git-хук (post-receive).\n"
    "- Исключать wp-config.php и .env из репозитория — содержат секреты.";

std::vector<Skill> get_wp_skills() {
    return {
        {"wp_theme", "Иерархия шаблонов и безопасная вёрстка темы", kWpThemeSkill, "wordpress"},
        {"wp_hook", "Правильные хуки WordPress (action/filter/shortcode)", kWpHookSkill, "wordpress"},
        {"wp_database", "Работа с базой данных WordPress через wp-cli", kWpDatabaseSkill, "wordpress"},
        {"wp_media", "Работа с медиафайлами WordPress", kWpMediaSkill, "wordpress"},
        {"wp_plugin_boilerplate", "Каркас корректного плагина WordPress", kWpPluginBoilerplateSkill, "wordpress"},
        {"wp_git", "Работа с Git в проектах WordPress", kWpGitSkill, "wordpress"}
    };
}

} // namespace wp
} // namespace coder

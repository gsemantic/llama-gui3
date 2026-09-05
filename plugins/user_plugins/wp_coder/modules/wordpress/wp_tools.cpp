#include "wp_tools.h"
#include "../../core/tools_registry.h"
#include "../../core/engine.h"
#include "../../core/skills_manager.h"

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

std::string run_capture(const std::string& cmd) {
    std::string full = cmd + " 2>&1";
    FILE* f = popen(full.c_str(), "r");
    if (!f) return "[ошибка] не удалось запустить: " + cmd;
    char buf[4096];
    std::string out;
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    return out;
}

bool run_capture_status(const std::string& cmd, std::string& out, int& exit_code) {
    std::string full = cmd + " 2>&1";
    FILE* f = popen(full.c_str(), "r");
    if (!f) { out = "[ошибка] не удалось запустить: " + cmd; exit_code = -1; return false; }
    char buf[4096];
    out.clear();
    while (fgets(buf, sizeof(buf), f)) out += buf;
    exit_code = pclose(f);
    return exit_code == 0;
}

const std::vector<std::string> kSkipDirs = {".git", "node_modules", "vendor",
                                            "wp-includes", "wp-admin"};

void walk_php(const fs::path& root, std::vector<std::string>& out, size_t limit = 4000) {
    if (!fs::exists(root)) return;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& p = it->path();
        if (it->is_directory()) {
            std::string name = p.filename().string();
            if (std::find(kSkipDirs.begin(), kSkipDirs.end(), name) != kSkipDirs.end()) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (it->is_regular_file() && p.extension() == ".php") {
            out.push_back(p.string());
            if (out.size() >= limit) return;
        }
    }
}

/* ===== WP-CLI ===== */
std::string wp_cli(const std::string& args) {
    const auto& st = engine_state();
    if (args.empty()) return "[ошибка] пустая команда wp-cli";
    if (st.project_dir.empty()) return "[ошибка] не задан project_dir";
    std::string cmd = "wp --path=\"" + st.project_dir + "\" " + args + " --no-color 2>&1";
    std::string out = run_capture(cmd);
    if (out.size() > 8000) { out.resize(8000); out += "\n...[обрезано]"; }
    return out.empty() ? "[wp-cli: нет вывода]" : out;
}

/* ===== wp_db ===== */
std::string wp_db(const std::string& query) {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return "[ошибка] не задан project_dir";
    if (query.empty()) return "[ошибка] пустой SQL-запрос";
    std::string cmd = "wp --path=\"" + st.project_dir + "\" db query \""
                      + query + "\" --no-color 2>&1";
    std::string out = run_capture(cmd);
    if (out.size() > 8000) { out.resize(8000); out += "\n...[обрезано]"; }
    return out.empty() ? "[wp db query: нет вывода]" : out;
}

/* ===== wp_media ===== */
std::string wp_media(int count) {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return "[ошибка] не задан project_dir";
    if (count <= 0) count = 20;
    std::string cmd = "wp --path=\"" + st.project_dir + "\" media list --posts_per_page="
                      + std::to_string(count) + " --no-color 2>&1";
    std::string out = run_capture(cmd);
    if (out.size() > 8000) { out.resize(8000); out += "\n...[обрезано]"; }
    return out.empty() ? "[wp media list: нет медиафайлов]" : out;
}

/* ===== wp_option ===== */
std::string wp_option(const std::string& name) {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return "[ошибка] не задан project_dir";
    if (name.empty()) return "[ошибка] пустое имя опции";
    std::string cmd = "wp --path=\"" + st.project_dir + "\" option get \""
                      + name + "\" --no-color 2>&1";
    std::string out = run_capture(cmd);
    if (out.size() > 8000) { out.resize(8000); out += "\n...[обрезано]"; }
    return out.empty() ? "[wp option get: опция не найдена]" : out;
}

/* ===== wp_rest ===== */
std::string wp_rest(const std::string& ep) {
    const auto& st = engine_state();
    if (st.wp_site_url.empty() || st.wp_app_password.empty())
        return "[ошибка] не заданы wp_site_url / app_password";
    std::string url = st.wp_site_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/wp-json/wp/v2/" + ep;
    std::string cmd = "curl -s -m 30 --fail -u \""
        + st.wp_app_user + ":" + st.wp_app_password + "\" \"" + url + "\"";
    std::string out = run_capture(cmd);
    if (out.size() > 8000) { out.resize(8000); out += "\n...[обрезано]"; }
    return out.empty() ? "[wp_rest: пустой ответ]" : out;
}

/* ===== wp_check_deps ===== */
std::string wp_check_deps() {
    std::stringstream s;
    s << "[Проверка зависимостей WordPress]\n\n";
    int ok = 0, fail = 0, warn = 0;

    auto check = [&](const std::string& name, const std::string& cmd, const std::string& hint) {
        std::string out; int rc;
        bool found = run_capture_status(cmd, out, rc) && !out.empty();
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
    run_capture_status("php -m", php_out, php_rc);
    auto check_mod = [&](const std::string& mod) {
        if (php_out.find(mod) != std::string::npos) { s << "  OK " << mod << "\n"; ok++; }
        else { s << "  FAIL " << mod << " — НУЖЕН (sudo apt install php-" + mod + ")\n"; fail++; }
    };
    check_mod("mysqli"); check_mod("xml"); check_mod("mbstring");
    check_mod("curl"); check_mod("zip"); check_mod("gd");

    s << "\n[Сервисы]\n";
    {
        std::string out; int rc;
        run_capture_status("systemctl is-active mariadb 2>/dev/null || systemctl is-active mysql 2>/dev/null", out, rc);
        bool db_running = (out.find("active") != std::string::npos);
        s << (db_running ? "OK" : "WARN") << " MariaDB/MySQL: " << (db_running ? "запущен" : "не запущен") << "\n";
        if (db_running) ok++; else warn++;
    }
    {
        std::string out; int rc;
        run_capture_status("systemctl is-active apache2 2>/dev/null", out, rc);
        bool web_running = (out.find("active") != std::string::npos);
        s << (web_running ? "OK" : "WARN") << " Apache2: " << (web_running ? "запущен" : "не запущен") << "\n";
        if (web_running) ok++; else warn++;
    }

    s << "\n[Итого] OK " << ok << " | FAIL " << fail << " | WARN " << warn << "\n";
    return s.str();
}

/* ===== wp_create_site ===== */
std::string wp_create_site(const std::string& site_name, const std::string& db_name_in,
                            const std::string& db_user_in, const std::string& db_pass_in,
                            const std::string& site_url) {
    if (site_name.empty()) return "[ошибка] укажи имя сайта";
    std::string db_name = db_name_in.empty() ? "wp_" + site_name : db_name_in;
    std::string db_user = db_user_in.empty() ? "wp_" + site_name : db_user_in;
    std::string db_pass = db_pass_in.empty() ? "pass_" + site_name : db_pass_in;
    std::string docroot = "/var/www/" + site_name;

    std::stringstream s;
    s << "[Создание WordPress-сайта: " << site_name << "]\n\n";
    int step = 0;

    auto run_step = [&](const std::string& desc, const std::string& cmd) {
        s << ++step << ". " << desc << "... ";
        std::string out; int rc;
        bool ok = run_capture_status(cmd, out, rc);
        s << (ok ? "OK" : "FAIL: " + out.substr(0, 500)) << "\n";
        return ok;
    };

    if (!run_step("Создание директории " + docroot,
                   "sudo mkdir -p " + docroot + " && sudo chown -R www-data:www-data " + docroot))
        return s.str();
    {
        std::string create_db = "sudo mariadb -e \"CREATE DATABASE IF NOT EXISTS `" + db_name
            + "`; CREATE USER IF NOT EXISTS '" + db_user + "'@'localhost' IDENTIFIED BY '" + db_pass
            + "'; GRANT ALL ON `" + db_name + "`.* TO '" + db_user + "'@'localhost'; FLUSH PRIVILEGES;\"";
        if (!run_step("Создание БД " + db_name, create_db)) return s.str();
    }
    if (!run_step("Скачивание WordPress",
                   "sudo -u www-data wp core download --path=" + docroot + " --locale=ru_RU --allow-root"))
        return s.str();
    {
        std::string wp_config = "sudo -u www-data wp config create --path=" + docroot
            + " --dbname=" + db_name + " --dbuser=" + db_user + " --dbpass=" + db_pass
            + " --allow-root 2>&1";
        if (!run_step("Создание wp-config.php", wp_config)) return s.str();
    }
    {
        std::string url = site_url.empty() ? "http://" + site_name + ".localhost" : site_url;
        std::string install = "sudo -u www-data wp core install --path=" + docroot
            + " --url=" + url + " --title=\"" + site_name
            + "\" --admin_user=admin --admin_password=admin --admin_email=admin@" + site_name
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
    std::string cmd = st.php_bin + " -l \"" + path + "\"";
    std::string out = run_capture(cmd);
    return out.empty() ? "[php -l: нет ошибок]" : out;
}

/* ===== validate ===== */
std::string validate() {
    const auto& st = engine_state();
    std::vector<std::string> files;
    walk_php(st.project_dir, files);
    if (files.empty()) return "[validate: php-файлы не найдены]";
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
    out << "[validate: проверено " << files.size() << " файлов, ошибок: " << bad << "]";
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
            ? st.deploy_host
            : (st.deploy_user + "@" + st.deploy_host);
        std::string cmd = "rsync -az --delete --exclude=wp-config.php --exclude=.git --exclude=node_modules "
            "\"" + st.project_dir + "/\" \"" + target + ":" + st.deploy_remote_dir + "/\"";
        std::string out = run_capture(cmd);
        return "[deploy rsync] " + (out.empty() ? "успешно" : out);
    }
    fs::path script = fs::path(st.project_dir) / "deploy.sh";
    if (!fs::exists(script))
        return "[ошибка] proto=" + st.deploy_proto + ", но нет " + script.string();
    std::string out = run_capture("\"" + script.string() + "\"");
    return "[deploy " + st.deploy_proto + "] " + (out.empty() ? "успешно" : out);
}

/* ===== verify ===== */
std::string verify() {
    const auto& st = engine_state();
    std::stringstream out;
    out << validate() << "\n";
    if (!st.wp_local_url.empty()) {
        std::string curl = "curl -s -o /dev/null -m 20 -w '%{http_code}' \"" + st.wp_local_url + "\"";
        std::string code = run_capture(curl);
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
    /* Попытка использовать headless_browser если доступен. */
    return "[headless_render] " + url + " (headless browser integration)";
}

} // anonymous namespace

/* ===== Регистрация инструментов ===== */

void register_wp_tools() {
    auto& reg = ToolsRegistry::instance();

    reg.register_tool("wp_cli", [](const ToolArgs& a) -> std::string {
        return wp_cli(a.cli);
    }, "WP-CLI команда");

    reg.register_tool("wp_db", [](const ToolArgs& a) -> std::string {
        return wp_db(a.query);
    }, "SQL-запрос через WP");

    reg.register_tool("wp_media", [](const ToolArgs& a) -> std::string {
        return wp_media(a.k);
    }, "Список медиа");

    reg.register_tool("wp_option", [](const ToolArgs& a) -> std::string {
        return wp_option(a.query);
    }, "Опция WordPress");

    reg.register_tool("wp_rest", [](const ToolArgs& a) -> std::string {
        return wp_rest(a.query);
    }, "REST API");

    reg.register_tool("wp_check_deps", [](const ToolArgs&) -> std::string {
        return wp_check_deps();
    }, "Проверка зависимостей");

    reg.register_tool("wp_create_site", [](const ToolArgs& a) -> std::string {
        return wp_create_site(a.query, a.pattern, a.content, a.cli, a.url);
    }, "Создание WP-сайта");

    reg.register_tool("deploy", [](const ToolArgs&) -> std::string {
        return deploy();
    }, "Деплой на хостер");

    reg.register_tool("verify", [](const ToolArgs&) -> std::string {
        return verify();
    }, "Комплексная проверка");

    reg.register_tool("php_lint", [](const ToolArgs& a) -> std::string {
        return php_lint(a.path);
    }, "Проверка синтаксиса PHP");

    reg.register_tool("headless_render", [](const ToolArgs& a) -> std::string {
        return headless_render(a.url);
    }, "Рендер DOM сайта");

    reg.register_tool("validate", [](const ToolArgs&) -> std::string {
        return validate();
    }, "Проверка синтаксиса PHP всех файлов");
}

/* ===== WP-навыки ===== */

static const char* kWpThemeSkill =
    "# wp_theme\n"
    "Описание: иерархия шаблонов и безопасная вёрстка темы\n"
    "При правке темы WordPress:\n"
    "- Точка входа — style.css (заголовок темы обязателен) и index.php.\n"
    "- Подключай стили/скрипты только через wp_enqueue_scripts.\n"
    "- Используй цикл: if ( have_posts() ) : while ( have_posts() ) : the_post(); ... endwhile; endif;\n"
    "- Экранируй вывод: esc_html(), esc_attr(), esc_url(); перевод — __('...', 'textdomain').\n"
    "- Не правь wp-includes/wp-admin — только wp-content/themes/<theme> и wp-content/plugins.";

static const char* kWpHookSkill =
    "# wp_hook\n"
    "Описание: правильные хуки WordPress (action/filter/shortcode)\n"
    "- add_action( 'init', 'my_init' ); — первый аргумент имя хука, второй — коллбэк.\n"
    "- add_filter( 'the_title', 'my_title_filter', 10, 1 ); — фильтр ДОЛЖЕН возвращать return.\n"
    "- Имя коллбэка уникально; префиксуй функции чтобы не конфликтовать.\n"
    "- Для shortcode: add_shortcode( 'mysc', 'my_shortcode_cb' );\n"
    "- Никогда не выводи echo внутри фильтра — только return.";

static const char* kWpDatabaseSkill =
    "# wp_database\n"
    "Описание: работа с базой данных WordPress через wp-cli\n"
    "- Используй wp db query для SQL-запросов.\n"
    "- Всегда делай бэкап перед изменением: wp db export backup.sql.\n"
    "- Для поиска данных используй wp db search.\n"
    "- Проверяй опции через wp option get / wp option update.\n"
    "- Не удаляй таблицы ядра без крайней необходимости.";

static const char* kWpMediaSkill =
    "# wp_media\n"
    "Описание: работа с медиафайлами WordPress\n"
    "- Загрузка: wp media import <file> --title='...' --featured_image.\n"
    "- Список: wp media list --posts_per_page=N.\n"
    "- Удаление: wp media delete <id>.\n"
    "- Для галерей используй shortcode [gallery ids='1,2,3'].";

static const char* kWpPluginBoilerplateSkill =
    "# wp_plugin_boilerplate\n"
    "Описание: структура плагина WordPress\n"
    "- Минимальный плагин: один PHP-файл с комментарием в шапке (Plugin Name, Description, Version).\n"
    "- Хуки: register_activation_hook / register_deactivation_hook.\n"
    "- Для_Options API: register_setting / add_settings_section / add_settings_field.\n"
    "- Админка: add_menu_page / add_submenu_page.\n"
    "- Не забывай nonce: wp_verify_nonce / wp_create_nonce.\n"
    "- Текстуризация: esc_html(), esc_attr(), esc_url(), sanitize_text_field().";

static const char* kWpGitSkill =
    "# wp_git\n"
    "Описание: Git для WordPress-проектов\n"
    "- .gitignore: wp-config.php, wp-content/uploads/, wp-content/plugins/*/vendor/.\n"
    "- Коммиты: регулярные, с описанием изменений.\n"
    "- Ветки: main (продакшн), dev (разработка), feature/*.\n"
    "- Деплой через rsync или Git-хук (post-receive).";

std::vector<Skill> get_wp_skills() {
    return {
        {"wp_theme", "Иерархия шаблонов и безопасная вёрстка темы", kWpThemeSkill},
        {"wp_hook", "Правильные хуки WordPress (action/filter/shortcode)", kWpHookSkill},
        {"wp_database", "Работа с базой данных WordPress через wp-cli", kWpDatabaseSkill},
        {"wp_media", "Работа с медиафайлами WordPress", kWpMediaSkill},
        {"wp_plugin_boilerplate", "Структура плагина WordPress", kWpPluginBoilerplateSkill},
        {"wp_git", "Git для WordPress-проектов", kWpGitSkill}
    };
}

} // namespace wp
} // namespace coder

#include "wp_coder.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <cstdio>
#include <algorithm>

#include "headless_browser/headless_browser.h"

namespace fs = std::filesystem;

namespace {

const std::vector<std::string> kSkipDirs = {".git", "node_modules", "vendor",
                                            "wp-includes", "wp-admin"};

/* Рекурсивный обход *.php файлов в root (с пропуском служебных каталогов). */
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

std::string read_text_file(const std::string& path, size_t max_chars = 60000) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "[ошибка] не удалось открыть файл: " + path;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() > max_chars) {
        s.resize(max_chars);
        s += "\n...[обрезано]";
    }
    return s;
}

/* Извлечь хуки: add_action/add_filter/add_shortcode с именем. */
std::string grep_hooks(const std::string& root, const std::string& pattern) {
    std::vector<std::string> files;
    walk_php(root, files);
    std::regex re(R"((add_action|add_filter|add_shortcode)\s*\(\s*['"]([^'"]+)['"])");
    std::regex pat_re;
    bool have_pat = !pattern.empty();
    if (have_pat) {
        try { pat_re = std::regex(pattern); } catch (...) { have_pat = false; }
    }
    std::stringstream out;
    size_t found = 0;
    for (const auto& fp : files) {
        std::ifstream f(fp);
        if (!f) continue;
        std::string line;
        size_t ln = 0;
        while (std::getline(f, line)) {
            ++ln;
            std::smatch m;
            if (std::regex_search(line, m, re)) {
                std::string hook = m[2].str();
                if (have_pat && !std::regex_search(hook, pat_re)) continue;
                out << fp << ":" << ln << "  " << m[1].str() << "(\"" << hook << "\")\n";
                ++found;
            }
        }
    }
    out << "[найдено хуков: " << found << " в " << files.size() << " php-файлах]";
    return out.str();
}

std::string php_lint(const std::string& path) {
    if (g_state.php_bin.empty())
        return "[ошибка] php-cli не найден (проверьте wp_coder.php_bin / PATH)";
    std::string cmd = g_state.php_bin + " -l " + std::string("\"") + path + std::string("\"");
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "[ошибка] не удалось запустить php -l";
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    return out.empty() ? "[php -l: нет вывода (файл, видимо, синтаксически корректен)]" : out;
}

/* wp-cli: запуск `wp --path=<root> <args>` (wp должен быть в PATH). */
std::string wp_cli(const std::string& args) {
    if (args.empty()) return "[ошибка] пустая команда wp-cli";
    std::string cmd = "wp --path=" + std::string("\"") + g_state.project_dir + std::string("\"")
                      + " " + args + " --no-color 2>&1";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "[ошибка] не удалось запустить wp (wp-cli не установлен?)";
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    if (out.size() > 8000) { out.resize(8000); out += "\n...[обрезано]"; }
    return out.empty() ? "[wp-cli: нет вывода]" : out;
}

/* Headless-браузер: рендер DOM локального/удалённого сайта (проверка вёрстки). */
std::string headless_render(const std::string& url) {
    if (url.empty()) return "[ошибка] пустой URL";
    headless_browser::RenderOptions opts;
    if (!headless_browser::available(opts))
        return "[ошибка] headless-браузер недоступен (нужен chromium в PATH)";
    std::string err;
    std::string dom = headless_browser::render_dom(url, opts, &err);
    if (dom.empty()) return "[ошибка рендера] " + err;
    bool thin = headless_browser::is_thin_content(dom);
    std::string preview = dom.substr(0, 6000);
    std::stringstream s;
    s << "[DOM длина=" << dom.size() << " символов, thin_content=" << (thin ? "да" : "нет")
      << "]\n" << preview;
    if (dom.size() > 6000) s << "\n...[обрезано]";
    return s.str();
}

/* wp_rest: GET к /wp-json/wp/v2/<ep> с Basic-авторизацией (app_password). */
std::string wp_rest(const std::string& ep) {
    if (g_state.wp_site_url.empty() || g_state.wp_app_password.empty())
        return "[ошибка] не заданы wp_site_url / app_password (вкладка Проект)";
    std::string url = g_state.wp_site_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/wp-json/wp/v2/" + ep;
    std::string cmd = "curl -s -m 30 --fail -u "
        + std::string("\"") + g_state.wp_app_user + ":" + g_state.wp_app_password + std::string("\"")
        + " " + std::string("\"") + url + std::string("\"");
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "[ошибка] не удалось запустить curl";
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    if (out.size() > 8000) { out.resize(8000); out += "\n...[обрезано]"; }
    return out.empty() ? "[wp_rest: пустой ответ (проверьте URL/креды/эндпоинт)]" : out;
}

/* validate: php -l по всем php-файлам проекта (ловим синтаксические ошибки). */
std::string validate() {
    std::vector<std::string> files;
    walk_php(g_state.project_dir, files);
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

/* deploy: rsync (рекоменд.) либо внешний deploy.sh в корне проекта (ftp/sftp). */
std::string deploy() {
    if (g_state.project_dir.empty())
        return "[ошибка] не задан project_dir";
    if (g_state.deploy_proto == "rsync") {
        if (g_state.deploy_host.empty() || g_state.deploy_remote_dir.empty())
            return "[ошибка] не заданы deploy_host / deploy_remote_dir";
        std::string target = g_state.deploy_user.empty()
            ? g_state.deploy_host
            : (g_state.deploy_user + "@" + g_state.deploy_host);
        std::string cmd = "rsync -az --delete --exclude=wp-config.php --exclude=.git --exclude=node_modules "
            + std::string("\"") + g_state.project_dir + "/\""
            + " " + std::string("\"") + target + ":" + g_state.deploy_remote_dir + "/\"";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return "[ошибка] не удалось запустить rsync";
        std::string out; char buf[4096];
        while (fgets(buf, sizeof(buf), f)) out += buf;
        pclose(f);
        return "[deploy rsync] " + (out.empty() ? "успешно (без вывода)" : out);
    }
    /* ftp/ftps/sftp — полагаемся на внешний deploy.sh в корне проекта
       (аналог news_rewriter/deploy.sh: curl/sshpass/sftp). */
    fs::path script = fs::path(g_state.project_dir) / "deploy.sh";
    if (!fs::exists(script))
        return "[ошибка] proto=" + g_state.deploy_proto
               + ", но нет " + script.string()
               + " (создайте deploy.sh по образу news_rewriter)";
    std::string cmd = std::string("\"") + script.string() + std::string("\"");
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "[ошибка] не удалось запустить deploy.sh";
    std::string out; char buf[4096];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    return "[deploy " + g_state.deploy_proto + "] " + (out.empty() ? "успешно" : out);
}

/* verify: авто-проверка через WP — php -l + HTTP-статус локального сайта + рендер. */
std::string verify() {
    std::stringstream out;
    out << validate() << "\n";
    if (!g_state.wp_local_url.empty()) {
        std::string url = g_state.wp_local_url;
        std::string curl = "curl -s -o /dev/null -m 20 -w '%{http_code}' "
            + std::string("\"") + url + std::string("\"");
        FILE* f = popen(curl.c_str(), "r");
        std::string code;
        if (f) { char b[16]; while (fgets(b, sizeof(b), f)) code += b; pclose(f); }
        out << "[HTTP " << (code.empty() ? "?" : code) << "] " << url << "\n";
        out << headless_render(url) << "\n";
    } else {
        out << "[verify: wp_local_url не задан — пропускаем проверку сайта]\n";
    }
    out << "[verify: готово]";
    return out.str();
}

/* list_skills: имена и описания доступных навыков. */
std::string list_skills() {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    if (g_state.skills.empty()) return "[навыков нет — положите .md в data_dir/wp_coder/skills]";
    std::stringstream s;
    s << "[навыки " << g_state.skills.size() << "]:\n";
    for (const auto& sk : g_state.skills)
        s << "• " << sk.name << " — " << sk.description << "\n";
    return s.str();
}

std::string rag_index(const std::string& root) {
    std::string base = root.empty() ? g_state.project_dir : root;
    if (base.empty()) return "[ошибка] не задан корень индексации (project_dir / root)";
    std::vector<std::string> files;
    walk_php(base, files);
    if (!g_api || !g_host) return "[ошибка] нет хост-API";
    int ok = 0;
    for (const auto& fp : files) {
        if (g_api->rag_process_document(g_host, fp.c_str()) == 1) ++ok;
    }
    std::stringstream s;
    s << "[проиндексировано " << ok << "/" << files.size() << " php-файлов в RAG]";
    return s.str();
}

std::string rag_query(const std::string& query, int k) {
    if (!g_api || !g_host) return "[ошибка] нет хост-API";
    if (query.empty()) return "[ошибка] пустой запрос";
    char* prompt = g_api->rag_build_prompt(g_host, query.c_str(),
                                           k > 0 ? k : 6, nullptr);
    if (!prompt) return "[ошибка] RAG не вернул контекст";
    std::string res = prompt;
    g_api->free_string(g_host, prompt);
    if (res.empty()) res = "[RAG: пусто — возможно, проект ещё не проиндексирован]";
    return res;
}

/* repo-map: компактный обзор проекта (файлы + функции/хуки) для контекста модели,
 * не выгружая содержимое целиком (идея aider repo-map). */
std::string repo_map(const std::string& root) {
    std::string base = root.empty() ? g_state.project_dir : root;
    std::vector<std::string> files;
    walk_php(base, files, 1500);
    if (files.empty()) return "[repo_map: php-файлы не найдены в " + base + "]";

    std::regex fn_re(R"((?:function|class)\s+([a-zA-Z_][a-zA-Z0-9_]*))");
    std::regex hook_re(R"((add_action|add_filter|add_shortcode)\s*\(\s*['"]([^'"]+)['"])");

    std::stringstream out;
    out << "[repo_map] " << files.size() << " php-файлов:\n";
    for (const auto& fp : files) {
        std::ifstream f(fp);
        if (!f) continue;
        std::string line;
        std::vector<std::string> syms;
        while (std::getline(f, line)) {
            std::smatch m;
            if (std::regex_search(line, m, fn_re) && syms.size() < 40)
                syms.push_back("f:" + m[1].str());
            else if (std::regex_search(line, m, hook_re) && syms.size() < 40)
                syms.push_back("h:" + m[2].str());
        }
        std::string rel = fp;
        if (!base.empty() && rel.rfind(base, 0) == 0)
            rel = rel.substr(base.size() + 1);
        out << "• " << rel;
        if (!syms.empty()) {
            out << "  [";
            for (size_t i = 0; i < syms.size(); ++i) {
                if (i) out << ", ";
                out << syms[i];
            }
            out << "]";
        }
        out << "\n";
    }
    return out.str();
}

} // namespace

std::string tool_run(const std::string& tool,
                     const std::string& arg_path,
                     const std::string& arg_root,
                     const std::string& arg_query,
                     const std::string& arg_pattern,
                     int arg_k,
                     const std::string& arg_content,
                     const std::string& arg_cli,
                     const std::string& arg_url) {
    /* Ролевой режим: Research запрещает мутирующие инструменты. */
    if (g_state.mode == 1 && (tool == "write_file" || tool == "deploy"))
        return "[запрещено в режиме Research] инструмент " + tool
               + " не применяется (только чтение/исследование)";

    if (tool == "read_file") {
        return read_text_file(project_resolve(arg_path));
    }
    if (tool == "write_file") {
        /* План-режим: правка только предлагается, применяется по кнопке в UI. */
        if (g_state.plan_mode) {
            std::lock_guard<std::mutex> lk(g_state.mtx);
            g_state.pending.push_back({arg_path, arg_content});
            return "[предложено, НЕ применено] " + arg_path
                   + " (" + std::to_string(arg_content.size()) + " байт) — см. список ниже";
        }
        std::string abs = project_resolve(arg_path);
        std::ofstream f(abs, std::ios::binary);
        if (!f) return "[ошибка] не удалось записать: " + abs;
        f << arg_content;
        f.close();
        return "[записано] " + abs + " (" + std::to_string(arg_content.size()) + " байт)";
    }
    if (tool == "grep_hooks") {
        return grep_hooks(project_resolve(arg_root.empty() ? g_state.project_dir : arg_root),
                          arg_pattern);
    }
    if (tool == "php_lint") {
        return php_lint(project_resolve(arg_path));
    }
    if (tool == "wp_cli") {
        return wp_cli(arg_cli);
    }
    if (tool == "headless_render") {
        return headless_render(arg_url);
    }
    if (tool == "rag_index") {
        return rag_index(project_resolve(arg_root));
    }
    if (tool == "rag_query") {
        return rag_query(arg_query, arg_k);
    }
    if (tool == "repo_map") {
        return repo_map(project_resolve(arg_root));
    }
    if (tool == "wp_rest") {
        return wp_rest(arg_query);
    }
    if (tool == "validate") {
        return validate();
    }
    if (tool == "deploy") {
        return deploy();
    }
    if (tool == "verify") {
        return verify();
    }
    if (tool == "list_skills") {
        return list_skills();
    }
    return "[ошибка] неизвестный инструмент: " + tool;
}

void pending_apply(size_t idx) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    if (idx >= g_state.pending.size()) return;
    const auto& p = g_state.pending[idx];
    std::string abs = project_resolve(p.path);
    std::ofstream f(abs, std::ios::binary);
    if (f) { f << p.content; f.close(); }
    g_state.pending.erase(g_state.pending.begin() + idx);
}

void pending_discard(size_t idx) {
    std::lock_guard<std::mutex> lk(g_state.mtx);
    if (idx >= g_state.pending.size()) return;
    g_state.pending.erase(g_state.pending.begin() + idx);
}

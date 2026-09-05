#include "base_tools.h"
#include "tools_registry.h"
#include "engine.h"
#include "security.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;
namespace coder {

namespace {

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

void walk_all(const fs::path& root, std::vector<std::string>& out, size_t limit = 4000) {
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
        if (it->is_regular_file()) {
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

/* Разрешить относительный путь относительно корня проекта. */
std::string resolve_path(const std::string& rel) {
    const auto& st = engine_state();
    if (st.project_dir.empty()) return rel;
    if (rel.empty()) return st.project_dir;
    if (rel.size() > 0 && (rel[0] == '/' || rel.find(":") == 1)) return rel;
    std::string p = st.project_dir;
    if (!p.empty() && p.back() != '/') p += '/';
    p += rel;
    return p;
}

/* grep_search: поиск по файлам с использованием regex. */
std::string base_grep(const std::string& root, const std::string& pattern) {
    std::string base = root.empty() ? engine_state().project_dir : root;
    std::vector<std::string> files;
    walk_all(base, files, 2000);

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
            if (have_pat) {
                if (std::regex_search(line, pat_re)) {
                    out << fp << ":" << ln << "  " << line << "\n";
                    ++found;
                }
            }
        }
    }
    out << "[найдено совпадений: " << found << " в " << files.size() << " файлах]";
    return out.str();
}

/* repo_map: компактный обзор каталога. */
std::string base_repo_map(const std::string& root) {
    std::string base = root.empty() ? engine_state().project_dir : root;
    std::vector<std::string> files;
    walk_all(base, files, 1500);
    if (files.empty()) return "[repo_map: файлы не найдены в " + base + "]";

    std::regex fn_re(R"((?:function|class)\s+([a-zA-Z_][a-zA-Z0-9_]*))");
    std::regex hook_re(R"((add_action|add_filter|add_shortcode)\s*\(\s*['"]([^'"]+)['"])");

    std::stringstream out;
    out << "[repo_map] " << files.size() << " файлов:\n";
    for (const auto& fp : files) {
        std::string rel = fp;
        if (!base.empty() && rel.rfind(base, 0) == 0)
            rel = rel.substr(base.size() + 1);
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
        out << "  " << rel;
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

/* list_skills: имена и описания доступных навыков. */
std::string base_list_skills() {
    const auto& skills = SkillsManager::instance().all_skills();
    if (skills.empty()) return "[навыков нет]";
    std::stringstream s;
    s << "[навыки " << skills.size() << "]:\n";
    for (const auto& sk : skills)
        s << "  " << sk.name << " — " << sk.description << "\n";
    return s.str();
}

} // anonymous namespace

void register_base_tools() {
    auto& reg = ToolsRegistry::instance();

    reg.register_tool("read_file", [](const ToolArgs& a) -> std::string {
        std::string abs = resolve_path(a.path);
        return read_text_file(abs);
    }, "Чтение файла");

    reg.register_tool("write_file", [](const ToolArgs& a) -> std::string {
        auto& st = engine_state();
        if (st.plan_mode) {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.pending.push_back({a.path, a.content});
            return "[предложено, НЕ применено] " + a.path
                   + " (" + std::to_string(a.content.size()) + " байт)";
        }
        std::string abs = resolve_path(a.path);
        if (!security::is_path_safe(a.path))
            return "[запрещено] небезопасный путь: " + a.path;
        if (!security::is_path_not_dangerous(abs))
            return "[запрещено] запись запрещена в: " + abs;
        std::ofstream f(abs, std::ios::binary);
        if (!f) return "[ошибка] не удалось записать: " + abs;
        f << a.content;
        f.close();
        return "[записано] " + abs + " (" + std::to_string(a.content.size()) + " байт)";
    }, "Запись файла");

    reg.register_tool("repo_map", [](const ToolArgs& a) -> std::string {
        return base_repo_map(a.root);
    }, "Обзор структуры каталога");

    reg.register_tool("grep_search", [](const ToolArgs& a) -> std::string {
        return base_grep(a.root, a.pattern);
    }, "Поиск по файлам");

    reg.register_tool("list_skills", [](const ToolArgs& a) -> std::string {
        return base_list_skills();
    }, "Список навыков");

    /* search_replace: поиск и замена текста в файле (diff-based edit). */
    reg.register_tool("search_replace", [](const ToolArgs& a) -> std::string {
        std::string abs = resolve_path(a.path);
        std::ifstream fin(abs, std::ios::binary);
        if (!fin) return "[ошибка] не удалось открыть файл: " + abs;
        std::string content((std::istreambuf_iterator<char>(fin)),
                            std::istreambuf_iterator<char>());
        fin.close();

        /* a.query = что искать, a.content = на что заменять. */
        if (a.query.empty()) return "[ошибка] пустой поисковый запрос (QUERY)";
        size_t pos = content.find(a.query);
        if (pos == std::string::npos)
            return "[search_replace] текст не найден в " + abs;

        /* Проверяем уникальность. */
        size_t count = 0;
        size_t search_from = 0;
        while ((pos = content.find(a.query, search_from)) != std::string::npos) {
            count++;
            search_from = pos + 1;
        }
        if (count > 1)
            return "[search_replace] НАЙДЕНО " + std::to_string(count)
                   + " ВХОЖДЕНИЙ. Уточни запрос (добавь контекст вокруг замены).";

        /* Одно вхождение — заменяем. */
        pos = content.find(a.query);
        content.replace(pos, a.query.size(), a.content);

        if (engine_state().plan_mode) {
            std::lock_guard<std::mutex> lk(engine_state().mtx);
            engine_state().pending.push_back({a.path, content});
            return "[предложено] " + abs + " (search_replace, "
                   + std::to_string(a.query.size()) + " -> "
                   + std::to_string(a.content.size()) + " байт)";
        }

        std::ofstream fout(abs, std::ios::binary | std::ios::trunc);
        if (!fout) return "[ошибка] не удалось записать: " + abs;
        fout << content;
        fout.close();
        return "[search_replace] " + abs + ": заменено " + std::to_string(a.query.size())
               + " -> " + std::to_string(a.content.size()) + " байт";
    }, "Поиск и замена текста в файле");

    /* exec_command: запуск shell-команды. */
    reg.register_tool("exec_command", [](const ToolArgs& a) -> std::string {
        if (a.cli.empty()) return "[ошибка] пустая команда (CLI)";
        if (!security::is_command_allowed(a.cli))
            return "[запрещено] команда заблокирована политикой безопасности";
        std::string full = a.cli + " 2>&1";
        FILE* f = popen(full.c_str(), "r");
        if (!f) return "[ошибка] не удалось запустить: " + a.cli;
        char buf[4096];
        std::string out;
        while (fgets(buf, sizeof(buf), f)) out += buf;
        pclose(f);
        if (out.size() > 10000) { out.resize(10000); out += "\n...[обрезано]"; }
        return out.empty() ? "[exec: нет вывода]" : out;
    }, "Запуск shell-команды");
}

void register_rag_tools() {
    auto& reg = ToolsRegistry::instance();

    reg.register_tool("rag_index", [](const ToolArgs& a) -> std::string {
        std::string base = a.root.empty() ? engine_state().project_dir : a.root;
        if (base.empty()) return "[ошибка] не задан корень индексации";
        std::vector<std::string> files;
        walk_php(base, files);
        const auto& cb = Engine::instance().callbacks();
        int ok = 0;
        for (const auto& fp : files) {
            if (cb.rag_process_document && cb.rag_process_document(fp)) ++ok;
        }
        std::stringstream s;
        s << "[проиндексировано " << ok << "/" << files.size() << " php-файлов в RAG]";
        return s.str();
    }, "Индексация в RAG");

    reg.register_tool("rag_query", [](const ToolArgs& a) -> std::string {
        const auto& cb = Engine::instance().callbacks();
        if (!cb.rag_build_prompt) return "[ошибка] RAG не доступен";
        std::string result = cb.rag_build_prompt(a.query, a.k, "");
        if (result.empty()) return "[RAG: пусто — проект не проиндексирован]";
        return result;
    }, "Поиск в RAG");
}

} // namespace coder

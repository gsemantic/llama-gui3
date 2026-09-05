#include "skills_manager.h"
#include "module_api.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;
namespace coder {

SkillsManager& SkillsManager::instance() {
    static SkillsManager mgr;
    return mgr;
}

void SkillsManager::load() {
    skills_.clear();
    active_.clear();

    /* 1. Собираем навыки из всех зарегистрированных модулей. */
    const auto& modules = ModuleRegistry::instance().modules();
    for (const auto* mod : modules) {
        if (mod->get_skills) {
            auto mod_skills = mod->get_skills();
            for (auto& sk : mod_skills) {
                skills_.push_back(std::move(sk));
            }
        }
    }

    /* 2. Загружаем .md файлы из каталогов навыков.
     *    Ищем в: plugin_dir/skills/, data_dir/coder/skills/ */
    /* Внешние каталоги загружаются вызывающим кодом (plugin_main.cpp)
     * через load_from_directory() — здесь только объединяем. */
}

void SkillsManager::load_from_directory(const std::string& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;

    for (auto it = fs::directory_iterator(dir, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file() || it->path().extension() != ".md") continue;

        std::ifstream f(it->path());
        if (!f) continue;

        std::string text;
        {
            std::stringstream ss;
            ss << f.rdbuf();
            text = ss.str();
        }

        Skill sk;
        sk.name = it->path().stem().string();

        /* Парсинг: первая строка "# Имя" -> имя, вторая -> описание, остальное -> body. */
        std::istringstream is(text);
        std::string line;
        bool first = true;
        std::stringstream body;

        while (std::getline(is, line)) {
            if (first && !line.empty() && line[0] == '#') {
                sk.name = line.substr(1);
                /* trim */
                while (!sk.name.empty() && (sk.name[0] == ' ' || sk.name[0] == '\t'))
                    sk.name.erase(0, 1);
                first = false;
                continue;
            }
            if (first) first = false;

            if (sk.description.empty() && !line.empty()) {
                sk.description = line;
                size_t c = sk.description.find(": ");
                if (c != std::string::npos)
                    sk.description = sk.description.substr(c + 2);
            } else {
                body << line << "\n";
            }
        }
        sk.body = body.str();

        /* Не дублируем если уже есть от модуля (модуль имеет приоритет). */
        bool exists = false;
        for (const auto& s : skills_) {
            if (s.name == sk.name) { exists = true; break; }
        }
        if (!exists) {
            skills_.push_back(std::move(sk));
        }
    }
}

void SkillsManager::set_active(const std::vector<std::string>& names) {
    active_ = names;
}

void SkillsManager::toggle(const std::string& name, bool on) {
    auto it = std::find(active_.begin(), active_.end(), name);
    if (on && it == active_.end()) {
        active_.push_back(name);
    } else if (!on && it != active_.end()) {
        active_.erase(it);
    }
}

const std::vector<Skill>& SkillsManager::all_skills() const {
    return skills_;
}

const std::vector<std::string>& SkillsManager::active_skills() const {
    return active_;
}

std::string SkillsManager::build_skills_prompt() const {
    std::string result;
    for (const auto& name : active_) {
        for (const auto& sk : skills_) {
            if (sk.name == name) {
                result += "\n\n[НАВЫК: " + sk.name + "]\n" + sk.body;
                break;
            }
        }
    }
    return result;
}

const Skill* SkillsManager::find(const std::string& name) const {
    for (const auto& sk : skills_) {
        if (sk.name == name) return &sk;
    }
    return nullptr;
}

} // namespace coder

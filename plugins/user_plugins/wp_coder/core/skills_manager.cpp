#include "skills_manager.h"
#include "module_api.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <cctype>

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

    /* 3. Восстанавливаем активные навыки.
     *
     * Раньше здесь active_ просто оставался пустым, а set_module()
     * вызывался из plugin_main.cpp только если настройка active_module
     * была непустой. На чистой установке это значило: навыков нет ВООБЩЕ,
     * build_skills_prompt() возвращал "", и модель не знала, что
     * инструмент skill_detail вообще существует. */
    refresh_active();
}

void SkillsManager::refresh_active() {
    if (active_module_.empty()) {
        /* Модуль не выбран — активны навыки всех модулей: лучше показать
         * лишний навык в каталоге, чем не показать ни одного. */
        set_active(all_skill_names());
    } else {
        std::vector<std::string> only;
        for (const auto& sk : skills_) {
            if (sk.module_name == active_module_) only.push_back(sk.name);
        }
        if (only.empty()) {
            std::cerr << "[wp_coder] skills: модуль '" << active_module_
                      << "' не дал навыков, активны навыки всех модулей"
                      << std::endl;
            set_active(all_skill_names());
        } else {
            set_active(only);
        }
    }
}

std::vector<std::string> SkillsManager::all_skill_names() const {
    std::vector<std::string> names;
    names.reserve(skills_.size());
    for (const auto& sk : skills_) names.push_back(sk.name);
    return names;
}

void SkillsManager::load_from_directory(const std::string& dir, const std::string& module_name) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    /* Запоминаем каталог даже если навыков в нём пока нет: системе
     * разрешений он нужен как доверенный (И2.4), а решение «каталог
     * пуст — значит не доверенный» зависит от того, что туда положили
     * на этой неделе. */
    if (std::find(dirs_.begin(), dirs_.end(), dir) == dirs_.end()) {
        dirs_.push_back(dir);
    }

    /* И9.5: что лежит в каталоге рядом с навыками, кроме самих навыков.
     *
     * Считается ОДИН раз на каталог, а не внутри цикла по файлам: это
     * содержимое каталога, а не свойство каждого навыка, и перебор каталога
     * на каждый файл означал бы, что число навыков умножает работу чтения
     * без нужды. Чужие .md отброшены — они и есть другие навыки, и их
     * повторение в «ресурсах» сбило бы модель с толку. */
    std::vector<std::string> sibling_files;
    {
        /* Конец итератора берётся ОДИН раз, а не конструируется заново в
         * условии цикла. Причина конкретная: конструктор directory_iterator
         * в условии затирает error_code, которой же заканчивается обход, —
         * и разыменование на выходе за последний элемент становится
         * разыменованием итератора «конец», то есть падением. Наблюдалось
         * как Segmentation fault в path::extension(). */
        std::error_code sec;
        fs::directory_iterator sit(dir, sec);
        const fs::directory_iterator send;
        if (!sec) {
            for (; sit != send; sit.increment(sec)) {
                if (sec) break;
                /* Чужие .md — это ДРУГИЕ навыки, а не ресурсы: перечислить
                 * их рядом с навыком значило бы сказать модели, что они
                 * принадлежат ему. */
                if (sit->path().extension() == ".md") continue;
                std::error_code isec;
                sibling_files.push_back(
                    sit->path().filename().string() +
                    (sit->is_directory(isec) ? "/" : ""));
            }
        }
        /* Порядок каталога не задан, а навыки попадают в промпт: без
         * сортировки один и тот же каталог давал бы разный текст от
         * запуска к запуску, а кэш промпта этого не различает. */
        std::sort(sibling_files.begin(), sibling_files.end());
    }

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

        /* Имя навыка = имя файла БЕЗ расширения (skills/wp_setup.md ->
         * wp_setup). Это единственная форма, которую модель может
         * безошибочно передать в skill_detail QUERY (поиск точный).
         *
         * Раньше имя бралось из строки заголовка '# ...':
         *   "# wp_setup — настройка окружения" -> "wp_setup — настройка окружения"
         *   "# Late Skill"                    -> "Late"
         * Оба варианта невызываемы или вводят в заблуждение.
         * Заголовок теперь идёт в описание. */
        Skill sk;
        sk.name = it->path().stem().string();

        /* Формат .md: "# Заголовок", затем строка-описание, затем тело. */
        std::istringstream is(text);
        std::string line;
        bool first = true;
        bool have_title = false;
        std::string title_holder;
        std::stringstream body;

        while (std::getline(is, line)) {
            if (first && !line.empty() && line[0] == '#') {
                title_holder = line.substr(1);
                size_t b = 0;
                while (b < title_holder.size() &&
                       (title_holder[b] == ' ' || title_holder[b] == '\t')) ++b;
                title_holder = title_holder.substr(b);
                have_title = !title_holder.empty();
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
        /* И9.5: ресурсы каталога достаются навыку вместе с телом. */
        sk.files = sibling_files;

        /* Имя из файла — основное. Заголовок в описании не дублируем: он
         * почти всегда совпадает с именем. */
        if (have_title && sk.name.empty()) {
            /* Файл без имени (например ".md") — sanitized-заголовок. */
            std::string t;
            for (char c : title_holder) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                    t += c;
            }
            if (t.empty()) t = "skill";
            sk.name = t;
        }
        if (have_title && sk.description.empty()) {
            sk.description = title_holder;
        }

        /* Не дублируем если уже есть от модуля (модуль имеет приоритет). */
        bool exists = false;
        for (const auto& s : skills_) {
            if (s.name == sk.name) { exists = true; break; }
        }
        if (!exists) {
            sk.module_name = module_name;
            skills_.push_back(std::move(sk));
        }
    }
}

void SkillsManager::set_module(const std::string& module_name) {
    active_module_ = module_name;
    refresh_active();
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
    /* Только имена и описания активно включённых навыков. Полные тела навыков
     * НЕ инжектируются в промпт — они тяжёлые и оплачиваются на каждом шаге.
     * Модель подгружает нужный навык через инструмент skill_detail. */
    if (skills_.empty()) {
        /* Навыков нет вообще. Модель всё равно должна знать про
         * skill_detail — иначе она не сможет вызвать его, даже если
         * пользователь установит навык позже в этой же сессии. */
        return "\n\n## НАВЫКИ\n"
               "Список навыков пуст. Инструмент skill_detail всё равно доступен: "
               "вызови его, чтобы проверить, не появились ли навыки.\n";
    }

    /* Каталог строим по всем навыкам, но с пометкой активных — так модель
     * видит и активные, и те, что можно включить через UI. */
    std::string result = "\n\n## НАВЫКИ\n"
                         "(подробная инструкция — через skill_detail QUERY: <имя>)\n";
    for (const auto& sk : skills_) {
        bool on = std::find(active_.begin(), active_.end(), sk.name) != active_.end();
        result += on ? "- " : "- (выключен) ";
        result += sk.name;
        if (!sk.description.empty()) result += " — " + sk.description;
        result += "\n";
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

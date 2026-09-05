#pragma once

/*
 * skills_manager.h — Менеджер навыков AI-кодера.
 *
 * Загружает навыки из:
 *   1. Модулей (каждый модуль предоставляет свои skills)
 *   2. Файлов .md в каталогах (data_dir/coder/skills/, plugin_dir/skills/)
 *
 * Навыки инжектируются в системный промпт при сборке.
 */

#include "module_api.h"
#include <string>
#include <vector>

namespace coder {

class SkillsManager {
public:
    static SkillsManager& instance();

    /* Загрузка навыков из модулей + файлов .md. */
    void load();

    /* Обновить список активных навыков. */
    void set_active(const std::vector<std::string>& names);
    void toggle(const std::string& name, bool on);

    /* Получить все доступные навыки. */
    const std::vector<Skill>& all_skills() const;

    /* Получить имена активных навыков. */
    const std::vector<std::string>& active_skills() const;

    /* Собрать текст активных навыков для инжекта в промпт. */
    std::string build_skills_prompt() const;

    /* Поиск навыка по имени. */
    const Skill* find(const std::string& name) const;

    /* Загрузка навыков из каталога .md файлов (внешний вызов). */
    void load_from_directory(const std::string& dir);

private:
    std::vector<Skill> skills_;
    std::vector<std::string> active_;
};

} // namespace coder

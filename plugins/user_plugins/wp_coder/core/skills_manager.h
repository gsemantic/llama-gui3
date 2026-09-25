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

    /* Пересчитать active_ под текущий набор skills_ и active_module_.
     * Вызывать ПОСЛЕ всех load_from_directory(), иначе навыки из
     * каталогов попадут в skills_, но не в active_. */
    void refresh_active();

    /* Установить активный модуль и обновить активные навыки.
     * Если у модуля нет навыков — подхватываются навыки всех модулей,
     * чтобы агент не остался без навыков молча. */
    void set_module(const std::string& module_name);

    /* Обновить список активных навыков. */
    void set_active(const std::vector<std::string>& names);
    void toggle(const std::string& name, bool on);

    /* Получить все доступные навыки. */
    const std::vector<Skill>& all_skills() const;

    /* Имена всех навыков (включая неактивные). */
    std::vector<std::string> all_skill_names() const;

    /* Получить имена активных навыков. */
    const std::vector<std::string>& active_skills() const;

    /* Собрать каталог навыков для инжекта в промпт.
     * НЕ возвращает пустую строку: даже при пустом наборе модель должна
     * знать, что инструмент skill_detail существует. */
    std::string build_skills_prompt() const;

    /* Поиск навыка по имени. */
    const Skill* find(const std::string& name) const;

    /* Загрузка навыков из каталога .md файлов (внешний вызов). */
    void load_from_directory(const std::string& dir, const std::string& module_name = "");

    /* Каталоги, из которых загружались навыки. Нужны системе разрешений
     * (И2.4): каталог навыков — доверенный, агент может писать в него
     * без вопроса пользователю. */
    const std::vector<std::string>& source_dirs() const { return dirs_; }

private:
    std::vector<Skill> skills_;
    std::vector<std::string> active_;
    std::string active_module_;
    std::vector<std::string> dirs_;
};

} // namespace coder

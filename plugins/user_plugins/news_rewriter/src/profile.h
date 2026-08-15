#pragma once

#include <string>
#include <vector>

#include "config.h"

namespace news_rewriter {

// Автономные профили настроек плагина — хранятся ВНЕ профилей основного
// приложения, как отдельные файлы в <data_dir>/news_rewriter/profiles/.
// Каждый профиль — это <slug>.json: {"name": <имя>, "config": <config_to_json>}.
// Активный профиль помечен в active.json: {"active": <имя>}.

// Базовый каталог профилей. Пусто, если data_dir не задан.
std::string profiles_dir(const std::string& data_dir);

// Список имён профилей (отсортированный). Пусто, если каталога нет.
std::vector<std::string> list_profiles(const std::string& data_dir);

// Имя активного профиля (без расширения). "" — если не задан/нет каталога.
std::string active_profile_name(const std::string& data_dir);

// Сделать профиль активным (записать active.json).
void set_active_profile(const std::string& data_dir, const std::string& name);

// Загрузить профиль по имени. Если не найден — default_config().
Config load_profile(const std::string& data_dir, const std::string& name);

// Сохранить профиль (создаёт/перезаписывает <slug>.json) и сделать его
// активным. Возвращает true при успехе записи.
bool save_profile(const std::string& data_dir, const std::string& name,
                  const Config& cfg);

// Удалить профиль по имени. Удаление последнего профиля запрещено
// (вернёт false). Если удалён активный — активируется первый оставшийся.
bool delete_profile(const std::string& data_dir, const std::string& name);

// Безопасный slug имени файла (буквы/цифры/дефис/подчёркивание, иначе '_').
std::string profile_slug(const std::string& name);

} // namespace news_rewriter

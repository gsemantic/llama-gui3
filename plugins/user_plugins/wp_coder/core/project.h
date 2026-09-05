#pragma once

/*
 * project.h — Универсальные настройки проекта AI-кодера.
 *
 * Абстрагирует настройки от WP-специфики.
 * WP-специфичные настройки (wp_site_url, app_password и т.д.)
 * живут в модуле WordPress.
 */

#include <string>

namespace coder {

/* Загрузка настроек проекта из хранилища хоста. */
void project_load_settings();

/* Сохранение настроек проекта. */
void project_save_settings();

/* Определение PHP (поиск в PATH). */
void project_detect_php();

/* Решение пути: относительный -> абсолютный. */
std::string project_resolve(const std::string& rel);

/* Вспомогательные настройки (храним как JSON-строку). */
std::string setting_get_str(const std::string& key, const std::string& def);
void setting_set_str(const std::string& key, const std::string& value);

} // namespace coder

#pragma once

/*
 * project.h — Пути и окружение проекта AI-кодера.
 *
 * Абстрагирует настройки от WP-специфики.
 * WP-специфичные навыки (wp_setup, деплой) живут в модуле WordPress.
 *
 * Про настройки: ЗДЕСЬ их нет. Ими занимается Engine
 * (Engine::load_settings / Engine::save_settings, engine.cpp), а модули
 * получают доступ через own_engine_settings() и CoderModule::load_settings.
 * Функции project_load_settings / project_save_settings /
 * setting_get_str / setting_set_str были заглушками без единого
 * вызывающего и удалены в И0.9.
 */

#include <string>

namespace coder {

/* Определение PHP (поиск в PATH). Пишет engine_state().php_bin. */
void project_detect_php();

/* Решение пути: относительный -> абсолютный относительно project_dir.
 * Возвращает rel как есть, если он абсолютный (POSIX или Windows-диск)
 * либо если project_dir не задан. */
std::string project_resolve(const std::string& rel);

} // namespace coder

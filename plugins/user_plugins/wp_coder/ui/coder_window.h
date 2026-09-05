#pragma once

/*
 * coder_window.h — Главное окно AI-кодера с выбором модуля.
 *
 * Рисует панель настроек проекта, селектор модуля, панель инструментов.
 */

#include "../core/engine.h"

namespace coder {
namespace ui {

/* Регистрация окон и команд плагина. */
void init_windows();

/* Отрисовка всех окон (вызывается из ll_plugin_render). */
void render_all_windows();

/* Отрисовка extras под лентой сообщений (для agent mode). */
void render_extras();

} // namespace ui
} // namespace coder

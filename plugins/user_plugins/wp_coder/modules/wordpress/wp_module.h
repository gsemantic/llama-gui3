#pragma once

/*
 * wp_module.h — Точка входа модуля WordPress.
 *
 * Регистрирует WP-инструменты, навыки и промпт в ядре AI-кодера.
 */

#include "../../core/module_api.h"

namespace coder {
namespace wp {

/* Регистрация модуля WordPress в глобальном реестре. */
void register_module();

} // namespace wp
} // namespace coder

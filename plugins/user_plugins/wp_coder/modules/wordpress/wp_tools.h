#pragma once

/*
 * wp_tools.h — WordPress-специфичные инструменты.
 *
 * wp_cli, wp_db, wp_media, wp_option, wp_rest, wp_create_site,
 * wp_check_deps, deploy, verify, php_lint, headless_render.
 */

#include "../../core/module_api.h"
#include <vector>

namespace coder {
namespace wp {

/* Регистрация WP-инструментов в реестре. */
void register_wp_tools();

/* WP-навыки. */
std::vector<Skill> get_wp_skills();

} // namespace wp
} // namespace coder

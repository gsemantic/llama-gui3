#pragma once

/*
 * security.h — Безопасность AI-кодера.
 *
 * Валидация путей, санитизация ввода, блокировка опасных операций.
 */

#include <string>
#include <vector>

namespace coder {
namespace security {

/* Проверка path traversal: путь не должен содержать ".." после нормализации. */
bool is_path_safe(const std::string& path);

/* Проверка: путь не является絕對но опасным (symlink на /etc/passwd и т.п.). */
bool is_path_not_dangerous(const std::string& abs_path);

/* Список запрещённых шаблонов для exec_command. */
const std::vector<std::string>& blocked_commands();

/* Проверка: команда не заблокирована. */
bool is_command_allowed(const std::string& cmd);

/* Санитизация строки для использования в shell (экранирование). */
std::string shell_escape(const std::string& s);

} // namespace security
} // namespace coder

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

/* Проверка: project_dir не является корнем ФС. */
bool is_project_dir_valid(const std::string& project_dir);

/* Проверка: путь не является опасным (symlink на /etc/passwd и т.п.). */
bool is_path_not_dangerous(const std::string& abs_path);

/* Список запрещённых шаблонов для exec_command. */
const std::vector<std::string>& blocked_commands();

/* Проверка: команда не заблокирована. */
bool is_command_allowed(const std::string& cmd);

/* Санитизация строки для использования в shell (экранирование). */
std::string shell_escape(const std::string& s);

/* Санитизация идентификатора: только alnum, _, -, точка.
 * Используется для имён контейнеров, сервисов, имён сайтов, опций и т.д.
 * Предотвращает shell injection через имена. */
std::string sanitize_ident(const std::string& s);

/* Валидация shell-аргумента: запрещает ; | & ` $ > < — метасимволы,
 * позволяющие инъекцию команд. Возвращает true если аргумент безопасен. */
bool is_shell_arg_safe(const std::string& s);

} // namespace security
} // namespace coder

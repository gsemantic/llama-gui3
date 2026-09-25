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

/* Список запрещённых ШАБЛОНОВ для exec_command. И3: остался как первая
 * грубая линия перед политикой команд (core/command_policy.h), потому что
 * подстрока ловит и то, что разбор команды не понял. Перечислять плохие
 * строки нельзя — именно этим был дефект D5: `rm -rf ~` в списке не
 * значит, что он запрещён. Решение принимает CommandPolicy. */
const std::vector<std::string>& blocked_commands();

/* Проверка: команда не попала в грубый список подстрок. */
bool is_command_allowed(const std::string& cmd);

/* Проверка команды политикой команд (И3.6). Пустая строка — разрешено,
 * непустая — причина отказа по-русски, её показывает инструмент.
 *
 * check_command() — для произвольных команд, написанных моделью
 * (exec_command): подстроги + allowlist программ + allowlist хостов.
 * check_assembled_command() — для команд, которые собрал код плагина из
 * аргументов модели (git_*, deploy, systemd_*, docker_*, ssh_exec,
 * wp_cli, python_*): подстроки не проверяются, allowlist программ не
 * применяется, но все запреты по бинарнику действуют. */
std::string check_command(const std::string& cmd);
std::string check_assembled_command(const std::string& cmd);

/* Доверенные сетевые хосты для curl/wget: localhost по умолчанию,
 * wp_site_url / wp_local_url / deploy_host — из настроек (И3.6). */
void trust_command_hosts(const std::vector<std::string>& hosts);

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

#pragma once

#include "agent_capabilities.h"
#include "agent_types.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace agents {

/**
 * @brief Уровни безопасности для агентов
 */
enum class SecurityLevel {
    NONE,       ///< Без ограничений (только встроенные агенты)
    STANDARD,   ///< Стандартный уровень (плагины с проверкой)
    STRICT,     ///< Строгий режим (только подписанные плагины)
    SANDBOX     ///< Максимальная изоляция
};

/**
 * @brief Разрешения для агента
 */
struct Permissions {
    std::unordered_set<std::string> allowed_paths;    ///< Разрешённые пути
    std::unordered_set<std::string> denied_paths;     ///< Запрещённые пути
    std::unordered_set<std::string> allowed_commands; ///< Разрешённые команды
    std::unordered_set<std::string> allowed_hosts;    ///< Разрешённые хосты
    int max_memory_mb = 512;                          ///< Максимум памяти (MB)
    int timeout_ms = 30000;                           ///< Таймаут (ms)
    bool allow_network = false;                       ///< Доступ к сети
    bool allow_filesystem = false;                    ///< Доступ к ФС
    bool allow_terminal = false;                      ///< Доступ к терминалу
};

/**
 * @brief Результат проверки безопасности
 */
struct SecurityCheckResult {
    bool allowed;
    std::string message;
    std::string reason;

    static SecurityCheckResult allow() {
        return {true, "OK", ""};
    }

    static SecurityCheckResult deny(const std::string& reason) {
        return {false, "Access denied", reason};
    }
};

/**
 * @brief Менеджер безопасности
 * 
 * Управляет разрешениями и проверками безопасности для агентов.
 * Проверяет права доступа к файлам, сети, командам терминала.
 */
class SecurityManager {
public:
    SecurityManager();
    ~SecurityManager();

    /**
     * @brief Установка уровня безопасности
     * @param level Уровень безопасности
     */
    void set_security_level(SecurityLevel level);

    /**
     * @brief Получение уровня безопасности
     */
    SecurityLevel get_security_level() const;

    /**
     * @brief Регистрация разрешений для агента
     * @param agent_name Имя агента
     * @param permissions Разрешения
     */
    void register_permissions(const std::string& agent_name,
                               const Permissions& permissions);

    /**
     * @brief Получение разрешений агента
     * @param agent_name Имя агента
     * @return Разрешения или nullptr
     */
    const Permissions* get_permissions(const std::string& agent_name) const;

    /**
     * @brief Проверка доступа к файлу
     * @param agent_name Имя агента
     * @param path Путь к файлу
     * @param write true для записи, false для чтения
     * @return Результат проверки
     */
    SecurityCheckResult check_file_access(const std::string& agent_name,
                                           const std::string& path,
                                           bool write = false);

    /**
     * @brief Проверка доступа к директории
     * @param agent_name Имя агента
     * @param path Путь к директории
     * @return Результат проверки
     */
    SecurityCheckResult check_directory_access(const std::string& agent_name,
                                                const std::string& path);

    /**
     * @brief Проверка команды терминала
     * @param agent_name Имя агента
     * @param command Команда
     * @return Результат проверки
     */
    SecurityCheckResult check_command(const std::string& agent_name,
                                       const std::string& command);

    /**
     * @brief Проверка URL
     * @param agent_name Имя агента
     * @param url URL
     * @return Результат проверки
     */
    SecurityCheckResult check_url(const std::string& agent_name,
                                   const std::string& url);

    /**
     * @brief Проверка возможности агента
     * @param agent_name Имя агента
     * @param capability Возможность
     * @return true если разрешено
     */
    bool check_capability(const std::string& agent_name,
                          AgentCapability capability) const;

    /**
     * @brief Проверка выделения памяти
     * @param agent_name Имя агента
     * @param size_mb Размер в MB
     * @return Результат проверки
     */
    SecurityCheckResult check_memory_allocation(const std::string& agent_name,
                                                 int size_mb);

    /**
     * @brief Добавление доверенного пути
     * @param path Путь
     */
    void add_trusted_path(const std::string& path);

    /**
     * @brief Добавление заблокированного пути
     * @param path Путь
     */
    void add_blocked_path(const std::string& path);

    /**
     * @brief Добавление разрешённой команды
     * @param agent_name Имя агента
     * @param command Команда
     */
    void add_allowed_command(const std::string& agent_name,
                              const std::string& command);

    /**
     * @brief Добавление разрешённого хоста
     * @param agent_name Имя агента
     * @param host Хост
     */
    void add_allowed_host(const std::string& agent_name,
                           const std::string& host);

    /**
     * @brief Проверка файла на наличие угроз
     * @param path Путь к файлу
     * @return Результат проверки
     */
    SecurityCheckResult scan_file(const std::string& path);

    /**
     * @brief Валидация плагина
     * @param plugin_path Путь к плагину
     * @return Результат валидации
     */
    SecurityCheckResult validate_plugin(const std::string& plugin_path);

    /**
     * @brief Получение статистики нарушений
     */
    std::unordered_map<std::string, int> get_violation_stats() const;

    /**
     * @brief Сброс статистики нарушений
     */
    void reset_violation_stats();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agents

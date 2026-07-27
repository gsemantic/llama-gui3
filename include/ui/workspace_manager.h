#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>

namespace llama_gui {
namespace ui {

/**
 * @brief Типы рабочих пространств
 */
enum class WorkspaceType {
    User,        // Обычный пользователь - базовые функции
    Developer,   // Разработчик - инструменты отладки и расширения
    Admin        // Администратор - сервер, безопасность, логи
};

/**
 * @brief Преобразует WorkspaceType в строку
 */
inline std::string workspace_type_to_string(WorkspaceType type) {
    switch (type) {
        case WorkspaceType::User: return "User";
        case WorkspaceType::Developer: return "Developer";
        case WorkspaceType::Admin: return "Admin";
        default: return "Unknown";
    }
}

/**
 * @brief Преобразует строку в WorkspaceType
 */
inline WorkspaceType string_to_workspace_type(const std::string& str) {
    if (str == "User") return WorkspaceType::User;
    if (str == "Developer") return WorkspaceType::Developer;
    if (str == "Admin") return WorkspaceType::Admin;
    return WorkspaceType::User; // по умолчанию
}

/**
 * @brief Конфигурация видимости меню для рабочего пространства
 */
struct WorkspaceMenuConfig {
    std::string menu_key;        // Постоянный ключ меню (на английском)
    std::string menu_name;       // Переведённое название меню
    bool visible = true;
    std::vector<std::string> visible_items; // если пусто - все видимы
    std::vector<std::string> hidden_items;  // явно скрытые элементы
};

/**
 * @brief Конфигурация рабочего пространства
 */
struct WorkspaceConfiguration {
    std::string name;
    WorkspaceType type = WorkspaceType::User;
    std::vector<WorkspaceMenuConfig> menu_configs;
    std::unordered_set<std::string> enabled_commands;
    std::unordered_set<std::string> disabled_commands;
    bool show_developer_tools = false;
    bool show_admin_tools = false;
};

/**
 * @brief Менеджер рабочих пространств
 * 
 * Управляет переключением между режимами работы приложения:
 * - User: базовый интерфейс для обычной работы
 * - Developer: инструменты разработки и отладки
 * - Admin: управление сервером и безопасностью
 */
class WorkspaceManager {
public:
    WorkspaceManager();
    ~WorkspaceManager();

    // Запрет копирования
    WorkspaceManager(const WorkspaceManager&) = delete;
    WorkspaceManager& operator=(const WorkspaceManager&) = delete;

    // =========================================================================
    // Инициализация и конфигурация
    // =========================================================================

    /**
     * @brief Инициализация менеджера с конфигурациями по умолчанию
     */
    void initialize();

    /**
     * @brief Инициализация с загрузкой конфигураций из файла
     * @param filepath Путь к файлу конфигурации
     */
    void initializeWithConfig(const std::string& filepath);

    /**
     * @brief Загрузка конфигураций из файла
     * @param filepath Путь к файлу конфигурации
     * @return true если успешно
     */
    bool loadConfig(const std::string& filepath);

    /**
     * @brief Сохранение текущих конфигураций в файл
     * @param filepath Путь к файлу конфигурации
     * @return true если успешно
     */
    bool saveConfig(const std::string& filepath) const;

    // =========================================================================
    // Переключение рабочих пространств
    // =========================================================================

    /**
     * @brief Переключить рабочее пространство по типу
     * @param type Тип рабочего пространства
     */
    void switchWorkspace(WorkspaceType type);

    /**
     * @brief Переключить рабочее пространство по имени
     * @param name Имя рабочего пространства
     * @return true если успешно
     */
    bool switchWorkspace(const std::string& name);

    /**
     * @brief Получить текущее рабочее пространство
     * @return Ссылка на текущую конфигурацию
     */
    const WorkspaceConfiguration& getCurrentWorkspace() const;

    /**
     * @brief Получить тип текущего рабочего пространства
     * @return Тип текущего workspace
     */
    WorkspaceType getCurrentWorkspaceType() const;

    /**
     * @brief Получить имя текущего рабочего пространства
     */
    std::string getCurrentWorkspaceName() const;

    // =========================================================================
    // Проверка видимости элементов
    // =========================================================================

    /**
     * @brief Проверить, видимо ли меню в текущем workspace
     * @param menu_name Имя меню
     * @return true если меню видимо
     */
    bool isMenuVisible(const std::string& menu_name) const;

    /**
     * @brief Проверить, видим ли элемент меню в текущем workspace
     * @param menu_name Имя меню
     * @param item_name Имя элемента
     * @return true если элемент видим
     */
    bool isMenuItemVisible(const std::string& menu_name, const std::string& item_name) const;

    /**
     * @brief Проверить, доступна ли команда в текущем workspace
     * @param command_name Имя команды
     * @return true если команда доступна
     */
    bool isCommandEnabled(const std::string& command_name) const;

    // =========================================================================
    // Доступ к конфигурациям
    // =========================================================================

    /**
     * @brief Получить все доступные рабочие пространства
     * @return Вектор имён рабочих пространств
     */
    std::vector<std::string> getAvailableWorkspaces() const;

    /**
     * @brief Получить конфигурацию по имени
     * @param name Имя рабочего пространства
     * @return Указатель на конфигурацию или nullptr если не найдено
     */
    const WorkspaceConfiguration* getWorkspaceConfig(const std::string& name) const;

    /**
     * @brief Получить конфигурацию по типу
     * @param type Тип рабочего пространства
     * @return Указатель на конфигурацию или nullptr если не найдено
     */
    const WorkspaceConfiguration* getWorkspaceConfig(WorkspaceType type) const;

    // =========================================================================
    // Callbacks для уведомлений о переключении
    // =========================================================================

    using WorkspaceChangedCallback = std::function<void(WorkspaceType)>;
    using MenuUpdateCallback = std::function<void()>;

    /**
     * @brief Добавить callback для уведомления о смене workspace
     * @param callback Функция обратного вызова
     */
    void addWorkspaceChangedCallback(WorkspaceChangedCallback callback);

    /**
     * @brief Удалить callback
     * @param callback Функция обратного вызова
     */
    void removeWorkspaceChangedCallback(WorkspaceChangedCallback callback);

    /**
     * @brief Установить callback для обновления меню после переключения workspace
     * @param callback Функция обратного вызова
     */
    void setMenuUpdateCallback(MenuUpdateCallback callback);

    /**
     * @brief Уведомить callbacks о смене workspace (публичный метод для инициализации)
     */
    void notifyWorkspaceChanged();

    // =========================================================================
    // Сериализация
    // =========================================================================

    /**
     * @brief Сериализовать текущую конфигурацию в JSON
     * @return JSON строка
     */
    std::string serializeToJson() const;

    /**
     * @brief Десериализовать конфигурацию из JSON
     * @param json_str JSON строка
     * @return true если успешно
     */
    bool deserializeFromJson(const std::string& json_str);

private:
    // Текущее рабочее пространство
    WorkspaceConfiguration current_workspace_;

    // Все доступные конфигурации
    std::unordered_map<std::string, WorkspaceConfiguration> workspaces_;

    // Callbacks для уведомлений
    std::vector<WorkspaceChangedCallback> workspace_changed_callbacks_;
    MenuUpdateCallback menu_update_callback_;

    // =========================================================================
    // Вспомогательные методы
    // =========================================================================

    /**
     * @brief Создать конфигурацию User workspace по умолчанию
     */
    WorkspaceConfiguration createDefaultUserWorkspace() const;

    /**
     * @brief Создать конфигурацию Developer workspace по умолчанию
     */
    WorkspaceConfiguration createDefaultDeveloperWorkspace() const;

    /**
     * @brief Создать конфигурацию Admin workspace по умолчанию
     */
    WorkspaceConfiguration createDefaultAdminWorkspace() const;
};

} // namespace ui
} // namespace llama_gui

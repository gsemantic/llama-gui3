#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace llama_gui {
namespace ui {

/**
 * @brief Пользовательская группа пунктов меню (кастомное подменю)
 *
 * Ссылается на пункты по стабильным ключам (command или name),
 * поэтому не ломается при смене языка интерфейса.
 */
struct MenuLayoutGroup {
    std::string id;                      // Стабильный id группы ("group_1", ...)
    std::string title;                   // Отображаемый заголовок (вводит пользователь)
    std::vector<std::string> item_keys;  // Ключи пунктов внутри группы, по порядку
};

/**
 * @brief Запись порядка верхнего уровня меню: пункт или группа
 */
struct MenuLayoutEntry {
    bool is_group = false;
    std::string key;   // Ключ пункта (если is_group == false) или id группы
};

/**
 * @brief Пользовательская раскладка одного меню в рамках workspace
 */
struct MenuLayoutForMenu {
    bool hidden = false;                        // Скрыть меню целиком
    std::vector<std::string> hidden_items;      // Скрытые пункты (по ключам)
    std::vector<MenuLayoutEntry> entries;       // Порядок верхнего уровня (пусто = как по умолчанию)
    std::vector<MenuLayoutGroup> groups;        // Пользовательские группы
};

/**
 * @brief Раскладка всех меню одного workspace
 */
struct MenuLayoutForWorkspace {
    std::string workspace_name;
    std::unordered_map<std::string, MenuLayoutForMenu> menus;  // По menu_key
};

/**
 * @brief Стабильный ключ пункта меню: command если задан, иначе имя
 *
 * Имя зависит от локали, command — нет, поэтому предпочитаем его.
 */
inline std::string make_menu_item_key(const std::string& command, const std::string& name) {
    return command.empty() ? name : command;
}

/**
 * @brief Менеджер пользовательской раскладки меню
 *
 * Хранит per-workspace настройки видимости, порядка и групп пунктов меню.
 * Не зависит от AdvancedMenuSystem (работает со строковыми ключами), что
 * позволяет подключать его и к системе меню, и к будущему редактору.
 *
 * Файл хранения: JSON (по умолчанию .config/llama-gui/menu_layout.json).
 */
class MenuLayoutManager {
public:
    MenuLayoutManager() = default;
    ~MenuLayoutManager() = default;

    MenuLayoutManager(const MenuLayoutManager&) = delete;
    MenuLayoutManager& operator=(const MenuLayoutManager&) = delete;

    // =========================================================================
    // Конфигурация
    // =========================================================================

    void setConfigPath(const std::string& path) { config_path_ = path; }
    const std::string& getConfigPath() const { return config_path_; }

    /**
     * @brief Активный workspace — область действия запросов и мутаций
     */
    void setActiveWorkspace(const std::string& name);
    const std::string& getActiveWorkspace() const { return active_workspace_; }

    // =========================================================================
    // Загрузка / сохранение
    // =========================================================================

    /**
     * @brief Загрузить раскладку из файла
     * @return true если файл прочитан; отсутствие файла не считается ошибкой
     */
    bool load();

    /**
     * @brief Сохранить раскладку в файл (создаёт каталоги при необходимости)
     */
    bool save() const;

    /** @brief Сериализовать всю раскладку в JSON (для экспорта) */
    std::string serializeToJson() const;

    /**
     * @brief Заменить состояние из JSON (для импорта)
     * @return true если разбор успешен; при ошибке состояние не меняется
     */
    bool deserializeFromJson(const std::string& json_str);

    bool isDirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }

    // =========================================================================
    // Запросы (для рендера)
    // =========================================================================

    bool isMenuHidden(const std::string& menu_key) const;
    bool isItemHidden(const std::string& menu_key, const std::string& item_key) const;

    const MenuLayoutForMenu* findMenuLayout(const std::string& menu_key) const;

    // =========================================================================
    // Мутации (для редактора раскладки)
    // =========================================================================

    MenuLayoutForMenu& getOrCreateMenuLayout(const std::string& menu_key);

    void setMenuHidden(const std::string& menu_key, bool hidden);
    void setItemHidden(const std::string& menu_key, const std::string& item_key, bool hidden);

    /** @brief Создать группу, вернуть её id */
    std::string createGroup(const std::string& menu_key, const std::string& title);
    void removeGroup(const std::string& menu_key, const std::string& group_id);

    /** @brief Создать или обновить группу (по id) */
    void setGroup(const std::string& menu_key, const MenuLayoutGroup& group);

    bool moveItemToGroup(const std::string& menu_key, const std::string& group_id,
                         const std::string& item_key);
    bool removeItemFromGroup(const std::string& menu_key, const std::string& group_id,
                             const std::string& item_key);

    /** @brief Задать порядок верхнего уровня (пункты + позиции групп) */
    void setEntries(const std::string& menu_key, const std::vector<MenuLayoutEntry>& entries);

    /** @brief Сбросить раскладку одного меню в активном workspace */
    void resetMenu(const std::string& menu_key);

    /** @brief Сбросить всю раскладку активного workspace */
    void resetActiveWorkspace();

    /** @brief Сбросить всю раскладку (все workspace) */
    void resetAll();

private:
    MenuLayoutForWorkspace& getOrCreateWorkspace(const std::string& name);
    MenuLayoutForWorkspace* findWorkspace(const std::string& name);
    const MenuLayoutForWorkspace* findWorkspace(const std::string& name) const;
    const MenuLayoutForWorkspace* findWorkspaceActive() const;

    std::string generateGroupId(const MenuLayoutForMenu& menu_layout) const;

    std::string config_path_ = ".config/llama-gui/menu_layout.json";
    std::string active_workspace_;
    std::unordered_map<std::string, MenuLayoutForWorkspace> workspaces_;
    mutable bool dirty_ = false;
};

} // namespace ui
} // namespace llama_gui

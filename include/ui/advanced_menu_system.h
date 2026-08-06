#pragma once

#include "command_manager.h"
#include "workspace_manager.h"
#include "window_manager.h"
#include "grid_snapping_system.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

#ifdef USE_IMGUI
#include "../external/imgui/imgui.h"
#endif

namespace llama_gui {
namespace ui {

// Структура для описания конфигурации окна (для сохранения в JSON)
struct WindowConfig {
    std::string name;
    bool visible;
    float x, y;
    float width, height;
};

// Тип элемента меню
enum class AdvancedMenuItemType {
    Item,
    Separator,
    Submenu
};

// Класс для управления окнами

// Структура для описания элемента меню с поддержкой управления окнами
struct AdvancedMenuItem {
    std::string name;                    // Имя элемента меню
    std::string command;                 // Имя команды для выполнения
    std::string shortcut;                // Горячая клавиша
    bool enabled = true;                 // Доступность элемента
    bool checked = false;                // Состояние checkbox/radio
    bool separator = false;              // Является ли разделителем
    std::string tooltip;                 // Подсказка
    std::function<bool()> check_func;    // Функция для checkbox/radio
    std::function<void(bool)> toggle_func; // Функция переключения
    std::string window_name;             // Имя окна для управления (если применимо)
    bool is_window_toggle = false;       // Является ли элемент переключателем окна
    AdvancedMenuItemType type = AdvancedMenuItemType::Item; // Тип элемента
    int command_id = 0;                  // ID команды
    std::function<void()> callback;      // Callback функция
    std::vector<AdvancedMenuItem> submenu_items; // Подменю
};

// Структура для описания меню
struct AdvancedMenu {
    std::string name;                    // Имя меню (переводимое, для отображения)
    std::string menu_key;                // Ключ меню (постоянный, для поиска в конфиге)
    std::string shortcut;                // Горячая клавиша для открытия меню
    std::vector<AdvancedMenuItem> items; // Элементы меню
    std::vector<std::unique_ptr<AdvancedMenu>> submenus; // Подменю
    bool enabled = true;                 // Доступность меню
};

// Класс для построения и управления современными меню
class AdvancedMenuSystem {
public:
    AdvancedMenuSystem() = default;
    ~AdvancedMenuSystem() = default;

    // Удалить копирование
    AdvancedMenuSystem(const AdvancedMenuSystem&) = delete;
    AdvancedMenuSystem& operator=(const AdvancedMenuSystem&) = delete;

    // Инициализация с менеджером команд и менеджером рабочих пространств
    void initialize(CommandManager* command_manager, WindowManager* window_manager, WorkspaceManager* workspace_manager = nullptr);

    // Построение стандартного современного меню
    void buildModernMenu();
    
    // Перестроение меню (для смены языка)
    void rebuildModernMenu();

    // Добавление меню
    bool addMenu(std::unique_ptr<AdvancedMenu> menu);
    bool addMenu(const std::string& name, std::vector<AdvancedMenuItem> items);
    
    // Добавление элемента в меню
    bool addMenuItem(const std::string& menu_name, const AdvancedMenuItem& item);
    bool addSeparator(const std::string& menu_name);
    
    // Удаление меню и элементов
    bool removeMenu(const std::string& menu_name);
    bool removeMenuItem(const std::string& menu_name, const std::string& item_name);
    
    // Получение меню
    AdvancedMenu* getMenu(const std::string& menu_name);
    const AdvancedMenu* getMenu(const std::string& menu_name) const;
    
    // Получение меню по стабильному ключу (не зависит от локали имени)
    AdvancedMenu* getMenuByKey(const std::string& menu_key);
    const AdvancedMenu* getMenuByKey(const std::string& menu_key) const;
    
    // Получение всех меню
    std::vector<std::string> getAllMenuNames() const;
    
    // Обновление состояния меню
    void updateMenuStates();
    
    // Отрисовка главного меню
    void renderMainMenu();

    // Отрисовка переключателя языка
    void renderLanguageSelector();
    
    // Отрисовка контекстного меню
    bool renderContextMenu(const std::string& menu_name, const ImVec2& position);
    
    // Настройка горячих клавиш
    void handleKeyboardShortcuts();
    
    // Вспомогательный метод для рендеринга элементов меню
    void renderMenuItems(const std::vector<AdvancedMenuItem>& items);
    
    // Включение/выключение элементов меню
    void setMenuItemEnabled(const std::string& menu_name, const std::string& item_name, bool enabled);
    void setMenuEnabled(const std::string& menu_name, bool enabled);
    
    // Настройка состояния checkbox/radio
    void setMenuItemChecked(const std::string& menu_name, const std::string& item_name, bool checked);
    bool isMenuItemChecked(const std::string& menu_name, const std::string& item_name) const;
    
    // Поиск элементов меню
    AdvancedMenuItem* findMenuItem(const std::string& menu_name, const std::string& item_name);
    const AdvancedMenuItem* findMenuItem(const std::string& menu_name, const std::string& item_name) const;
    
    // Экспорт/импорт конфигурации меню
    std::string exportMenuConfiguration() const;
    bool importMenuConfiguration(const std::string& config);
    
    // Получение статистики
    struct Statistics {
        size_t total_menus = 0;
        size_t total_items = 0;
        size_t total_shortcuts = 0;
        size_t total_windows = 0;
    };

    Statistics getStatistics() const;

    // Работа с workspace
    void setWorkspaceManager(WorkspaceManager* workspace_manager);
    WorkspaceManager* getWorkspaceManager() const { return workspace_manager_; }

    // Проверка, что меню уже построены
    bool isMenuBuilt() const { return !menus_ordered_.empty(); }

    // Переключение workspace через меню
    void switchToWorkspace(WorkspaceType type);
    void switchToWorkspace(const std::string& name);

    // Сохранение конфигурации рабочего пространства
    void saveWorkspace(const std::string& name);
    void loadWorkspace(const std::string& name);
    void setCurrentWorkspace(const WorkspaceConfig& config);
    
    // Получение списка рабочих пространств
    std::vector<std::string> getWorkspaceNames() const;
    bool hasWorkspace(const std::string& name) const;
    void deleteWorkspace(const std::string& name);
    std::string getCurrentWorkspaceName() const { return current_workspace_.name; }

    // Получить список workspace из файлов на диске
    std::vector<std::string> getAvailableWorkspaceFiles() const;
    
private:
    CommandManager* command_manager_ = nullptr;
    WindowManager* window_manager_ = nullptr;
    WorkspaceManager* workspace_manager_ = nullptr;
    std::vector<std::unique_ptr<AdvancedMenu>> menus_ordered_;  // Упорядоченный список меню
    std::unordered_map<std::string, AdvancedMenu*> menus_map_;  // Быстрый поиск по имени
    WorkspaceConfig current_workspace_;
    std::unordered_map<std::string, WorkspaceConfig> workspaces_;

    // Вспомогательные методы
    AdvancedMenu* findMenuInternal(const std::string& menu_name);
    void renderSubmenu(AdvancedMenu* submenu);
    void updateMenuItemState(AdvancedMenuItem& item);
    bool executeMenuItem(const AdvancedMenuItem& item);
    
    // Обновление списка окон в меню "Window" (динамически из WindowManager)
    void refreshWindowMenu();
    // Заполнить пункты меню из текущего списка окон
    void populateWindowMenu(AdvancedMenu& menu);
    
    // Обновление видимости меню для workspace
    void updateMenuVisibilityForWorkspace(WorkspaceType type);
    
    // Создание стандартных меню
    std::unique_ptr<AdvancedMenu> createFileMenu();
    std::unique_ptr<AdvancedMenu> createSettingsMenu();
    std::unique_ptr<AdvancedMenu> createViewMenu();
    std::unique_ptr<AdvancedMenu> createWindowMenu();
    std::unique_ptr<AdvancedMenu> createHelpMenu();
    std::unique_ptr<AdvancedMenu> createDeveloperMenu();
    std::unique_ptr<AdvancedMenu> createAgentsMenu();

    // Новые меню для разных workspace
    std::unique_ptr<AdvancedMenu> createToolsMenu();       // Для Developer
    // createDebugMenu() удалено - дублировало Developer menu
    std::unique_ptr<AdvancedMenu> createPerformanceMenu(); // Для Admin
    std::unique_ptr<AdvancedMenu> createSecurityMenu();    // Для Admin
    std::unique_ptr<AdvancedMenu> createLoggingMenu();     // Для Admin
    
    // Парсинг конфигурации
    bool parseMenuConfiguration(const std::string& config);
    std::unique_ptr<AdvancedMenu> parseMenuFromJson(const std::string& json);
};

// Вспомогательные функции для создания элементов меню
namespace AdvancedMenuItemFactory {
    
    AdvancedMenuItem createCommandItem(const std::string& name, const std::string& command,
                                      const std::string& shortcut = "", 
                                      const std::string& tooltip = "");
    
    AdvancedMenuItem createSeparator();
    
    AdvancedMenuItem createCheckboxItem(const std::string& name, const std::string& command,
                                       std::function<bool()> check_func, 
                                       std::function<void(bool)> toggle_func,
                                       const std::string& shortcut = "", 
                                       const std::string& tooltip = "");
    
    AdvancedMenuItem createWindowToggleItem(const std::string& name, const std::string& window_name,
                                           const std::string& shortcut = "", 
                                           const std::string& tooltip = "");
}

// Конфигурация меню по умолчанию
namespace DefaultAdvancedMenuConfig {
    
    // Стандартная конфигурация главного меню
    extern const char* STANDARD_MAIN_MENU_JSON;
    
    // Минимальная конфигурация для простого интерфейса
    extern const char* MINIMAL_MAIN_MENU_JSON;
    
    // Расширенная конфигурация с дополнительными функциями
    extern const char* EXTENDED_MAIN_MENU_JSON;
}

} // namespace ui
} // namespace llama_gui
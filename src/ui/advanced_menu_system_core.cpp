#include "../include/ui/advanced_menu_system.h"
#include "../external/imgui/imgui.h"
#include "../include/ui/localization_manager.h"
// #include "../include/ui/agent_chat_integration.h"  // ОТКЛЮЧЕНО: агенты временно отключены
#include <iostream>
#include <functional>

namespace llama_gui {
namespace ui {

AdvancedMenu* AdvancedMenuSystem::findMenuInternal(const std::string& menu_name) {
    auto it = menus_map_.find(menu_name);
    return (it != menus_map_.end()) ? it->second : nullptr;
}

AdvancedMenu* AdvancedMenuSystem::getMenu(const std::string& menu_name) {
    return findMenuInternal(menu_name);
}

const AdvancedMenu* AdvancedMenuSystem::getMenu(const std::string& menu_name) const {
    return const_cast<AdvancedMenuSystem*>(this)->findMenuInternal(menu_name);
}

AdvancedMenu* AdvancedMenuSystem::getMenuByKey(const std::string& menu_key) {
    for (auto& menu : menus_ordered_) {
        if (menu && menu->menu_key == menu_key) return menu.get();
    }
    return nullptr;
}

const AdvancedMenu* AdvancedMenuSystem::getMenuByKey(const std::string& menu_key) const {
    return const_cast<AdvancedMenuSystem*>(this)->getMenuByKey(menu_key);
}

void AdvancedMenuSystem::initialize(CommandManager* command_manager, WindowManager* window_manager, WorkspaceManager* workspace_manager) {
    command_manager_ = command_manager;
    window_manager_ = window_manager;
    workspace_manager_ = workspace_manager;
}

void AdvancedMenuSystem::buildModernMenu() {
    // Создаем базовые меню в правильном порядке
    auto file_menu = createFileMenu();
    auto settings_menu = createSettingsMenu();
    auto view_menu = createViewMenu();
    auto window_menu = createWindowMenu();
    auto agents_menu = createAgentsMenu();

    // Создаем меню для Developer workspace
    auto developer_menu = createDeveloperMenu();
    auto tools_menu = createToolsMenu();

    // Создаем меню для Admin workspace
    auto performance_menu = createPerformanceMenu();
    auto security_menu = createSecurityMenu();
    auto logging_menu = createLoggingMenu();

    // Создаем меню Справка (последним)
    auto help_menu = createHelpMenu();

    // Добавляем базовые меню
    if (file_menu) addMenu(std::move(file_menu));
    else std::cerr << "Warning: file_menu is null" << std::endl;
    if (settings_menu) addMenu(std::move(settings_menu));
    else std::cerr << "Warning: settings_menu is null" << std::endl;
    if (view_menu) addMenu(std::move(view_menu));
    else std::cerr << "Warning: view_menu is null" << std::endl;
    if (window_menu) addMenu(std::move(window_menu));
    else std::cerr << "Warning: window_menu is null" << std::endl;
    if (agents_menu) addMenu(std::move(agents_menu));
    else std::cerr << "Warning: agents_menu is null" << std::endl;

    // Добавляем меню для разработчика
    if (developer_menu) addMenu(std::move(developer_menu));
    else std::cerr << "Warning: developer_menu is null" << std::endl;
    if (tools_menu) addMenu(std::move(tools_menu));
    else std::cerr << "Warning: tools_menu is null" << std::endl;

    // Добавляем меню для администратора
    if (performance_menu) addMenu(std::move(performance_menu));
    else std::cerr << "Warning: performance_menu is null" << std::endl;
    if (security_menu) addMenu(std::move(security_menu));
    else std::cerr << "Warning: security_menu is null" << std::endl;
    if (logging_menu) addMenu(std::move(logging_menu));
    else std::cerr << "Warning: logging_menu is null" << std::endl;

    // Добавляем меню Справка (всегда последнее)
    if (help_menu) addMenu(std::move(help_menu));
    else std::cerr << "Warning: help_menu is null" << std::endl;

    // Добавляем callback для обновления меню при смене workspace
    // (только один раз, даже если меню перестраиваются при смене языка)
    if (workspace_manager_ && !workspace_callback_registered_) {
        workspace_callback_registered_ = true;
        workspace_manager_->addWorkspaceChangedCallback([this](WorkspaceType type) {
            updateMenuVisibilityForWorkspace(type);
        });
    }

    std::cout << "✓ Modern menu built (" << menus_ordered_.size() << " menus)" << std::endl;
}

void AdvancedMenuSystem::rebuildModernMenu() {
    std::cout << "Rebuilding menu for language change..." << std::endl;

    // Сохраняем текущие состояния workspace
    WorkspaceType current_workspace = WorkspaceType::User;
    if (workspace_manager_) {
        current_workspace = workspace_manager_->getCurrentWorkspaceType();
    } else {
        std::cerr << "Warning: workspace_manager_ is null, using default User workspace" << std::endl;
    }

    // Очищаем все меню
    menus_ordered_.clear();
    menus_map_.clear();

    // Перестраиваем меню с новыми переводами
    buildModernMenu();

    // Восстанавливаем видимость меню для текущего workspace
    updateMenuVisibilityForWorkspace(current_workspace);

    std::cout << "✓ Menu rebuilt for language change" << std::endl;
}

bool AdvancedMenuSystem::addMenu(std::unique_ptr<AdvancedMenu> menu) {
    if (!menu) {
        std::cerr << "Cannot add null menu" << std::endl;
        return false;
    }

    std::string menu_name = menu->name;
    menus_map_[menu_name] = menu.get();
    menus_ordered_.push_back(std::move(menu));
    return true;
}

bool AdvancedMenuSystem::addMenu(const std::string& name, std::vector<AdvancedMenuItem> items) {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->name = name;
    menu->items = std::move(items);
    return addMenu(std::move(menu));
}

bool AdvancedMenuSystem::addMenuItem(const std::string& menu_name, const AdvancedMenuItem& item) {
    // Сначала ищем по стабильному ключу (устойчив к локализации имени),
    // затем по локализованному имени.
    AdvancedMenu* menu = getMenuByKey(menu_name);
    if (!menu) {
        auto it = menus_map_.find(menu_name);
        if (it == menus_map_.end()) {
            std::cerr << "Menu not found: " << menu_name << std::endl;
            return false;
        }
        menu = it->second;
    }

    menu->items.push_back(item);
    std::cout << "✓ Added menu item: " << item.name << " to menu: " << menu_name << std::endl;
    return true;
}

bool AdvancedMenuSystem::addSeparator(const std::string& menu_name) {
    AdvancedMenuItem separator;
    separator.type = AdvancedMenuItemType::Separator;
    return addMenuItem(menu_name, separator);
}

bool AdvancedMenuSystem::removeMenu(const std::string& menu_name) {
    auto it = menus_map_.find(menu_name);
    if (it == menus_map_.end()) {
        return false;
    }

    // Удаляем из map
    menus_map_.erase(it);
    
    // Удаляем из ordered списка
    auto ordered_it = std::find_if(menus_ordered_.begin(), menus_ordered_.end(),
        [&menu_name](const std::unique_ptr<AdvancedMenu>& menu) {
            return menu->name == menu_name;
        });
    
    if (ordered_it != menus_ordered_.end()) {
        menus_ordered_.erase(ordered_it);
    }
    
    std::cout << "✓ Removed menu: " << menu_name << std::endl;
    return true;
}

bool AdvancedMenuSystem::removeMenuItem(const std::string& menu_name, const std::string& item_name) {
    auto it = menus_map_.find(menu_name);
    if (it == menus_map_.end()) {
        return false;
    }

    auto& items = it->second->items;
    auto item_it = std::find_if(items.begin(), items.end(),
        [&item_name](const AdvancedMenuItem& item) {
            return item.name == item_name;
        });

    if (item_it == items.end()) {
        return false;
    }

    items.erase(item_it);
    std::cout << "✓ Removed menu item: " << item_name << " from menu: " << menu_name << std::endl;
    return true;
}

void AdvancedMenuSystem::updateMenuStates() {
    // Update menu states based on current application state
    // Список окон обновляется динамически (окна могут регистрироваться после построения меню)
    refreshWindowMenu();

    for (auto& menu : menus_ordered_) {
        for (auto& item : menu->items) {
            updateMenuItemState(item);

            // Обновляем и элементы подменю
            if (item.type == AdvancedMenuItemType::Submenu) {
                for (auto& sub_item : item.submenu_items) {
                    updateMenuItemState(sub_item);
                }
            }
        }
    }
}

void AdvancedMenuSystem::updateMenuItemState(AdvancedMenuItem& item) {
    // Update window toggle items
    if (item.is_window_toggle && !item.window_name.empty() && window_manager_) {
        item.checked = window_manager_->isWindowVisible(item.window_name);
    }

    // Отключаем элементы, чьи команды являются заглушками или не зарегистрированы.
    // Заглушки отображаются серым (неактивными), но остаются видимыми.
    if (!item.command.empty()) {
        if (!command_manager_ || !command_manager_->isCommandAvailable(item.command)) {
            item.enabled = false;
        }
    }
}

void AdvancedMenuSystem::setMenuItemEnabled(const std::string& menu_name, const std::string& item_name, bool enabled) {
    auto* menu = findMenuInternal(menu_name);
    if (menu) {
        for (auto& item : menu->items) {
            if (item.name == item_name) {
                item.enabled = enabled;
                return;
            }
        }
    }
}

void AdvancedMenuSystem::setMenuEnabled(const std::string& menu_name, bool enabled) {
    auto* menu = findMenuInternal(menu_name);
    if (menu) {
        menu->enabled = enabled;
    }
}

void AdvancedMenuSystem::setMenuItemChecked(const std::string& menu_name, const std::string& item_name, bool checked) {
    auto* menu = findMenuInternal(menu_name);
    if (menu) {
        for (auto& item : menu->items) {
            if (item.name == item_name) {
                item.checked = checked;
                return;
            }
        }
    }
}

bool AdvancedMenuSystem::isMenuItemChecked(const std::string& menu_name, const std::string& item_name) const {
    auto* menu = const_cast<AdvancedMenuSystem*>(this)->findMenuInternal(menu_name);
    if (menu) {
        for (const auto& item : menu->items) {
            if (item.name == item_name) {
                return item.checked;
            }
        }
    }
    return false;
}

// Фабричные методы создания меню перенесены в advanced_menu_system_factories.cpp

namespace AdvancedMenuItemFactory {

AdvancedMenuItem createCommandItem(const std::string& name, const std::string& command,
                                  const std::string& shortcut, const std::string& tooltip) {
    AdvancedMenuItem item;
    item.name = name;
    item.command = command;
    item.shortcut = shortcut;
    item.tooltip = tooltip;
    item.enabled = true;
    item.type = AdvancedMenuItemType::Item;
    return item;
}

AdvancedMenuItem createSeparator() {
    AdvancedMenuItem item;
    item.name = "";
    item.type = AdvancedMenuItemType::Separator;
    item.separator = true;
    return item;
}

AdvancedMenuItem createCheckboxItem(const std::string& name, const std::string& command,
                                   std::function<bool()> check_func, std::function<void(bool)> toggle_func,
                                   const std::string& shortcut, const std::string& tooltip) {
    AdvancedMenuItem item = createCommandItem(name, command, shortcut, tooltip);
    item.check_func = std::move(check_func);
    item.toggle_func = std::move(toggle_func);
    return item;
}

AdvancedMenuItem createWindowToggleItem(const std::string& name, const std::string& window_name,
                                       const std::string& shortcut, const std::string& tooltip) {
    AdvancedMenuItem item = createCommandItem(name, "toggle_window_" + window_name, shortcut, tooltip);
    item.window_name = window_name;
    item.is_window_toggle = true;
    return item;
}

} // namespace AdvancedMenuItemFactory

// =========================================================================
// Workspace Manager методы
// =========================================================================

void AdvancedMenuSystem::setWorkspaceManager(WorkspaceManager* workspace_manager) {
    workspace_manager_ = workspace_manager;
}

void AdvancedMenuSystem::switchToWorkspace(WorkspaceType type) {
    std::cout << "[MENU] switchToWorkspace called: " << workspace_type_to_string(type) << std::endl;
    if (workspace_manager_) {
        workspace_manager_->switchWorkspace(type);
        std::cout << "[MENU] switchToWorkspace completed" << std::endl;
    }
}

void AdvancedMenuSystem::switchToWorkspace(const std::string& name) {
    std::cout << "[MENU] switchToWorkspace(string) called: " << name << std::endl;
    if (workspace_manager_) {
        workspace_manager_->switchWorkspace(name);
        std::cout << "[MENU] switchToWorkspace completed" << std::endl;
    }
}

void AdvancedMenuSystem::updateMenuVisibilityForWorkspace(WorkspaceType type) {
    if (!workspace_manager_) return;

    const auto& config = workspace_manager_->getCurrentWorkspace();
    std::cout << "[MENU] updateMenuVisibilityForWorkspace: " << config.name << std::endl;

    // Если workspace не инициализирован (нет ни одной конфигурации меню) —
    // показываем все меню. Иначе при пересборке меню (смена языка) все пункты
    // кроме Window/Workspace были бы скрыты, т.к. isMenuVisible() возвращает false.
    if (config.menu_configs.empty()) {
        for (auto& menu : menus_ordered_) {
            menu->enabled = true;
        }
        return;
    }

    // Скрываем/показываем меню в зависимости от workspace
    for (auto& menu : menus_ordered_) {
        // Меню Workspace и Window всегда видимы для переключения
        if (menu->menu_key == "Workspace" || menu->menu_key == "Window") {
            menu->enabled = true;
            continue;
        }

        bool visible = workspace_manager_->isMenuVisible(menu->menu_key);
        menu->enabled = visible;
    }
}

// Фабричные методы создания меню перенесены в advanced_menu_system_factories.cpp

} // namespace ui
} // namespace llama_gui

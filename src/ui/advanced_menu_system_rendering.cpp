#include "../include/ui/advanced_menu_system.h"
#include "../include/ui/language_selector.h"
#include "../external/imgui/imgui.h"
#include "../include/ui/localization_manager.h"
#include <iostream>
#include <string>

namespace llama_gui {
namespace ui {

void AdvancedMenuSystem::renderMainMenu() {
    // Update menu states before rendering
    updateMenuStates();

    bool main_menu_bar = ImGui::BeginMainMenuBar();

    if (main_menu_bar) {
        // Render all menus in order
        for (const auto& menu : menus_ordered_) {
            if (menu && menu->enabled) {
                if (ImGui::BeginMenu(menu->name.c_str())) {
                    renderMenuItems(menu->items);
                    ImGui::EndMenu();
                }
            }
        }

        ImGui::EndMainMenuBar();
    }
}

void AdvancedMenuSystem::renderMenuItems(const std::vector<AdvancedMenuItem>& items) {
    for (const auto& item : items) {
        if (!item.enabled) {
            continue;
        }

        // Проверяем доступность команды для текущего workspace
        if (workspace_manager_ && !item.command.empty()) {
            if (!workspace_manager_->isCommandEnabled(item.command)) {
                continue; // Скрываем элементы с отключенными командами
            }
        }

        switch (item.type) {
            case AdvancedMenuItemType::Separator:
                ImGui::Separator();
                break;

            case AdvancedMenuItemType::Item:
                if (ImGui::MenuItem(item.name.c_str(), item.shortcut.c_str(), item.checked, item.enabled)) {
                    if (item.callback) {
                        item.callback();
                    } else if (!item.command.empty() && command_manager_) {
                        auto result = command_manager_->executeCommand(item.command);
                        if (!result.success) {
                            std::cerr << "[AdvancedMenuSystem] Error: " << result.error << std::endl;
                        }
                    }
                }
                break;

            case AdvancedMenuItemType::Submenu:
                // Проверяем, есть ли в подменю хотя бы один доступный элемент
                bool has_visible_items = false;
                for (const auto& sub_item : item.submenu_items) {
                    if (sub_item.enabled && (sub_item.command.empty() || workspace_manager_->isCommandEnabled(sub_item.command))) {
                        has_visible_items = true;
                        break;
                    }
                }
                
                if (has_visible_items) {
                    if (ImGui::BeginMenu(item.name.c_str())) {
                        renderMenuItems(item.submenu_items);
                        ImGui::EndMenu();
                    }
                }
                break;
        }

        // Show tooltip if available
        if (ImGui::IsItemHovered() && !item.tooltip.empty()) {
            ImGui::SetTooltip("%s", item.tooltip.c_str());
        }
    }
}

void AdvancedMenuSystem::handleKeyboardShortcuts() {
    // Handle keyboard shortcuts for menu items
    for (auto& menu : menus_ordered_) {
        for (const auto& item : menu->items) {
            if (!item.enabled || item.shortcut.empty()) {
                continue;
            }

            // Check if the shortcut key combination is pressed
            // This is a simplified implementation
            // In a real implementation, you'd parse the shortcut string
            // and check the actual key state
        }
    }

    // Горячая клавиша Ctrl+L для переключения языка
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L)) {
        auto& lang_mgr = getLocalizationManager();
        Language current = lang_mgr.getCurrentLanguage();
        if (current == Language::Russian) {
            lang_mgr.setCurrentLanguage(Language::English);
        } else {
            lang_mgr.setCurrentLanguage(Language::Russian);
        }
    }
}

} // namespace ui
} // namespace llama_gui

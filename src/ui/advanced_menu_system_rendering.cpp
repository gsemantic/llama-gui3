#include "../include/ui/advanced_menu_system.h"
#include "../include/ui/language_selector.h"
#include "../external/imgui/imgui.h"
#include "../include/ui/localization_manager.h"
#include <algorithm>
#include <iostream>
#include <iterator>
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

        // Language selector at the right side of the menu bar
        renderLanguageSelector();

        ImGui::EndMainMenuBar();
    }
}

void AdvancedMenuSystem::renderLanguageSelector() {
#ifdef USE_IMGUI
    auto& loc = getLocalizationManager();
    std::vector<LanguageInfo> languages = loc.getLanguageInfos();
    if (languages.size() < 2) return;

    std::string current_code = loc.getCurrentLanguageCode();
    std::string current_name = loc.getLanguageDisplayName(current_code);

    // Render as a regular submenu item, exactly like the other menu bar entries.
    ImGui::Separator();
    if (ImGui::BeginMenu(current_name.c_str())) {
        for (const auto& lang : languages) {
            const bool is_selected = (lang.code == current_code);
            if (ImGui::MenuItem(lang.display_name.c_str(), nullptr, is_selected)) {
                loc.setCurrentLanguage(lang.code);
            }
        }
        ImGui::EndMenu();
    }
#endif
}

void AdvancedMenuSystem::renderMenuItems(const std::vector<AdvancedMenuItem>& items) {
    for (const auto& item : items) {
        // Проверяем доступность команды для текущего workspace.
        // Отключенные workspace команды скрываем, а заглушки (stubs) показываем серым.
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

            case AdvancedMenuItemType::Submenu: {
                // Проверяем, есть ли в подменю хотя бы один видимый элемент
                bool has_visible_items = false;
                for (const auto& sub_item : item.submenu_items) {
                    if (sub_item.type == AdvancedMenuItemType::Separator) {
                        continue;
                    }
                    // Скрытые workspace команды не считаются видимыми
                    if (workspace_manager_ && !sub_item.command.empty() &&
                        !workspace_manager_->isCommandEnabled(sub_item.command)) {
                        continue;
                    }
                    has_visible_items = true;
                    break;
                }
                
                if (has_visible_items) {
                    if (ImGui::BeginMenu(item.name.c_str(), item.enabled)) {
                        renderMenuItems(item.submenu_items);
                        ImGui::EndMenu();
                    }
                }
                break;
            }
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
        std::vector<std::string> codes = lang_mgr.getAvailableLanguageCodes();
        if (codes.size() > 1) {
            std::string current = lang_mgr.getCurrentLanguageCode();
            auto it = std::find(codes.begin(), codes.end(), current);
            size_t idx = (it == codes.end()) ? 0 : (static_cast<size_t>(std::distance(codes.begin(), it)) + 1) % codes.size();
            lang_mgr.setCurrentLanguage(codes[idx]);
        }
    }
}

} // namespace ui
} // namespace llama_gui

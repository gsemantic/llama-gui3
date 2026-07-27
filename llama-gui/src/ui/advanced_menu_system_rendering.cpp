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

    if (ImGui::BeginMainMenuBar()) {
        // Итерируемся по упорядоченному списку для сохранения порядка меню
        for (auto& menu : menus_ordered_) {
            if (!menu->enabled) {
                continue;
            }

            // Use translated name if available
            std::string menu_name = TR(menu->name.c_str());
            if (ImGui::BeginMenu(menu_name.c_str())) {
                renderMenuItems(menu->items);

                ImGui::EndMenu();
            }
        }

        // Добавляем меню выбора активного режима (Пользователь/Разработчик/Администратор)
        if (workspace_manager_) {
            auto current_type = workspace_manager_->getCurrentWorkspaceType();
            
            // Определяем название текущего режима для отображения в меню
            // Используем std::string для корректного хранения результата перевода
            std::string current_mode_name = std::string(TR("mode.prefix")) + " " + TR("mode.user");
            if (current_type == WorkspaceType::Developer) {
                current_mode_name = std::string(TR("mode.prefix")) + " " + TR("mode.developer");
            } else if (current_type == WorkspaceType::Admin) {
                current_mode_name = std::string(TR("mode.prefix")) + " " + TR("mode.admin");
            }

            if (ImGui::BeginMenu(current_mode_name.c_str())) {
                // Переключатель режима через MenuItem
                if (ImGui::MenuItem(TR("mode.user"), "", current_type == WorkspaceType::User)) {
                    switchToWorkspace(WorkspaceType::User);
                }
                if (ImGui::MenuItem(TR("mode.developer"), "", current_type == WorkspaceType::Developer)) {
                    switchToWorkspace(WorkspaceType::Developer);
                }
                if (ImGui::MenuItem(TR("mode.admin"), "", current_type == WorkspaceType::Admin)) {
                    switchToWorkspace(WorkspaceType::Admin);
                }

                ImGui::Separator();

                // Информация о текущем режиме
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", TR("mode.active"));
                switch (current_type) {
                    case WorkspaceType::User:
                        ImGui::TextWrapped("%s", TR("mode.user_desc"));
                        break;
                    case WorkspaceType::Developer:
                        ImGui::TextWrapped("%s", TR("mode.developer_desc"));
                        break;
                    case WorkspaceType::Admin:
                        ImGui::TextWrapped("%s", TR("mode.admin_desc"));
                        break;
                }

                ImGui::EndMenu();
            }
        }

        // Добавляем переключатель языка в правую часть меню
        renderLanguageSelector();

        ImGui::EndMainMenuBar();
    }
}

void AdvancedMenuSystem::renderLanguageSelector() {
    // Переключатель языка в правой части меню
    ImGui::Separator();

    // Компактный переключатель без иконки - создаём локальный объект
    LanguageSelector language_selector;
    if (ImGui::BeginMenu(TR("language.selector.label"))) {
        language_selector.renderComboBox("##language");
        ImGui::EndMenu();
    }
}

bool AdvancedMenuSystem::renderContextMenu(const std::string& menu_name, const ImVec2& position) {
    auto it = menus_map_.find(menu_name);
    if (it == menus_map_.end() || !it->second->enabled) {
        return false;
    }

    ImGui::SetNextWindowPos(position);
    if (ImGui::BeginPopupContextItem(menu_name.c_str())) {
        renderMenuItems(it->second->items);
        ImGui::EndPopup();
        return true;
    }
    return false;
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
                        command_manager_->executeCommand(item.command);
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

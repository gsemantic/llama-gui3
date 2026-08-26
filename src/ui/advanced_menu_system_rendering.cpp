#include "../include/ui/advanced_menu_system.h"
#include "../include/ui/language_selector.h"
#include "../external/imgui/imgui.h"
#include "../include/ui/localization_manager.h"
#include <algorithm>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace llama_gui {
namespace ui {

void AdvancedMenuSystem::renderMainMenu() {
    // Update menu states before rendering
    updateMenuStates();

    bool main_menu_bar = ImGui::BeginMainMenuBar();

    if (main_menu_bar) {
        // Render all menus in order
        for (const auto& menu : menus_ordered_) {
            if (!menu || !menu->enabled) continue;

            // Стабильный ключ меню для пользовательской раскладки
            // (плагины могут добавлять меню без menu_key — используем имя)
            const std::string menu_key = !menu->menu_key.empty() ? menu->menu_key : menu->name;

            // Скрытое пользователем меню не рендерим вовсе
            if (menu_layout_manager_.isMenuHidden(menu_key)) continue;

            if (ImGui::BeginMenu(menu->name.c_str())) {
                renderMenuItems(menu_key, menu->items);
                ImGui::EndMenu();
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
    // Вызов без контекста меню: пользовательская раскладка не применяется
    renderMenuItems(std::string(), items);
}

void AdvancedMenuSystem::renderMenuItems(const std::string& menu_key,
                                         const std::vector<AdvancedMenuItem>& items) {
    const MenuLayoutForMenu* layout =
        menu_key.empty() ? nullptr : menu_layout_manager_.findMenuLayout(menu_key);
    const bool use_entries = layout && !layout->entries.empty();

    // Видимость пункта: отключенные workspace-команды скрываются,
    // скрытые пользователем (раскладка) тоже
    auto item_visible = [&](const AdvancedMenuItem& it) -> bool {
        if (workspace_manager_ && !it.command.empty() &&
            !workspace_manager_->isCommandEnabled(it.command)) {
            return false;
        }
        if (layout) {
            const std::string key = make_menu_item_key(it.command, it.name);
            if (!key.empty() && menu_layout_manager_.isItemHidden(menu_key, key)) {
                return false;
            }
        }
        return true;
    };

    // Отрисовка одного элемента (без управления разделителями)
    auto draw_item = [&](const AdvancedMenuItem& it) {
        switch (it.type) {
            case AdvancedMenuItemType::Separator:
                ImGui::Separator();
                break;

            case AdvancedMenuItemType::Item: {
                // Уникальный ImGui-ID для пункта: ImGui использует текст пункта
                // как ID, поэтому пункты с одинаковым именем в одном меню (например
                // "RAG Search" у приложения и у плагина) давали бы ID collision.
                // Часть после "##" не отображается, поэтому текст пункта не меняется.
                const std::string label =
                    it.command.empty() ? it.name : (it.name + "##" + it.command);
                if (ImGui::MenuItem(label.c_str(), it.shortcut.c_str(), it.checked, it.enabled)) {
                    if (it.callback) {
                        it.callback();
                    } else if (!it.command.empty() && command_manager_) {
                        auto result = command_manager_->executeCommand(it.command);
                        if (!result.success) {
                            std::cerr << "[AdvancedMenuSystem] Error: " << result.error << std::endl;
                        }
                    }
                }
                break;
            }

            case AdvancedMenuItemType::Submenu: {
                // Проверяем, есть ли в подменю хотя бы один видимый элемент
                bool has_visible_items = false;
                for (const auto& sub_item : it.submenu_items) {
                    if (sub_item.type == AdvancedMenuItemType::Separator) {
                        continue;
                    }
                    if (!item_visible(sub_item)) {
                        continue;
                    }
                    has_visible_items = true;
                    break;
                }

                if (has_visible_items) {
                    if (ImGui::BeginMenu(it.name.c_str(), it.enabled)) {
                        renderMenuItems(menu_key, it.submenu_items);
                        ImGui::EndMenu();
                    }
                }
                break;
            }
        }

        // Show tooltip if available
        if (ImGui::IsItemHovered() && !it.tooltip.empty()) {
            ImGui::SetTooltip("%s", it.tooltip.c_str());
        }
    };

    // ------------------------------------------------------------------
    // Без записей порядка — прежнее поведение + фильтрация скрытых
    // ------------------------------------------------------------------
    if (!use_entries) {
        for (const auto& it : items) {
            if (it.type != AdvancedMenuItemType::Separator && !item_visible(it)) continue;
            draw_item(it);
        }
        return;
    }

    // ------------------------------------------------------------------
    // Режим раскладки: порядок из entries, группы как подменю,
    // неупомянутые элементы идут в хвосте в исходном порядке
    // ------------------------------------------------------------------
    std::unordered_map<std::string, const AdvancedMenuItem*> by_key;
    for (const auto& it : items) {
        if (it.type == AdvancedMenuItemType::Separator) continue;
        by_key[make_menu_item_key(it.command, it.name)] = &it;
    }
    std::unordered_set<const AdvancedMenuItem*> consumed;  // Позиция задана entries/группой

    bool drawn_any = false;   // На текущем уровне что-то уже нарисовано
    bool pending_sep = false; // Встречен разделитель — рисуем перед следующим элементом

    auto sep_before_next = [&]() {
        if (drawn_any && pending_sep) {
            ImGui::Separator();
        }
        pending_sep = false;
    };

    auto draw_with_sep = [&](const AdvancedMenuItem& it) {
        if (!item_visible(it)) return;
        sep_before_next();
        draw_item(it);
        drawn_any = true;
    };

    auto draw_group = [&](const MenuLayoutGroup& group) {
        // Пустая группа (всё скрыто или пункты исчезли) не рендерится
        bool any_visible = false;
        for (const auto& k : group.item_keys) {
            auto found = by_key.find(k);
            if (found != by_key.end() && item_visible(*found->second)) {
                any_visible = true;
                break;
            }
        }
        if (!any_visible) return;

        sep_before_next();
        if (ImGui::BeginMenu(group.title.c_str())) {
            for (const auto& k : group.item_keys) {
                auto found = by_key.find(k);
                if (found == by_key.end()) continue;
                if (!item_visible(*found->second)) continue;
                consumed.insert(found->second);
                draw_item(*found->second);
            }
            ImGui::EndMenu();
        }
        drawn_any = true;
    };

    for (const auto& entry : layout->entries) {
        if (entry.is_group) {
            for (const auto& group : layout->groups) {
                if (group.id == entry.key) {
                    draw_group(group);
                    break;
                }
            }
        } else {
            auto found = by_key.find(entry.key);
            if (found != by_key.end()) {
                consumed.insert(found->second);
                draw_with_sep(*found->second);
            }
        }
    }

    // Хвост: элементы вне раскладки и исходные разделители между ними.
    // Разделители схлопываются и не рисуются по краям списка.
    for (const auto& it : items) {
        if (it.type == AdvancedMenuItemType::Separator) {
            if (drawn_any) pending_sep = true;
            continue;
        }
        if (consumed.count(&it) > 0) continue;
        draw_with_sep(it);
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

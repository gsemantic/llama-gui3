#include "../include/ui/menu_layout_editor_dialog.h"
#include "../include/ui/advanced_menu_system.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui.h"
#include <algorithm>
#include <cstdio>

namespace llama_gui {
namespace ui {

void MenuLayoutEditorDialog::selectMenu(const std::string& menu_key) {
    current_menu_key_ = menu_key;
    target_group_ = -1;
    new_group_title_[0] = '\0';
    rebuildModel();
}

void MenuLayoutEditorDialog::rebuildModel() {
    nodes_.clear();
    groups_.clear();
    item_names_.clear();
    existing_keys_.clear();
    target_group_ = -1;

    if (!menu_system_ || current_menu_key_.empty()) return;

    const AdvancedMenu* menu = menu_system_->getMenuByKey(current_menu_key_);
    if (!menu) menu = menu_system_->getMenu(current_menu_key_);
    if (!menu) return;

    // Ключи и отображаемые имена пунктов верхнего уровня (в исходном порядке)
    std::vector<std::string> ordered_keys;
    for (const auto& item : menu->items) {
        if (item.type == AdvancedMenuItemType::Separator) continue;
        const std::string key = make_menu_item_key(item.command, item.name);
        if (key.empty() || existing_keys_.count(key) > 0) continue;
        existing_keys_.insert(key);
        item_names_[key] = item.name;
        ordered_keys.push_back(key);
    }

    auto& mgr = menu_system_->getMenuLayoutManager();
    const MenuLayoutForMenu* layout = mgr.findMenuLayout(current_menu_key_);

    std::unordered_set<std::string> placed;
    if (layout) {
        std::unordered_map<std::string, const MenuLayoutGroup*> by_id;
        for (const auto& g : layout->groups) by_id[g.id] = &g;

        for (const auto& entry : layout->entries) {
            if (entry.is_group) {
                auto git = by_id.find(entry.key);
                if (git == by_id.end()) continue;

                EditorGroup eg;
                eg.id = git->second->id;
                eg.title = git->second->title;
                // Ссылки на исчезнувшие пункты молча отбрасываем
                for (const auto& k : git->second->item_keys) {
                    if (existing_keys_.count(k) > 0) eg.item_keys.push_back(k);
                }
                for (const auto& k : eg.item_keys) placed.insert(k);

                EditorNode node;
                node.is_group = true;
                node.group_index = static_cast<int>(groups_.size());
                groups_.push_back(std::move(eg));
                nodes_.push_back(node);
            } else if (existing_keys_.count(entry.key) > 0 && placed.count(entry.key) == 0) {
                EditorNode node;
                node.is_group = false;
                node.key = entry.key;
                nodes_.push_back(node);
                placed.insert(entry.key);
            }
        }
    }

    // Неупомянутые в раскладке пункты — в хвосте в исходном порядке
    for (const auto& key : ordered_keys) {
        if (placed.count(key) > 0) continue;
        EditorNode node;
        node.is_group = false;
        node.key = key;
        nodes_.push_back(node);
    }
}

void MenuLayoutEditorDialog::commit() {
    if (!menu_system_ || current_menu_key_.empty()) return;

    auto& mgr = menu_system_->getMenuLayoutManager();

    // Порядок верхнего уровня
    std::vector<MenuLayoutEntry> entries;
    entries.reserve(nodes_.size());
    std::unordered_set<std::string> present_groups;
    for (const auto& node : nodes_) {
        if (!node.is_group) {
            MenuLayoutEntry e;
            e.is_group = false;
            e.key = node.key;
            entries.push_back(e);
        } else if (node.group_index >= 0 &&
                   node.group_index < static_cast<int>(groups_.size())) {
            const auto& g = groups_[static_cast<size_t>(node.group_index)];
            MenuLayoutEntry e;
            e.is_group = true;
            e.key = g.id;
            entries.push_back(e);
            present_groups.insert(g.id);
        }
    }
    mgr.setEntries(current_menu_key_, entries);

    // Содержимое групп
    for (const auto& g : groups_) {
        MenuLayoutGroup mg;
        mg.id = g.id;
        mg.title = g.title.empty() ? g.id : g.title;
        mg.item_keys = g.item_keys;
        mgr.setGroup(current_menu_key_, mg);
    }

    // Группы, исчезнувшие из модели, удаляем из раскладки
    std::vector<std::string> stale;
    if (const MenuLayoutForMenu* layout = mgr.findMenuLayout(current_menu_key_)) {
        for (const auto& g : layout->groups) {
            if (present_groups.count(g.id) == 0) stale.push_back(g.id);
        }
    }
    for (const auto& id : stale) {
        mgr.removeGroup(current_menu_key_, id);
    }

    mgr.save();
}

void MenuLayoutEditorDialog::render(bool* open) {
    if (!menu_system_) return;

    if (!ImGui::Begin(TRF("menu.layout_editor.title", "Настройка меню"), open)) {
        ImGui::End();
        if (open && !*open) show_ = false;
        return;
    }

    auto& mgr = menu_system_->getMenuLayoutManager();

    // ------------------------------------------------------------------
    // Выбор меню
    // ------------------------------------------------------------------
    const std::vector<std::string> menu_keys = menu_system_->getAllMenuNames();
    if (menu_keys.empty()) {
        ImGui::TextUnformatted(TRF("menu.layout_editor.no_menus", "Меню не построены"));
        ImGui::End();
        return;
    }

    // Текущий выбор может исчезнуть (выгрузка плагина) — восстанавливаемся
    bool current_exists =
        !current_menu_key_.empty() &&
        std::find(menu_keys.begin(), menu_keys.end(), current_menu_key_) != menu_keys.end();
    if (!current_exists) {
        selectMenu(menu_keys.front());
    }

    if (ImGui::BeginCombo("##menu_selector", current_menu_key_.c_str())) {
        for (const auto& key : menu_keys) {
            const AdvancedMenu* m = menu_system_->getMenuByKey(key);
            std::string label = key;
            if (m && !m->name.empty()) label = m->name + " (" + key + ")";
            bool selected = (key == current_menu_key_);
            if (ImGui::Selectable(label.c_str(), selected)) {
                selectMenu(key);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    // Меню View не даём скрывать: через него открывается сам редактор
    bool view_locked = (current_menu_key_ == "View");
    bool menu_hidden = mgr.isMenuHidden(current_menu_key_);
    if (view_locked) {
        ImGui::TextUnformatted(TRF("menu.layout_editor.view_protected",
                                   "(меню View защищено от скрытия)"));
    } else if (ImGui::Checkbox(TRF("menu.layout_editor.hide_menu", "Скрыть меню целиком"),
                               &menu_hidden)) {
        mgr.setMenuHidden(current_menu_key_, menu_hidden);
        mgr.save();
    }

    ImGui::Separator();

    // ------------------------------------------------------------------
    // Список пунктов и групп текущего меню.
    // Любое структурное изменение помечает restart: стеки ImGui корректно
    // закрываются, список перерисовывается следующим кадром.
    // ------------------------------------------------------------------
    bool restart = false;

    ImGui::BeginChild("##rows", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 5),
                      ImGuiChildFlags_Borders);

    for (size_t i = 0; i < nodes_.size() && !restart; ++i) {
        EditorNode& node = nodes_[i];
        ImGui::PushID(static_cast<int>(i));

        if (!node.is_group) {
            // ----- Обычный пункт -----
            const std::string key = node.key;

            bool visible = !mgr.isItemHidden(current_menu_key_, key);
            if (ImGui::Checkbox("##vis", &visible)) {
                mgr.setItemHidden(current_menu_key_, key, !visible);
                commit();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", TRF("menu.layout_editor.visible_hint",
                                            "Показывать этот пункт"));
            }

            ImGui::SameLine();
            auto name_it = item_names_.find(key);
            ImGui::TextUnformatted(name_it != item_names_.end()
                                       ? name_it->second.c_str() : key.c_str());

            ImGui::SameLine();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("^")) {
                std::swap(nodes_[i], nodes_[i - 1]);
                commit();
                restart = true;
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 >= nodes_.size());
            if (ImGui::SmallButton("v")) {
                std::swap(nodes_[i], nodes_[i + 1]);
                commit();
                restart = true;
            }
            ImGui::EndDisabled();

            if (!groups_.empty() && target_group_ >= 0 && !restart) {
                ImGui::SameLine();
                if (ImGui::SmallButton(TRF("menu.layout_editor.to_group", "в группу"))) {
                    groups_[static_cast<size_t>(target_group_)].item_keys.push_back(key);
                    nodes_.erase(nodes_.begin() + static_cast<long>(i));
                    commit();
                    restart = true;
                }
            }
        } else if (node.group_index >= 0 &&
                   node.group_index < static_cast<int>(groups_.size())) {
            // ----- Группа -----
            const size_t gi = static_cast<size_t>(node.group_index);
            EditorGroup& group = groups_[gi];

            bool tree_open =
                ImGui::TreeNodeEx("##group", ImGuiTreeNodeFlags_DefaultOpen, "%s",
                                  group.title.c_str());

            ImGui::SameLine();
            if (ImGui::SmallButton(TRF("menu.layout_editor.delete", "удалить"))) {
                // Пункты группы возвращаются на верхний уровень на её место
                std::vector<EditorNode> replacement;
                for (const auto& k : group.item_keys) {
                    if (existing_keys_.count(k) == 0) continue;
                    EditorNode n;
                    n.is_group = false;
                    n.key = k;
                    replacement.push_back(n);
                }
                int removed_index = node.group_index;
                nodes_.erase(nodes_.begin() + static_cast<long>(i));
                nodes_.insert(nodes_.begin() + static_cast<long>(i),
                              replacement.begin(), replacement.end());
                groups_.erase(groups_.begin() + static_cast<long>(gi));
                for (auto& n : nodes_) {
                    if (n.is_group && n.group_index > removed_index) n.group_index--;
                }
                if (target_group_ >= static_cast<int>(groups_.size())) target_group_ = -1;
                commit();
                restart = true;
            }

            if (tree_open) {
                char title_buf[128];
                std::snprintf(title_buf, sizeof(title_buf), "%s", group.title.c_str());
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::InputText("##title", title_buf, sizeof(title_buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    group.title = title_buf;
                    commit();
                }

                for (size_t j = 0; j < group.item_keys.size() && !restart; ++j) {
                    ImGui::PushID(static_cast<int>(j));
                    const std::string key = group.item_keys[j];

                    bool visible = !mgr.isItemHidden(current_menu_key_, key);
                    if (ImGui::Checkbox("##gvis", &visible)) {
                        mgr.setItemHidden(current_menu_key_, key, !visible);
                        commit();
                    }

                    ImGui::SameLine();
                    auto name_it = item_names_.find(key);
                    ImGui::TextUnformatted(name_it != item_names_.end()
                                               ? name_it->second.c_str() : key.c_str());

                    ImGui::SameLine();
                    ImGui::BeginDisabled(j == 0);
                    if (ImGui::SmallButton("^")) {
                        std::swap(group.item_keys[j], group.item_keys[j - 1]);
                        commit();
                        restart = true;
                    }
                    ImGui::EndDisabled();

                    ImGui::SameLine();
                    ImGui::BeginDisabled(j + 1 >= group.item_keys.size());
                    if (ImGui::SmallButton("v")) {
                        std::swap(group.item_keys[j], group.item_keys[j + 1]);
                        commit();
                        restart = true;
                    }
                    ImGui::EndDisabled();

                    // Из группы обратно на верхний уровень (сразу после группы)
                    if (!restart) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(
                                TRF("menu.layout_editor.from_group", "из группы"))) {
                            group.item_keys.erase(group.item_keys.begin()
                                                  + static_cast<long>(j));
                            EditorNode n;
                            n.is_group = false;
                            n.key = key;
                            nodes_.insert(nodes_.begin() + static_cast<long>(i) + 1, n);
                            commit();
                            restart = true;
                        }
                    }

                    ImGui::PopID();
                }

                ImGui::TreePop();
            }
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    // ------------------------------------------------------------------
    // Создание группы и выбор целевой группы
    // ------------------------------------------------------------------
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##new_group_title",
                             TRF("menu.layout_editor.new_group_hint",
                                 "Заголовок новой группы"), new_group_title_,
                             sizeof(new_group_title_));
    ImGui::SameLine();
    if (ImGui::Button(TRF("menu.layout_editor.create_group", "Создать группу"))) {
        // Уникальный id во всей конфигурации: сканируем модель и раскладку
        int max_id = 0;
        for (const auto& g : groups_) {
            if (g.id.rfind("group_", 0) == 0) {
                try { max_id = std::max(max_id, std::stoi(g.id.substr(6))); } catch (...) {}
            }
        }
        if (const MenuLayoutForMenu* layout = mgr.findMenuLayout(current_menu_key_)) {
            for (const auto& g : layout->groups) {
                if (g.id.rfind("group_", 0) == 0) {
                    try { max_id = std::max(max_id, std::stoi(g.id.substr(6))); } catch (...) {}
                }
            }
        }

        EditorGroup g;
        g.id = "group_" + std::to_string(max_id + 1);
        g.title = new_group_title_[0] != '\0' ? new_group_title_ : g.id;

        EditorNode node;
        node.is_group = true;
        node.group_index = static_cast<int>(groups_.size());
        groups_.push_back(std::move(g));
        nodes_.push_back(node);

        new_group_title_[0] = '\0';
        target_group_ = static_cast<int>(groups_.size()) - 1;
        commit();
    }

    ImGui::SameLine();
    // Целевая группа для кнопки "в группу"
    const char* target_label = TRF("menu.layout_editor.target_group_none",
                                   "<группа не выбрана>");
    std::string target_title_storage;
    if (target_group_ >= 0 && target_group_ < static_cast<int>(groups_.size())) {
        target_title_storage = groups_[static_cast<size_t>(target_group_)].title;
        target_label = target_title_storage.c_str();
    }
    if (ImGui::BeginCombo("##target_group", target_label)) {
        for (int gi = 0; gi < static_cast<int>(groups_.size()); ++gi) {
            bool selected = (gi == target_group_);
            if (ImGui::Selectable(groups_[static_cast<size_t>(gi)].title.c_str(), selected)) {
                target_group_ = gi;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    // ------------------------------------------------------------------
    // Сброс / экспорт / импорт
    // ------------------------------------------------------------------
    if (ImGui::Button(TRF("menu.layout_editor.reset_menu", "Сбросить это меню"))) {
        mgr.resetMenu(current_menu_key_);
        mgr.save();
        rebuildModel();
    }
    ImGui::SameLine();
    if (ImGui::Button(TRF("menu.layout_editor.reset_workspace", "Сбросить workspace"))) {
        mgr.resetActiveWorkspace();
        mgr.save();
        rebuildModel();
    }
    ImGui::SameLine();
    if (ImGui::Button(TRF("menu.layout_editor.export", "Экспорт в буфер"))) {
        ImGui::SetClipboardText(mgr.serializeToJson().c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(TRF("menu.layout_editor.import", "Импорт из буфера"))) {
        if (mgr.deserializeFromJson(ImGui::GetClipboardText())) {
            mgr.save();
            rebuildModel();
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", TRF("menu.layout_editor.hint",
        "Изменения сразу применяются к главному меню и сохраняются автоматически"));

    if (open && !*open) show_ = false;
    ImGui::End();
}

} // namespace ui
} // namespace llama_gui

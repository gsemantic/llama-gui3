#include "../include/ui/profile_manager_dialog.h"
#include "../include/ui/workspace_manager.h"
#include "../external/imgui/imgui.h"
#include <cstring>
#include "../include/ui/input_text_context_menu.h"

namespace llama_gui {
namespace ui {

ProfileManagerDialog::ProfileManagerDialog(llama_gui::core::ConfigManager& config)
    : config_manager_(config) {
}

void ProfileManagerDialog::showCreateDialog() {
    show_create_dialog_ = true;
    std::memset(new_profile_name_, 0, sizeof(new_profile_name_));
    request_focus_create_dialog_ = true;
}

void ProfileManagerDialog::setProfileLoadCallback(std::function<void(const std::string&)> callback) {
    profile_load_callback_ = std::move(callback);
}

void ProfileManagerDialog::render() {
    if (!is_open_) {
        // Сбрасываем флаг при закрытии окна для следующей инициализации
        initialized_ = false;
        return;
    }

    if (!ImGui::Begin("Управление профилями", &is_open_)) {
        ImGui::End();
        return;
    }

    // === ВАЖНО: Инициализация при первом открытии окна ===
    // При первом открытии окна инициализируем selected_profile_ текущим активным профилем
    if (!initialized_) {
        std::string current_profile = config_manager_.getCurrentProfileName();
        if (!current_profile.empty()) {
            selected_profile_ = current_profile;
        } else {
            // Если нет текущего профиля, выбираем первый из списка
            auto profiles = config_manager_.listProfiles();
            if (!profiles.empty()) {
                selected_profile_ = profiles[0];
            }
        }
        std::strncpy(profile_name_buffer_, selected_profile_.c_str(), sizeof(profile_name_buffer_) - 1);
        initialized_ = true;
    }

    // === Список профилей ===
    renderProfileList();

    ImGui::Separator();

    // === Кнопки действий ===
    renderActionButtons();

    ImGui::Separator();

    // === Информация о текущем профиле ===
    std::string current_profile = config_manager_.getCurrentProfileName();
    ImGui::Text("Текущий профиль: %s", current_profile.empty() ? "(по умолчанию)" : current_profile.c_str());

    // === Workspace: видимость меню ===
    if (workspace_manager_) {
        renderWorkspaceEditor();
    }

    // === Статус сообщение ===
    renderStatusMessage();

    // === Диалоги ===
    // Вызываем OpenPopup НЕПОСРЕДСТВЕННО ПЕРЕД каждым BeginPopupModal - это критично!
    renderDeleteConfirmDialog();
    renderCreateDialog();
    renderRenameDialog();

    ImGui::End();
}

void ProfileManagerDialog::renderProfileList() {
    ImGui::Text("Доступные профили:");

    auto profiles = config_manager_.listProfiles();
    std::string current = config_manager_.getCurrentProfileName();

    // Если current пустой, но selected_profile_ установлен - используем его как "текущий"
    if (current.empty() && !selected_profile_.empty()) {
        current = selected_profile_;
    }

    // Список с выделением
    for (size_t i = 0; i < profiles.size(); ++i) {
        const auto& profile = profiles[i];
        bool is_selected = (profile == selected_profile_);
        bool is_current = (profile == current);

        // Формируем строку с индикатором текущего профиля
        std::string display_name = profile;
        if (is_current) {
            display_name += " (текущий)";
        }

        // Выделяем текущий профиль цветом и жирным шрифтом
        if (is_current) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
        }

        if (ImGui::Selectable(display_name.c_str(), is_selected)) {
            selected_profile_ = profile;
            std::strncpy(profile_name_buffer_, profile.c_str(), sizeof(profile_name_buffer_) - 1);
        }

        if (is_current) {
            ImGui::PopStyleColor();
        }

        // Контекстное меню по правой кнопке
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Переименовать")) {
                show_rename_dialog_ = true;
                request_focus_rename_dialog_ = true;
                std::strncpy(profile_name_buffer_, profile.c_str(), sizeof(profile_name_buffer_) - 1);
            }
            if (ImGui::MenuItem("Удалить", nullptr, false, !is_current)) {
                selected_profile_ = profile;
                show_delete_confirm_ = true;
            }
            ImGui::EndPopup();
        }
    }

    if (profiles.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::Text("Нет сохранённых профилей");
        ImGui::PopStyleColor();
    }
}

void ProfileManagerDialog::renderActionButtons() {
    ImGui::Text("Действия:");
    
    float button_width = (ImGui::GetContentRegionAvail().x - 10) / 3;
    
    // Кнопка загрузки
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button("Загрузить", ImVec2(button_width, 0))) {
        if (!selected_profile_.empty()) {
            std::string current = config_manager_.getCurrentProfileName();
            
            // Если выбран уже текущий профиль - перезагрузка не нужна
            if (selected_profile_ == current) {
                showStatusMessage("Профиль уже активен: " + selected_profile_);
            } else if (config_manager_.loadProfile(selected_profile_)) {
                showStatusMessage("Профиль загружен: " + selected_profile_);
                // Вызываем callback для перезагрузки сервера
                if (profile_load_callback_) {
                    profile_load_callback_(selected_profile_);
                }
            } else {
                showStatusMessage("Ошибка загрузки профиля");
            }
        }
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    
    // Кнопка сохранения
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.8f, 1.0f));
    if (ImGui::Button("Сохранить в", ImVec2(button_width, 0))) {
        if (!selected_profile_.empty()) {
            if (config_manager_.saveProfile(selected_profile_)) {
                showStatusMessage("Профиль сохранён: " + selected_profile_);
            } else {
                showStatusMessage("Ошибка сохранения профиля");
            }
        }
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    
    // Кнопка удаления
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    bool can_delete = !selected_profile_.empty();
    ImGui::BeginDisabled(!can_delete);
    if (ImGui::Button("Удалить", ImVec2(button_width, 0))) {
        if (!selected_profile_.empty()) {
            show_delete_confirm_ = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    
    ImGui::Separator();
    
    // Кнопка создания нового
    if (ImGui::Button("Создать новый профиль", ImVec2(-1, 0))) {
        show_create_dialog_ = true;
        std::memset(new_profile_name_, 0, sizeof(new_profile_name_));
    }
    
    // Кнопка сохранения текущего
    ImGui::Separator();
    if (ImGui::Button("Сохранить текущие настройки", ImVec2(-1, 0))) {
        std::string current = config_manager_.getCurrentProfileName();
        if (current.empty()) {
            show_create_dialog_ = true;
        } else {
            if (config_manager_.saveProfile(current)) {
                showStatusMessage("Настройки сохранены в профиль: " + current);
            }
        }
    }
}

void ProfileManagerDialog::renderCreateDialog() {
    if (!show_create_dialog_) return;

    // Вызываем OpenPopup непосредственно перед BeginPopupModal - это критично!
    ImGui::OpenPopup("Создать новый профиль");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Создать новый профиль", &show_create_dialog_,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Введите имя нового профиля:");
        ImGui::Separator();

        ImGui::SetNextItemWidth(250);
        ImGui::InputText("##name", new_profile_name_, sizeof(new_profile_name_));
        InputTextContextMenu();
        
        // Устанавливаем фокус только один раз при открытии диалога
        if (request_focus_create_dialog_) {
            ImGui::SetKeyboardFocusHere(0);
            request_focus_create_dialog_ = false;
        }

        ImGui::Separator();

        if (ImGui::Button("Создать", ImVec2(100, 0))) {
            std::string name(new_profile_name_);
            if (!name.empty()) {
                if (config_manager_.saveProfile(name)) {
                    showStatusMessage("Профиль создан: " + name);
                    selected_profile_ = name;
                    show_create_dialog_ = false;
                    std::memset(new_profile_name_, 0, sizeof(new_profile_name_));
                } else {
                    showStatusMessage("Ошибка создания профиля");
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(100, 0))) {
            show_create_dialog_ = false;
            std::memset(new_profile_name_, 0, sizeof(new_profile_name_));
        }

        ImGui::EndPopup();
    }
}

void ProfileManagerDialog::renderDeleteConfirmDialog() {
    if (!show_delete_confirm_) return;

    // Вызываем OpenPopup непосредственно перед BeginPopupModal - это критично!
    ImGui::OpenPopup("Подтверждение удаления");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Подтверждение удаления", &show_delete_confirm_,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Вы уверены, что хотите удалить профиль\n\"%s\"?", selected_profile_.c_str());
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Это действие нельзя отменить!");
        ImGui::Separator();

        if (ImGui::Button("Удалить", ImVec2(100, 0))) {
            std::string profile_to_delete = selected_profile_;
            std::string profiles_dir = config_manager_.getProfilesDirectory();
            std::string path = profiles_dir + "/" + profile_to_delete + ".json";

            std::error_code ec;
            bool removed = std::filesystem::remove(path, ec);

            if (removed) {
                showStatusMessage("Профиль удалён: " + profile_to_delete);
                selected_profile_.clear();
            } else {
                std::string error_msg = "Ошибка удаления: " + ec.message();
                showStatusMessage(error_msg);
            }
            show_delete_confirm_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(100, 0))) {
            show_delete_confirm_ = false;
        }

        ImGui::EndPopup();
    }
}

void ProfileManagerDialog::renderRenameDialog() {
    if (!show_rename_dialog_) return;

    // Вызываем OpenPopup непосредственно перед BeginPopupModal - это критично!
    ImGui::OpenPopup("Переименовать профиль");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Переименовать профиль", &show_rename_dialog_,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Новое имя профиля:");
        ImGui::Separator();

        ImGui::SetNextItemWidth(250);
        ImGui::InputText("##name", profile_name_buffer_, sizeof(profile_name_buffer_));
        InputTextContextMenu();

        // Устанавливаем фокус только один раз при открытии диалога
        if (request_focus_rename_dialog_) {
            ImGui::SetKeyboardFocusHere(0);
            request_focus_rename_dialog_ = false;
        }

        ImGui::Separator();

        if (ImGui::Button("Переименовать", ImVec2(100, 0))) {
            std::string new_name(profile_name_buffer_);
            if (!new_name.empty() && new_name != selected_profile_) {
                // Копируем файл профиля
                std::string profiles_dir = config_manager_.getProfilesDirectory();
                std::string old_path = profiles_dir + "/" + selected_profile_ + ".json";
                std::string new_path = profiles_dir + "/" + new_name + ".json";

                try {
                    std::filesystem::copy_file(old_path, new_path,
                                               std::filesystem::copy_options::overwrite_existing);
                    std::filesystem::remove(old_path);

                    showStatusMessage("Профиль переименован: " + new_name);
                    selected_profile_ = new_name;
                    show_rename_dialog_ = false;
                } catch (const std::exception& e) {
                    showStatusMessage("Ошибка переименования: " + std::string(e.what()));
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(100, 0))) {
            show_rename_dialog_ = false;
        }

        ImGui::EndPopup();
    }
}

void ProfileManagerDialog::renderWorkspaceEditor() {
    // Заголовок сворачиваемой секции
    if (ImGui::TreeNode("Видимость меню (workspace)")) {
        const auto& ws = workspace_manager_->getCurrentWorkspace();

        // Тип workspace
        ImGui::Text("Текущий workspace: %s", ws.name.c_str());
        ImGui::Separator();

        // Редактирование видимости каждого меню
        for (auto& menu_config : const_cast<std::vector<WorkspaceMenuConfig>&>(ws.menu_configs)) {
            // Чекбокс видимости меню
            bool visible = menu_config.visible;
            if (ImGui::Checkbox(menu_config.menu_name.c_str(), &visible)) {
                menu_config.visible = visible;
            }

            // Если меню видимо — показываем список элементов
            if (menu_config.visible) {
                ImGui::Indent(20.0f);

                // Показываем скрытые элементы (hidden_items)
                if (!menu_config.hidden_items.empty()) {
                    ImGui::TextDisabled("Скрыты:");
                    for (auto& item : menu_config.hidden_items) {
                        ImGui::BulletText("%s", item.c_str());
                    }
                }

                // Если нет явного списка — все элементы видимы
                if (menu_config.visible_items.empty() && menu_config.hidden_items.empty()) {
                    ImGui::TextDisabled("Все элементы видимы");
                }

                ImGui::Unindent(20.0f);
            }

            ImGui::Separator();
        }

        // Команды: включённые
        if (!ws.enabled_commands.empty()) {
            if (ImGui::TreeNode("Включённые команды")) {
                for (const auto& cmd : ws.enabled_commands) {
                    ImGui::BulletText("%s", cmd.c_str());
                }
                ImGui::TreePop();
            }
        }

        // Команды: отключённые
        if (!ws.disabled_commands.empty()) {
            if (ImGui::TreeNode("Отключённые команды")) {
                for (const auto& cmd : ws.disabled_commands) {
                    ImGui::BulletText("%s", cmd.c_str());
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void ProfileManagerDialog::showStatusMessage(const std::string& message) {
    status_message_ = message;
    status_timer_ = 5.0f; // Показывать 5 секунд
}

void ProfileManagerDialog::renderStatusMessage() {
    if (status_timer_ > 0.0f && !status_message_.empty()) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
        ImGui::Text("%s", status_message_.c_str());
        ImGui::PopStyleColor();
        
        status_timer_ -= ImGui::GetIO().DeltaTime;
        if (status_timer_ <= 0.0f) {
            status_message_.clear();
        }
    }
}

} // namespace ui
} // namespace llama_gui

#include "../include/ui/settings_viewer_dialog.h"
#include "../include/core/ini_parser.h"
#include "../include/ui/localization_manager.h"
#include "../include/ui/input_text_context_menu.h"
#include "../external/imgui/imgui.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace llama_gui {
namespace ui {

SettingsViewerDialog::SettingsViewerDialog(Settings& settings)
    : settings_(settings) {
}

SettingsViewerDialog::~SettingsViewerDialog() = default;

void SettingsViewerDialog::show() {
    show_dialog_ = true;
    load_from_file();
}

void SettingsViewerDialog::hide() {
    show_dialog_ = false;
}

size_t SettingsViewerDialog::get_modified_count() const {
    return std::count_if(settings_rows_.begin(), settings_rows_.end(),
                        [](const IniSettingRow& row) { return row.is_modified; });
}

void SettingsViewerDialog::revert_all_changes() {
    // Восстанавливаем оригинальные значения из INI документа
    for (auto& row : settings_rows_) {
        if (row.is_modified) {
            auto it = ini_document_.find(row.section);
            if (it != ini_document_.end()) {
                auto value_it = it->second.find(row.key);
                if (value_it != it->second.end()) {
                    row.value = value_it->second;  // Восстанавливаем из оригинального документа
                    row.is_modified = false;
                }
            }
        }
    }
    settings_changed_ = false;
    status_message_ = TR("settings.viewer.status.reverted");
}

bool SettingsViewerDialog::load_from_file(const std::string& file_path) {
    std::string path = file_path.empty() ? settings_.get_ini_file_path() : file_path;
    
    if (!IniParser::load(path, ini_document_)) {
        status_message_ = std::string(TR("settings.viewer.status.load_failed")) + path;
        std::cerr << status_message_ << std::endl;
        return false;
    }
    
    build_settings_rows();
    build_sections_list();
    
    settings_changed_ = false;
    status_message_ = std::string(TR("settings.viewer.status.loaded")) + path;
    std::cout << "Settings viewer loaded: " << path << std::endl;
    
    return true;
}

bool SettingsViewerDialog::save_to_file(const std::string& file_path) {
    // Сначала применим изменения к INI документу
    for (const auto& row : settings_rows_) {
        if (row.is_modified) {
            IniParser::set(ini_document_, row.section, row.key, row.value);
        }
    }

    std::string path = file_path.empty() ? settings_.get_ini_file_path() : file_path;

    std::string header =
        "; ============================================================\n"
        "; Llama GUI - Централизованный файл настроек\n"
        "; Аналогия: php.ini для web-сервера\n"
        "; ============================================================\n\n"
        "; Этот файл содержит все настройки приложения\n"
        "; Формат: INI (секции [section], ключи key=value)\n"
        "; ============================================================\n\n";

    if (!IniParser::save(path, ini_document_, header)) {
        status_message_ = std::string(TR("settings.viewer.status.save_failed")) + path;
        std::cerr << status_message_ << std::endl;
        return false;
    }

    // Сбрасываем флаги только если сохраняем в ОСНОВНОЙ файл
    bool is_main_file = file_path.empty() || file_path == settings_.get_ini_file_path();
    
    if (is_main_file) {
        // Обновим значения в settings_rows_ из ini_document_ и сбросим флаги
        for (auto& row : settings_rows_) {
            auto it = ini_document_.find(row.section);
            if (it != ini_document_.end()) {
                auto value_it = it->second.find(row.key);
                if (value_it != it->second.end()) {
                    row.value = value_it->second;  // Обновляем значение
                    row.comment = value_it->second;  // Обновляем "оригинальное" значение
                    row.is_modified = false;
                }
            }
        }
        settings_changed_ = false;
    }

    status_message_ = std::string(TR("settings.viewer.status.saved")) + path;
    std::cout << "Settings viewer saved: " + path << std::endl;

    return true;
}

void SettingsViewerDialog::build_settings_rows() {
    settings_rows_.clear();
    
    for (const auto& [section, values] : ini_document_) {
        for (const auto& [key, value] : values) {
            IniSettingRow row;
            row.section = section;
            row.key = key;
            row.value = value;
            row.comment = value;  // Для отмены изменений
            row.is_modified = false;
            settings_rows_.push_back(row);
        }
    }
    
    // Сортировка по секциям и ключам
    std::sort(settings_rows_.begin(), settings_rows_.end(),
              [](const IniSettingRow& a, const IniSettingRow& b) {
                  if (a.section != b.section) return a.section < b.section;
                  return a.key < b.key;
              });
}

void SettingsViewerDialog::build_sections_list() {
    sections_list_.clear();
    sections_list_.push_back("all");  // Опция "все секции"
    
    for (const auto& [section, values] : ini_document_) {
        if (std::find(sections_list_.begin(), sections_list_.end(), section) == sections_list_.end()) {
            sections_list_.push_back(section);
        }
    }
    
    std::sort(sections_list_.begin() + 1, sections_list_.end());
}

std::vector<IniSettingRow> SettingsViewerDialog::get_filtered_rows() const {
    std::vector<IniSettingRow> filtered;
    
    for (const auto& row : settings_rows_) {
        // Фильтр по секции
        if (section_filter_ != "all" && row.section != section_filter_) {
            continue;
        }
        
        // Фильтр по изменённым
        if (show_modified_only_ && !row.is_modified) {
            continue;
        }
        
        // Поиск по ключу или значению
        if (!search_filter_.empty()) {
            bool key_match = row.key.find(search_filter_) != std::string::npos;
            bool value_match = row.value.find(search_filter_) != std::string::npos;
            bool section_match = row.section.find(search_filter_) != std::string::npos;
            
            if (!key_match && !value_match && !section_match) {
                continue;
            }
        }
        
        filtered.push_back(row);
    }
    
    return filtered;
}

void SettingsViewerDialog::render() {
    if (!show_dialog_) return;

    // ImGuiWindowFlags_NoCollapse - предотвращает закрытие по Esc
    if (ImGui::Begin(TR("settings.viewer.title"), &show_dialog_, ImGuiWindowFlags_NoCollapse)) {

        // Панель фильтров
        render_filter_panel();

        ImGui::Separator();

        // Таблица настроек
        render_settings_table();

        ImGui::Separator();

        // Панель действий
        render_action_panel();

        ImGui::End();
    }

    // Модальное окно редактирования
    render_edit_modal();
}

void SettingsViewerDialog::render_filter_panel() {
    const char* filter_label = TR("settings.viewer.filter");
    const char* search_label = TR("settings.viewer.search");
    const char* clear_label = TR("settings.viewer.clear");
    const char* modified_only_label = TR("settings.viewer.modified_only");
    const char* showing_label = TR("settings.viewer.showing");

    ImGui::Text("%s", filter_label);
    ImGui::SameLine();

    // Dropdown для секций
    const char* current_section = (section_filter_ == "all")
        ? TR("settings.viewer.all_sections") : section_filter_.c_str();
    if (ImGui::BeginCombo("##SectionFilter", current_section)) {
        for (const auto& section : sections_list_) {
            bool is_selected = (section_filter_ == section);
            const char* display_name = (section == "all")
                ? TR("settings.viewer.all_sections") : section.c_str();
            if (ImGui::Selectable(display_name, is_selected)) {
                section_filter_ = section;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();

    // Поиск - используем изменяемый буфер для мгновенной фильтрации
    static char search_buffer[256] = "";
    bool search_changed = ImGui::InputText("##Search", search_buffer, sizeof(search_buffer),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
    InputTextContextMenu();
    
    // Обновляем search_filter_ каждый кадр для мгновенной реакции
    std::string current_input = search_buffer;
    if (current_input != search_filter_) {
        search_filter_ = current_input;
        search_changed = true;
    }

    ImGui::SameLine();
    if (ImGui::Button(search_label)) {
        // Кнопка поиска - просто для визуальной совместимости
        search_filter_ = search_buffer;
    }

    ImGui::SameLine();
    if (ImGui::Button(clear_label)) {
        search_filter_ = "";
        std::memset(search_buffer, 0, sizeof(search_buffer));
        section_filter_ = "all";
        show_modified_only_ = false;
    }

    ImGui::SameLine();
    ImGui::Checkbox(modified_only_label, &show_modified_only_);

    // Статистика
    auto filtered = get_filtered_rows();
    size_t modified_count = get_modified_count();

    ImGui::SameLine();
    char stats_buffer[256];
    snprintf(stats_buffer, sizeof(stats_buffer), showing_label,
             filtered.size(), settings_rows_.size(), modified_count);
    ImGui::Text("%s", stats_buffer);

    // Статус
    if (!status_message_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "| %s", status_message_.c_str());
    }
}

void SettingsViewerDialog::render_settings_table() {
    auto filtered_rows = get_filtered_rows();

    if (filtered_rows.empty()) {
        ImGui::TextDisabled("%s", TR("settings.viewer.no_match"));
        return;
    }

    // Создадим таблицу
    if (ImGui::BeginTable("SettingsTable", 4,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                          ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV,
                          ImVec2(0, 400))) {

        ImGui::TableSetupColumn(TR("settings.viewer.col.section"), ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn(TR("settings.viewer.col.key"), ImGuiTableColumnFlags_WidthFixed, 250);
        ImGui::TableSetupColumn(TR("settings.viewer.col.value"), ImGuiTableColumnFlags_WidthStretch, 300);
        ImGui::TableSetupColumn(TR("settings.viewer.col.status"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupScrollFreeze(0, 1); // Закрепить заголовок
        ImGui::TableHeadersRow();

        int row_index = 0;
        for (const auto& row : filtered_rows) {
            // Найдём индекс строки в оригинальном settings_rows_
            auto it = std::find_if(settings_rows_.begin(), settings_rows_.end(),
                                   [&row](const IniSettingRow& r) {
                                       return r.section == row.section && r.key == row.key;
                                   });
            int original_index = (it != settings_rows_.end()) ? std::distance(settings_rows_.begin(), it) : -1;

            ImGui::PushID(row_index);
            render_table_row(original_index, row_index);
            ImGui::PopID();
            row_index++;
        }

        ImGui::EndTable();
    }
}

void SettingsViewerDialog::render_table_row(int original_index, int display_index) {
    if (original_index < 0 || original_index >= static_cast<int>(settings_rows_.size())) {
        return;  // Защита от невалидного индекса
    }

    IniSettingRow& row = settings_rows_[original_index];

    ImGui::TableNextRow();

    // Section
    ImGui::TableNextColumn();
    ImGui::Text("%s", row.section.c_str());

    // Key
    ImGui::TableNextColumn();
    ImGui::Text("%s", row.key.c_str());

    // Value
    ImGui::TableNextColumn();
    if (row.value.empty()) {
        // Для пустых значений используем Selectable для создания интерактивной области
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(128, 128, 128, 255));
        char selectable_id[64];
        snprintf(selectable_id, sizeof(selectable_id), "##empty_%d", original_index);
        ImGui::Selectable(selectable_id, false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 0));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR("settings.viewer.edit_tooltip"));
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            strncpy(current_edit_buffer_, "", sizeof(current_edit_buffer_) - 1);
            current_edit_buffer_[sizeof(current_edit_buffer_) - 1] = '\0';
            current_editing_row_index_ = original_index;
            show_edit_modal_ = true;
            modal_focus_frame_ = 0;
        }
        // Рисуем текст [empty] поверх
        ImGui::SameLine();
        ImGui::Text("%s", TR("settings.viewer.empty"));
        ImGui::PopStyleColor();
    } else {
        ImGui::Text("%s", row.value.c_str());
    }

    // Кнопка редактирования при наведении (для непустых значений)
    if (!row.value.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("settings.viewer.edit_tooltip"));
    }

    // Обработка двойного клика - открываем модальное окно (для непустых значений)
    if (!row.value.empty() && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        // Копируем значение в буфер и показываем модальное окно
        strncpy(current_edit_buffer_, row.value.c_str(), sizeof(current_edit_buffer_) - 1);
        current_edit_buffer_[sizeof(current_edit_buffer_) - 1] = '\0';
        current_editing_row_index_ = original_index;  // Сохраняем индекс, а не указатель
        show_edit_modal_ = true;
        modal_focus_frame_ = 0;  // Сбрасываем для установки фокуса
    }

    // Status
    ImGui::TableNextColumn();
    if (row.is_modified) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "%s", TR("settings.viewer.modified"));
    } else {
        ImGui::TextDisabled("-");
    }
}

void SettingsViewerDialog::render_action_panel() {
    // Локализованные строки
    const char* actions_label = TR("settings.viewer.actions");
    const char* save_label = TR("settings.viewer.save");
    const char* revert_label = TR("settings.viewer.revert");
    const char* reload_label = TR("settings.viewer.reload");
    const char* apply_label = TR("settings.viewer.apply");
    const char* close_label = TR("settings.viewer.close");

    ImGui::Text("%s", actions_label);
    ImGui::SameLine();

    bool has_changes = (get_modified_count() > 0);

    // Save
    if (ImGui::Button(save_label, ImVec2(100, 0))) {
        std::cout << "[INI Viewer] Save button clicked!" << std::endl;
        save_to_file();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("settings.viewer.tooltip.save"));
    }

    ImGui::SameLine();

    // Revert - только одна кнопка, с проверкой доступности
    if (has_changes) {
        if (ImGui::Button(revert_label, ImVec2(100, 0))) {
            std::cout << "[INI Viewer] Revert button clicked!" << std::endl;
            revert_all_changes();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR("settings.viewer.tooltip.revert"));
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(revert_label, ImVec2(100, 0));
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    // Reload / Откатить
    if (ImGui::Button(reload_label, ImVec2(100, 0))) {
        std::cout << "[INI Viewer] Reload button clicked!" << std::endl;
        load_from_file();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("settings.viewer.tooltip.reload"));
    }

    ImGui::SameLine();

    // Apply to Core Settings
    if (ImGui::Button(apply_label, ImVec2(100, 0))) {
        std::cout << "[INI Viewer] Apply button clicked!" << std::endl;
        apply_settings_to_core();
        status_message_ = TR("settings.viewer.status.applied");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("settings.viewer.tooltip.apply"));
    }

    ImGui::SameLine();
    ImGui::Separator();

    ImGui::SameLine();

    // Close
    if (ImGui::Button(close_label, ImVec2(80, 0))) {
        std::cout << "[INI Viewer] Close button clicked!" << std::endl;
        hide();
    }
}

void SettingsViewerDialog::render_edit_modal() {
    // Проверяем корректность индекса
    if (!show_edit_modal_ || 
        current_editing_row_index_ < 0 || 
        current_editing_row_index_ >= static_cast<int>(settings_rows_.size())) {
        return;
    }

    IniSettingRow& row = settings_rows_[current_editing_row_index_];

    // Центрируем окно
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_Appearing);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));

    // Используем уникальный ID для окна редактирования на основе индекса
    char window_id[256];
    snprintf(window_id, sizeof(window_id), "Edit Setting##%d.%s.%s",
             current_editing_row_index_,
             row.section.c_str(),
             row.key.c_str());

    bool modal_open = true;
    // ImGuiWindowFlags_Modal - автоматически затемняет фон корректно
    if (ImGui::Begin(window_id, &modal_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_Modal)) {

        // Заголовок
        char title_buffer[512];
        snprintf(title_buffer, sizeof(title_buffer), "%s.%s",
                 row.section.c_str(),
                 row.key.c_str());
        ImGui::Text("%s", title_buffer);
        ImGui::Separator();
        ImGui::Spacing();

        // Поле ввода
        ImGui::Text("%s", TR("settings.viewer.value"));
        ImGui::Spacing();
        ImGui::SetNextItemWidth(400);

        // Устанавливаем фокус при появлении окна
        if (modal_focus_frame_ == 0) {
            ImGui::SetKeyboardFocusHere();
            modal_focus_frame_ = 1;
        }

        bool enter_pressed = false;
        ImGui::SetNextItemWidth(400);
        if (ImGui::InputText("##EditValue", current_edit_buffer_, sizeof(current_edit_buffer_),
                            ImGuiInputTextFlags_EnterReturnsTrue)) {
            enter_pressed = true;
        }
        InputTextContextMenu();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Кнопки
        float button_width = 80.0f;
        float total_width = button_width * 3 + ImGui::GetStyle().ItemSpacing.x * 2;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total_width) / 2 + button_width);

        // Уникальные ID для кнопок на основе индекса
        char ok_id[64], cancel_id[64], apply_id[64];
        snprintf(ok_id, sizeof(ok_id), "OK##edit%d", current_editing_row_index_);
        snprintf(cancel_id, sizeof(cancel_id), "Cancel##edit%d", current_editing_row_index_);
        snprintf(apply_id, sizeof(apply_id), "Apply##edit%d", current_editing_row_index_);

        bool should_close = false;
        bool apply_clicked = false;

        if (ImGui::Button(ok_id, ImVec2(button_width, 0)) || enter_pressed) {
            std::string new_value = current_edit_buffer_;
            if (new_value != row.value) {
                row.value = new_value;
                row.is_modified = true;
                settings_changed_ = true;
                IniParser::set(ini_document_, row.section, row.key, new_value);
                std::cout << "[INI Viewer] Edited: " << row.section
                          << "." << row.key << " = " << new_value << std::endl;
            }
            should_close = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(cancel_id, ImVec2(button_width, 0))) {
            should_close = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(apply_id, ImVec2(button_width, 0))) {
            std::string new_value = current_edit_buffer_;
            if (new_value != row.value) {
                row.value = new_value;
                row.is_modified = true;
                settings_changed_ = true;
                IniParser::set(ini_document_, row.section, row.key, new_value);
                std::cout << "[INI Viewer] Edited: " << row.section
                          << "." << row.key << " = " << new_value << std::endl;
            }
            apply_clicked = true;
            // Не закрываем окно после Apply
        }

        // Проверяем закрытие через крестик
        if (!modal_open) {
            should_close = true;
        }

        if (should_close) {
            show_edit_modal_ = false;
            current_editing_row_index_ = -1;
            modal_focus_frame_ = 0;
        } else if (apply_clicked) {
            // После Apply сбрасываем фокус для возможности повторной установки
            modal_focus_frame_ = 0;
        }

        ImGui::End();
    } else {
        // Окно было закрыто (например, через крестик)
        if (show_edit_modal_) {
            show_edit_modal_ = false;
            current_editing_row_index_ = -1;
            modal_focus_frame_ = 0;
        }
    }

    ImGui::PopStyleVar(2);
}

void SettingsViewerDialog::apply_settings_to_core() {
    std::cout << "[INI Viewer] Applying settings..." << std::endl;
    std::cout << "[INI Viewer] Modified rows before save: " << get_modified_count() << std::endl;

    // Сохраняем временный файл и загружаем его в Settings
    std::string temp_file = "settings_temp.ini";

    if (save_to_file(temp_file)) {
        settings_.load_from_ini(temp_file);
        // sync_ctx_size() и sync_max_tokens() вызываются внутри load_from_ini()

        // После синхронизации СОХРАНЯЕМ в ОСНОВНОЙ settings.ini
        std::string main_ini = settings_.get_ini_file_path();
        std::cout << "[INI Viewer] Saving to main INI: " << main_ini << std::endl;
        save_to_file(main_ini);  // Сохраняем в основной файл (флаги сбрасываются внутри)

        // После сохранения ПЕРЕЗАГРУЖАЕМ ini_document_ из основного файла
        std::cout << "[INI Viewer] Reloading ini_document_ from main INI..." << std::endl;
        IniParser::load(main_ini, ini_document_);

        // Обновляем значения в settings_rows_ из обновлённого ini_document_
        // (флаги уже сброшены в save_to_file, но значения нужно обновить)
        for (auto& row : settings_rows_) {
            auto it = ini_document_.find(row.section);
            if (it != ini_document_.end()) {
                auto value_it = it->second.find(row.key);
                if (value_it != it->second.end()) {
                    row.value = value_it->second;
                    row.comment = value_it->second;
                }
            }
        }
        settings_changed_ = false;

        // Сохраняем синхронизированные значения в текущий профиль JSON
        std::string current_profile = settings_.get_current_profile_name();
        if (!current_profile.empty()) {
            settings_.save_profile(current_profile);
            std::cout << "[INI Viewer] Profile saved: " << current_profile << std::endl;
        }

        status_message_ = TR("settings.viewer.status.applied_sync");

        // Удаляем временный файл
        std::remove(temp_file.c_str());
        std::cout << "[INI Viewer] Temporary file removed: " << temp_file << std::endl;
    }
}

} // namespace ui
} // namespace llama_gui

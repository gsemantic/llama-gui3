#include "../include/ui/file_browser.h"
#include "../external/imgui/imgui.h"
#include "../include/ui/font_awesome_icons.h"
#include "../include/ui/localization_manager.h"
#include <iostream>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>

namespace llama_gui {
namespace ui {

FileBrowser::FileBrowser() {
    go_home();
    scan_directory();
}

void FileBrowser::render() {
    render_toolbar();
    render_breadcrumbs();
    ImGui::Separator();

    // Таблица должна заполнять оставшуюся высоту (минус нижняя панель), чтобы был
    // только один скролл — внутри таблицы, а не у родительского окна.
    float bottom_block = ImGui::GetFrameHeightWithSpacing()      // строка footer
                       + ImGui::GetStyle().ItemSpacing.y * 2.0f  // сепараторы/отступы
                       + 4.0f
                       + extra_bottom_reserve_;
    float table_height = ImGui::GetContentRegionAvail().y - bottom_block;
    if (table_height < 40.0f) {
        table_height = 40.0f;
    }
    render_file_list(table_height);

    ImGui::Separator();
    render_footer();
}

void FileBrowser::set_directory(const std::string& path) {
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
        current_path_ = std::filesystem::absolute(path).string();
        selected_file_.clear();
        scan_directory();
    }
}

void FileBrowser::go_up() {
    std::string parent = get_parent_directory(current_path_);
    if (!parent.empty() && parent != current_path_) {
        set_directory(parent);
    }
}

void FileBrowser::go_home() {
    const char* home = std::getenv("HOME");
    if (home) {
        current_path_ = home;
    } else {
        current_path_ = "/";
    }
    selected_file_.clear();
}

void FileBrowser::refresh() {
    scan_directory();
}

void FileBrowser::render_toolbar() {
    // Навигация одной строкой: ↑ / домой / обновить + показ скрытых + поиск
    if (ImGui::Button(FontAwesomeIcons::ArrowUp)) {
        go_up();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("file_browser.parent"));
    }
    ImGui::SameLine();
    if (ImGui::Button(FontAwesomeIcons::Home)) {
        go_home();
        scan_directory();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("file_browser.home"));
    }
    ImGui::SameLine();
    if (ImGui::Button(FontAwesomeIcons::RotateRight)) {
        refresh();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("file_browser.refresh"));
    }

    ImGui::SameLine();
    ImGui::Checkbox(TR("file_browser.show_hidden"), &show_hidden_files_);

    ImGui::SameLine();
    float search_width = ImGui::GetContentRegionAvail().x;
    if (search_width < 80.0f) {
        search_width = 80.0f;
    }
    ImGui::SetNextItemWidth(search_width);
    ImGui::InputTextWithHint("##file_search", TR("file_browser.search_hint"),
                             search_buf_, sizeof(search_buf_));
}

void FileBrowser::render_breadcrumbs() {
    ImGui::Text("%s %s", FontAwesomeIcons::Folder, current_path_.c_str());
}

void FileBrowser::render_file_list(float table_height) {
    // Table header
    if (ImGui::BeginTable("FileList", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                                        ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, table_height))) {
        ImGui::TableSetupColumn(TR("file_browser.name"), ImGuiTableColumnFlags_WidthFixed, name_column_width_);
        ImGui::TableSetupColumn(TR("file_browser.size"), ImGuiTableColumnFlags_WidthFixed, size_column_width_);
        ImGui::TableSetupColumn(TR("file_browser.type"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Render entries
        for (const auto& entry : entries_) {
            // Skip hidden files if option disabled
            if (!show_hidden_files_ && entry.name[0] == '.') {
                continue;
            }

            // Фильтр по поисковому запросу
            if (!matches_search(entry)) {
                continue;
            }

            ImGui::TableNextRow();

            // Name column
            ImGui::TableSetColumnIndex(0);
            const char* icon = entry.is_directory ? FontAwesomeIcons::Folder : FontAwesomeIcons::File;
            ImGui::Text("%s %s", icon, entry.name.c_str());

            // Double-click handler
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (entry.is_directory) {
                    on_directory_double_clicked(entry);
                } else {
                    on_file_double_clicked(entry);
                }
            }

            // Selection
            if (ImGui::IsItemClicked()) {
                selected_file_ = entry.full_path;
            }

            // Size column
            ImGui::TableSetColumnIndex(1);
            if (!entry.is_directory) {
                ImGui::Text("%s", format_size(entry.size).c_str());
            } else {
                ImGui::Text("-");
            }

            // Type column
            ImGui::TableSetColumnIndex(2);
            if (entry.is_directory) {
                ImGui::Text(TR("file_browser.directory"));
            } else if (!entry.extension.empty()) {
                ImGui::Text(TRF("file_browser.file_ext", "%s file"), entry.extension.substr(1).c_str());
            } else {
                ImGui::Text(TR("file_browser.file"));
            }
        }

        ImGui::EndTable();
    }
}

void FileBrowser::render_footer() {
    if (!selected_file_.empty()) {
        std::string filename = selected_file_.substr(selected_file_.find_last_of("/\\") + 1);
        ImGui::Text(TRF("file_browser.selected", "Selected: %s"), filename.c_str());

        ImGui::SameLine();
        const char* open_label = (pick_mode_ == PickMode::Directory)
            ? TR("file_picker.select_folder")
            : TR("file_browser.open");
        if (ImGui::Button(open_label)) {
            if (on_file_selected_) {
                on_file_selected_(selected_file_);
            }
        }
    } else {
        ImGui::Text(TR("file_browser.no_selection"));
    }
}

void FileBrowser::scan_directory() {
    entries_.clear();

    try {
        for (const auto& entry : std::filesystem::directory_iterator(current_path_)) {
            FileEntry file_entry;
            file_entry.name = entry.path().filename().string();
            file_entry.full_path = entry.path().string();
            file_entry.is_directory = entry.is_directory();

            if (!file_entry.is_directory) {
                file_entry.size = entry.file_size();
                file_entry.extension = entry.path().extension().string();
            } else {
                file_entry.size = 0;
                file_entry.extension = "";
            }

            entries_.push_back(file_entry);
        }

        sort_entries();
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "FileBrowser: Error scanning directory: " << e.what() << std::endl;
    }
}

void FileBrowser::on_file_double_clicked(const FileEntry& entry) {
    selected_file_ = entry.full_path;
    if (on_file_selected_) {
        on_file_selected_(entry.full_path);
    }
}

void FileBrowser::on_directory_double_clicked(const FileEntry& entry) {
    set_directory(entry.full_path);
}

void FileBrowser::sort_entries() {
    // Directories first, then files
    std::stable_sort(entries_.begin(), entries_.end(),
        [](const FileEntry& a, const FileEntry& b) {
            if (a.is_directory != b.is_directory) {
                return a.is_directory > b.is_directory;
            }
            return a.name < b.name;
        });
}

std::string FileBrowser::format_size(uintmax_t size) const {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double dsize = static_cast<double>(size);

    while (dsize >= 1024.0 && unit < 4) {
        dsize /= 1024.0;
        unit++;
    }

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.1f %s", dsize, units[unit]);
    return std::string(buffer);
}

std::string FileBrowser::get_parent_directory(const std::string& path) const {
    try {
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            return p.parent_path().string();
        }
    } catch (...) {
        // Ignore errors
    }
    return "";
}

bool FileBrowser::matches_search(const FileEntry& entry) const {
    if (search_buf_[0] == '\0') {
        return true;
    }

    std::string query = search_buf_;
    std::string name = entry.name;
    std::transform(query.begin(), query.end(), query.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return name.find(query) != std::string::npos;
}

} // namespace ui
} // namespace llama_gui

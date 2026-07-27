#pragma once

#include <string>
#include <vector>
#include <functional>
#include <filesystem>

namespace llama_gui {
namespace ui {

/**
 * File browser component for navigating and selecting files
 * Shows directory tree with navigation support
 */
class FileBrowser {
public:
    using FileSelectedCallback = std::function<void(const std::string&)>;

    FileBrowser();
    ~FileBrowser() = default;

    // Main rendering
    void render();

    // Navigation
    void set_directory(const std::string& path);
    void go_up();
    void go_home();
    void refresh();

    // Callbacks
    void set_file_selected_callback(FileSelectedCallback callback) {
        on_file_selected_ = callback;
    }

    // Get current state
    std::string get_current_directory() const { return current_path_; }
    std::string get_selected_file() const { return selected_file_; }

private:
    struct FileEntry {
        std::string name;
        std::string full_path;
        bool is_directory;
        uintmax_t size;
        std::string extension;
    };

    // Rendering helpers
    void render_header();
    void render_file_list();
    void render_breadcrumbs();
    void render_footer();

    // File operations
    void scan_directory();
    void on_file_double_clicked(const FileEntry& entry);
    void on_directory_double_clicked(const FileEntry& entry);

    // Sorting
    void sort_entries();

    // Helpers
    std::string format_size(uintmax_t size) const;
    std::string get_parent_directory(const std::string& path) const;

    // State
    std::string current_path_;
    std::string selected_file_;
    std::vector<FileEntry> entries_;
    FileSelectedCallback on_file_selected_;

    // UI state
    bool show_hidden_files_ = false;
    int sort_mode_ = 0; // 0=name, 1=size, 2=type
    bool sort_ascending_ = true;
    float name_column_width_ = 200.0f;
    float size_column_width_ = 100.0f;
};

} // namespace ui
} // namespace llama_gui

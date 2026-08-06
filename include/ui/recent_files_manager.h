#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

namespace llama_gui {
namespace ui {

/**
 * Manager for recent files persistence
 * Handles saving and loading recent files to/from disk
 */
class RecentFilesManager {
public:
    RecentFilesManager(const std::string& config_path = ".config/llama-gui/recent_files.json");
    ~RecentFilesManager() = default;

    // Load recent files from disk
    std::vector<std::string> load();

    // Save recent files to disk
    void save(const std::vector<std::string>& files);

    // Get/set config file path
    void set_config_path(const std::string& path) { config_path_ = path; }
    std::string get_config_path() const { return config_path_; }

private:
    std::string config_path_;

    // Helper to ensure directory exists
    bool ensure_directory_exists() const;
};

} // namespace ui
} // namespace llama_gui

#include "../include/ui/recent_files_manager.h"
#include <iostream>
#include <algorithm>

namespace llama_gui {
namespace ui {

RecentFilesManager::RecentFilesManager(const std::string& config_path)
    : config_path_(config_path) {
}

std::vector<std::string> RecentFilesManager::load() {
    std::vector<std::string> recent_files;

    try {
        std::ifstream file(config_path_);
        if (!file.is_open()) {
            std::cout << "RecentFilesManager: No recent files file found" << std::endl;
            return recent_files;
        }

        // Simple JSON parsing for recent files
        std::string line;
        while (std::getline(file, line)) {
            // Look for file paths in quotes
            size_t start = line.find("\"");
            while (start != std::string::npos) {
                size_t end = line.find("\"", start + 1);
                if (end != std::string::npos) {
                    std::string path = line.substr(start + 1, end - start - 1);
                    // Check if it looks like a file path (contains / or \)
                    if (path.find('/') != std::string::npos || path.find('\\') != std::string::npos) {
                        recent_files.push_back(path);
                    }
                    start = line.find("\"", end + 1);
                } else {
                    break;
                }
            }
        }

        file.close();
        std::cout << "RecentFilesManager: Loaded " << recent_files.size() << " recent files" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "RecentFilesManager: Error loading recent files: " << e.what() << std::endl;
    }

    return recent_files;
}

void RecentFilesManager::save(const std::vector<std::string>& files) {
    try {
        // Ensure config directory exists
        if (!ensure_directory_exists()) {
            std::cerr << "RecentFilesManager: Failed to create config directory" << std::endl;
            return;
        }

        std::ofstream file(config_path_);
        if (!file.is_open()) {
            std::cerr << "RecentFilesManager: Failed to save recent files" << std::endl;
            return;
        }

        file << "[\n";
        for (size_t i = 0; i < files.size(); ++i) {
            file << "  \"" << files[i] << "\"";
            if (i < files.size() - 1) {
                file << ",";
            }
            file << "\n";
        }
        file << "]\n";

        file.close();
        std::cout << "RecentFilesManager: Saved " << files.size() << " recent files" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "RecentFilesManager: Error saving recent files: " << e.what() << std::endl;
    }
}

bool RecentFilesManager::ensure_directory_exists() const {
    try {
        std::filesystem::path path(config_path_);
        std::filesystem::path parent = path.parent_path();

        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
            std::cout << "RecentFilesManager: Created directory: " << parent.string() << std::endl;
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "RecentFilesManager: Error creating directory: " << e.what() << std::endl;
        return false;
    }
}

} // namespace ui
} // namespace llama_gui

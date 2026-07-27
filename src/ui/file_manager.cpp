#include "../include/ui/file_manager.h"
#include "../include/core/rag_manager.h"
#include "../external/imgui/imgui.h"
#include "../include/ui/localization_manager.h"
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace llama_gui {
namespace ui {

FileManager::FileManager(StateManager& state_manager, Settings& settings)
    : state_manager_(state_manager)
    , settings_(settings)
    , recent_files_manager_(".config/llama-gui/recent_files.json") {
    // Initialize with default directory
    current_directory_ = ".";

    // Load recent files from disk
    recent_files_ = recent_files_manager_.load();

    // Set up file browser callback
    file_browser_.set_file_selected_callback([this](const std::string& file_path) {
        open_file(file_path);
    });
}

FileManager::~FileManager() = default;

void FileManager::render(bool* visible) {
    // Стандартное окно ImGui с заголовком и кнопками управления
    // Кнопка закрытия (×) и сворачивания (─) рисуются автоматически ImGui
    if (!ImGui::Begin(TR("files.title"), visible, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("FileTabs")) {
        if (ImGui::BeginTabItem(TR("files.browse"))) {
            render_browse_files();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(TR("files.recent"))) {
            render_recent_files();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(TR("files.attachments"))) {
            render_attachments();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(TR("files.operations"))) {
            render_file_operations();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void FileManager::open_file(const std::string& file_path) {
    std::cout << "FileManager: Opening file: " << file_path << std::endl;
    add_recent_file(file_path);

    // Read file content
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "FileManager: Failed to open file: " << file_path << std::endl;
        return;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
    file.close();

    std::cout << "FileManager: Read " << content.length() << " bytes from file" << std::endl;

    // Notify callback with file content
    if (on_file_content_) {
        on_file_content_(content);
    }
}

void FileManager::open_file_dialog(const std::string& title, std::function<void(const std::string&)> callback) {
    dialog_helper_.open_file_dialog(title, callback);
}

void FileManager::open_file_dialog(const std::string& title, std::function<void(const std::string&)> callback,
                                   const std::string& filter) {
    // For now, just use the regular dialog without filter support
    // TODO: Extend FileDialogHelper to support file filters
    dialog_helper_.open_file_dialog(title, callback);
}

void FileManager::open_directory_dialog(const std::string& title, std::function<void(const std::string&)> callback) {
    dialog_helper_.open_directory_dialog(title, callback);
}

void FileManager::add_recent_file(const std::string& file_path) {
    recent_files_.erase(std::remove(recent_files_.begin(), recent_files_.end(), file_path), recent_files_.end());
    recent_files_.insert(recent_files_.begin(), file_path);

    // Limit to 10 recent files
    if (recent_files_.size() > 10) {
        recent_files_.resize(10);
    }

    // Save to disk
    recent_files_manager_.save(recent_files_);
}

void FileManager::clear_recent_files() {
    recent_files_.clear();
    recent_files_manager_.save(recent_files_);
}

void FileManager::add_attachment(const std::string& file_path) {
    // Check if file already exists in attachments
    auto it = std::find(attachments_.begin(), attachments_.end(), file_path);
    if (it == attachments_.end()) {
        attachments_.push_back(file_path);
        std::cout << "FileManager: Added attachment: " << file_path << std::endl;

        // === ОБРАБОТКА ФАЙЛА ЧЕРЕЗ RAG (если включен) ===
        // Это позволяет модели "видеть" содержимое прикрепленного файла
        if (rag_manager_ && rag_enabled_) {
            std::cout << "[FILEMANAGER RAG] Processing attached file through RAG: " << file_path << std::endl;
            
            try {
                bool success = rag_manager_->process_document(file_path);
                if (success) {
                    std::cout << "[FILEMANAGER RAG] File successfully processed and indexed: " << file_path << std::endl;
                } else {
                    std::cerr << "[FILEMANAGER RAG] Failed to process file: " << file_path << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "[FILEMANAGER RAG] Error processing file: " << e.what() << std::endl;
            }
        } else {
            std::cout << "[FILEMANAGER RAG] RAG is disabled or not configured, file will not be indexed" << std::endl;
        }
        // ==================================================

        // Notify callback
        if (on_attachment_changed_) {
            on_attachment_changed_(attachments_);
        }
    }
}

void FileManager::remove_attachment(const std::string& file_path) {
    auto it = std::find(attachments_.begin(), attachments_.end(), file_path);
    if (it != attachments_.end()) {
        attachments_.erase(it);
        std::cout << "FileManager: Removed attachment: " << file_path << std::endl;

        // Notify callback
        if (on_attachment_changed_) {
            on_attachment_changed_(attachments_);
        }
    }
}

bool FileManager::can_preview(const std::string& file_path) const {
    // Implement file type checking using available settings
    std::string ext = get_file_extension(file_path);

    // Check against supported import formats
    const auto& supported_formats = settings_.files().supported_import_formats;
    for (const auto& format : supported_formats) {
        if (ext == "." + format) {
            return true;
        }
    }

    return false;
}

void FileManager::render_file_preview(const std::string& file_path) {
    // Stub implementation
    std::cout << "FileManager: Rendering preview for " << file_path << std::endl;
}

void FileManager::render_file_tree() {
    // Stub implementation - deprecated, use render_browse_files instead
}

void FileManager::render_browse_files() {
    file_browser_.render();
}

void FileManager::render_recent_files() {
    if (recent_files_.empty()) {
        ImGui::Text(TR("files.no_recent"));
        return;
    }

    ImGui::Text(TR("files.recent_header"));
    ImGui::Separator();

    for (size_t i = 0; i < recent_files_.size(); ++i) {
        const auto& file_path = recent_files_[i];
        std::string file_name = file_path.substr(file_path.find_last_of("/\\") + 1);

        if (ImGui::Selectable(file_name.c_str(), false)) {
            open_file(file_path);
        }

        // Right-click context menu
        if (ImGui::BeginPopupContextItem(("recent_" + std::to_string(i)).c_str())) {
            if (ImGui::MenuItem(TR("files.open"))) {
                open_file(file_path);
            }
            if (ImGui::MenuItem(TR("files.remove_recent"))) {
                recent_files_.erase(recent_files_.begin() + i);
                --i; // Adjust index after removal
            }
            ImGui::EndPopup();
        }
    }

    if (ImGui::Button(TR("files.clear_all"))) {
        clear_recent_files();
    }
}

void FileManager::render_attachments() {
    ImGui::Text(TR("files.attachments"));
    ImGui::Separator();

    if (attachments_.empty()) {
        ImGui::Text(TR("files.no_attachments"));
        return;
    }

    for (size_t i = 0; i < attachments_.size(); ++i) {
        const auto& file_path = attachments_[i];
        std::string file_name = file_path.substr(file_path.find_last_of("/\\") + 1);

        ImGui::BulletText("%s", file_name.c_str());

        ImGui::SameLine();
        ImGui::PushID(i);
        if (ImGui::SmallButton(TR("button.remove"))) {
            remove_attachment(file_path);
            --i; // Adjust index after removal
        }
        ImGui::PopID();
    }

    // Clear all button
    if (ImGui::Button(TR("files.clear_attachments"))) {
        attachments_.clear();
        std::cout << "FileManager: Cleared all attachments" << std::endl;

        // Notify callback
        if (on_attachment_changed_) {
            on_attachment_changed_(attachments_);
        }
    }
    
    // Info about attachments
    if (!attachments_.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Файлов прикреплено: %zu", attachments_.size());
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(Содержимое файлов будет отправлено модели)");
    }
}

void FileManager::render_file_operations() {
    ImGui::Text(TR("files.operations"));
    ImGui::Separator();

    if (ImGui::Button(TR("files.browse"))) {
        open_file_dialog(TR("files.browse"), [this](const std::string& file_path) {
            if (!file_path.empty()) {
                open_file(file_path);
            }
        });
    }

    ImGui::SameLine();
    if (ImGui::Button(TR("files.add_attachment"))) {
        open_file_dialog(TR("files.add_attachment"), [this](const std::string& file_path) {
            if (!file_path.empty()) {
                add_attachment(file_path);  // Используем метод add_attachment для обработки через RAG
            }
        });
    }

    ImGui::SameLine();
    if (ImGui::Button(TR("files.browse_dir"))) {
        open_directory_dialog(TR("files.browse_dir"), [this](const std::string& dir_path) {
            if (!dir_path.empty()) {
                current_directory_ = dir_path;
                std::cout << "FileManager: Current directory set to: " << dir_path << std::endl;
            }
        });
    }

    ImGui::SameLine();
    if (ImGui::Button(TR("button.load_model"))) {
        open_file_dialog(TR("button.load_model"), [this](const std::string& file_path) {
            if (!file_path.empty()) {
                std::cout << "FileManager: Model file selected: " << file_path << std::endl;
                // Notify callback for model loading
                if (on_model_load_) {
                    on_model_load_(file_path);
                }
            }
        }, "*.gguf *.bin *.ggml *.pth *.pt *.ckpt");
    }

    ImGui::Separator();
    ImGui::Text(TR("files.drag_drop"));
    handle_drag_and_drop();
}

void FileManager::handle_drag_and_drop() {
    // Check if we're receiving a drag and drop payload
    if (ImGui::BeginDragDropTarget()) {
        // Try to accept file paths
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("text/plain")) {
            if (payload->DataSize > 0) {
                // Extract file path from payload
                std::string file_path(static_cast<const char*>(payload->Data), payload->DataSize);

                // Remove trailing null characters
                while (!file_path.empty() && file_path.back() == '\0') {
                    file_path.pop_back();
                }

                // Check if it's a valid file path
                if (!file_path.empty() && std::filesystem::exists(file_path)) {
                    if (std::filesystem::is_regular_file(file_path)) {
                        // Add as attachment
                        add_attachment(file_path);
                        std::cout << "FileManager: File dropped: " << file_path << std::endl;
                    } else if (std::filesystem::is_directory(file_path)) {
                        // Set as current directory
                        current_directory_ = file_path;
                        std::cout << "FileManager: Directory dropped: " << file_path << std::endl;
                    }
                }
            }
        }

        // Try to accept file paths from external applications (file manager)
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("_FILEDROP")) {
            if (payload->DataSize > 0) {
                // Extract file path from payload (array of strings)
                const char* data = static_cast<const char*>(payload->Data);
                std::string file_path(data, payload->DataSize);

                // Remove trailing null characters
                while (!file_path.empty() && file_path.back() == '\0') {
                    file_path.pop_back();
                }

                // Check if it's a valid file path
                if (!file_path.empty() && std::filesystem::exists(file_path)) {
                    if (std::filesystem::is_regular_file(file_path)) {
                        // Add as attachment
                        add_attachment(file_path);
                        std::cout << "FileManager: External file dropped: " << file_path << std::endl;
                    } else if (std::filesystem::is_directory(file_path)) {
                        // Set as current directory
                        current_directory_ = file_path;
                        std::cout << "FileManager: External directory dropped: " << file_path << std::endl;
                    }
                }
            }
        }

        ImGui::EndDragDropTarget();
    }
}

std::string FileManager::get_file_extension(const std::string& file_path) const {
    size_t pos = file_path.find_last_of('.');
    if (pos == std::string::npos) return "";
    return file_path.substr(pos);
}

std::string FileManager::format_file_size(long long size) const {
    // Simple size formatting
    if (size < 1024) return std::to_string(size) + " B";
    if (size < 1024 * 1024) return std::to_string(size / 1024) + " KB";
    return std::to_string(size / (1024 * 1024)) + " MB";
}

} // namespace ui
} // namespace llama_gui

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "core/llama_interface.h"
#include "core/state_manager.h"
#include "core/settings.h"
#include "file_dialog_helper.h"
#include "file_browser.h"
#include "recent_files_manager.h"

// Forward declaration for RAG integration
namespace llama_gui { namespace core { class RagManager; } }

namespace llama_gui {
namespace ui {

using Settings = llama_gui::core::Settings;
using StateManager = llama_gui::core::StateManager;
using LlamaInterface = llama_gui::core::LlamaInterface;

/**
 * File manager component for handling file operations
 * Supports drag & drop, file browsing, and attachment management
 */
class FileManager {
public:
    FileManager(StateManager& state_manager, Settings& settings);
    ~FileManager();

    // Main rendering
    void render(bool* visible = nullptr);

    // File operations
    void open_file(const std::string& file_path);
    void add_recent_file(const std::string& file_path);
    void clear_recent_files();

    // Attachment management
    void add_attachment(const std::string& file_path);
    void remove_attachment(const std::string& file_path);
    std::vector<std::string> get_attachments() const { return attachments_; }

    // File dialogs
    void open_file_dialog(const std::string& title, std::function<void(const std::string&)> callback);
    void open_file_dialog(const std::string& title, std::function<void(const std::string&)> callback,
                         const std::string& filter);
    void open_directory_dialog(const std::string& title, std::function<void(const std::string&)> callback);

    // File preview
    bool can_preview(const std::string& file_path) const;
    void render_file_preview(const std::string& file_path);

    // Callback types
    using AttachmentChangedCallback = std::function<void(const std::vector<std::string>&)>;
    using ModelLoadCallback = std::function<void(const std::string&)>;
    using FileContentCallback = std::function<void(const std::string&)>;

    // Callback setters
    void set_attachment_changed_callback(AttachmentChangedCallback callback) {
        on_attachment_changed_ = callback;
    }

    void set_model_load_callback(ModelLoadCallback callback) {
        on_model_load_ = callback;
    }

    void set_file_content_callback(FileContentCallback callback) {
        on_file_content_ = callback;
    }

    // RAG integration
    void set_rag_manager(llama_gui::core::RagManager* rag_manager) {
        rag_manager_ = rag_manager;
    }
    void enable_rag(bool enable) { rag_enabled_ = enable; }

private:
    void render_file_tree();
    void render_browse_files();
    void render_recent_files();
    void render_attachments();
    void render_file_operations();
    void handle_drag_and_drop();

    // Helper methods
    std::string get_file_extension(const std::string& file_path) const;
    std::string format_file_size(long long size) const;

    // UI state
    std::vector<std::string> recent_files_;
    std::vector<std::string> attachments_;
    bool show_file_dialog_ = false;
    std::string current_directory_;

    // Components
    FileDialogHelper dialog_helper_;
    FileBrowser file_browser_;
    RecentFilesManager recent_files_manager_;

    // Callbacks
    AttachmentChangedCallback on_attachment_changed_;
    ModelLoadCallback on_model_load_;
    FileContentCallback on_file_content_;

    // References to core components
    StateManager& state_manager_;
    Settings& settings_;

    // RAG integration
    llama_gui::core::RagManager* rag_manager_ = nullptr;
    bool rag_enabled_ = false;
};

} // namespace ui
} // namespace llama_gui

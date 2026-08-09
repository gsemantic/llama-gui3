#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>
#include <mutex>
#include "core/rag_settings.h"
#include "core/rag_manager.h"

namespace llama_gui {
namespace core {
    class Settings; // Forward declaration
}
}

namespace llama_gui {
namespace ui {

// Forward declaration
class RagSettingsDialog;
class ChatInterface;
class FileDialogManager;

class RagInterface {
public:
    RagInterface();
    ~RagInterface();

    void render_ui(bool* visible = nullptr);
    void set_rag_manager(llama_gui::core::RagManager* rag_manager);
    void set_enabled(bool enabled) { rag_enabled_ = enabled; }
    bool is_enabled() const { return rag_enabled_; }
    void set_rag_settings_dialog(RagSettingsDialog* rag_settings_dialog) { rag_settings_dialog_ = rag_settings_dialog; }
    void update_settings_from_manager();

    // Установка ссылки на ChatInterface для синхронизации
    void set_chat_interface(ChatInterface* chat_interface) { chat_interface_ = chat_interface; }

    // Установка ссылки на Settings для сохранения
    void set_settings(llama_gui::core::Settings* settings) { settings_ = settings; }

    // Диалоги выбора файлов/папок идут через FileDialogManager
    void set_file_dialog_manager(FileDialogManager* mgr) { file_dialog_manager_ = mgr; }

    // Синхронизация состояния RAG с ChatInterface
    void sync_rag_state_with_chat();

    // === Управление профилями индексов ===
    void show_profile_selector();

    // Переиндексация текущего профиля по запросу пользователя (кнопка в уведомлении)
    void reindex_with_notice();

    // === Indexing progress accessors (for mini-indicator in ChatInterface) ===
    bool is_indexing() const { return indexing_active_.load(); }
    float get_indexing_progress() const { return indexing_progress_value_.load(); }
    std::string get_indexing_status() const { return indexing_status_; }
    llama_gui::core::IndexingPhase get_indexing_phase() const { return indexing_phase_; }

private:
    std::vector<std::string> loaded_documents_;
    bool rag_enabled_ = false;
    bool show_rag_panel_ = false;
    float progress_ = 0.0f;
    bool processing_ = false;
    std::string status_message_ = "";
    std::string current_operation_ = "";
    bool show_create_profile_dialog_ = false;  // Флаг диалога создания профиля

    // === Enhanced indexing progress tracking ===
    std::atomic<bool> indexing_active_{false};
    std::atomic<float> indexing_progress_value_{0.0f};
    std::string indexing_status_;
    llama_gui::core::IndexingPhase indexing_phase_ = llama_gui::core::IndexingPhase::Idle;
    llama_gui::core::IndexingProgress last_indexing_progress_;

    // Final statistics (shown after indexing completes)
    bool show_indexing_result_ = false;
    int result_file_count_ = 0;
    int result_chunk_count_ = 0;
    double result_elapsed_seconds_ = 0.0;

    void on_indexing_progress(const llama_gui::core::IndexingProgress& progress);
    void render_indexing_progress();
    void render_indexing_result();

    // Функции обработки
    void handle_document_upload();
    void handle_document_remove(int index);
    void process_uploaded_documents();
    void clear_documents();
    
    // === Персистентность: методы управления индексом ===
    void save_index();
    void load_index();
    void clear_index();
    size_t get_loaded_chunks_count();
    std::string get_persistent_index_path();
    bool persistent_index_exists();

    // Вспомогательные функции
    std::string get_filename_from_path(const std::string& path);
    std::vector<std::string> get_supported_extensions();

    // Callback для обновления прогресса
    std::function<void(float)> progress_callback_;

    // Указатель на RagManager
    llama_gui::core::RagManager* rag_manager_ = nullptr;

    // Указатель на диалог настроек RAG
    RagSettingsDialog* rag_settings_dialog_ = nullptr;
    
    // Указатель на ChatInterface для синхронизации
    ChatInterface* chat_interface_ = nullptr;
    
    // Указатель на Settings для сохранения
    llama_gui::core::Settings* settings_ = nullptr;

    // Указатель на FileDialogManager для гарантированного выбора файлов/папок
    FileDialogManager* file_dialog_manager_ = nullptr;
};

} // namespace ui
} // namespace llama_gui
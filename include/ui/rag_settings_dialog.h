#pragma once

#include <string>

// Используем void* для избежания циклических зависимостей
// В реализации мы будем приводить к правильному типу

namespace llama_gui {
namespace ui {

class MainWindow;  // Forward declaration

class RagSettingsDialog {
public:
    RagSettingsDialog(void* settings, void* main_window = nullptr);
    ~RagSettingsDialog() = default;

    void render();

    bool is_visible() const { return visible_; }
    void set_visible(bool visible) { visible_ = visible; }

    // Set main window pointer for file dialogs
    void set_main_window(void* main_window) { main_window_ptr_ = main_window; }

    // Вызов диалога выбора файла (должен вызываться из основного цикла)
    bool open_embedding_model_file_dialog();
    
    // Получить результат выбора файла
    std::string get_selected_file_path() const { return selected_file_path_; }
    bool has_pending_file_dialog() const { return pending_file_dialog_; }
    void clear_pending_dialog() { pending_file_dialog_ = false; }

private:
    void* settings_ptr_;
    void* main_window_ptr_ = nullptr;  // Pointer to MainWindow for file dialogs
    bool visible_ = false;

    // Временные переменные для редактирования настроек
    char embedding_model_path_buffer_[512];
    int max_chunks_in_memory_;
    float similarity_threshold_;
    int max_embedding_cache_size_;
    int max_tokens_per_chunk_;
    int search_k_;  // Количество результатов поиска RAG
    float mmr_lambda_;  // MMR параметр
    bool enable_mmr_;  // Включить MMR
    bool enable_caching_;
    int rag_mode_;  // 0=DocumentsOnly, 1=CacheOnly, 2=Both

    // Параметры гибридного поиска
    bool enable_hybrid_search_;
    float keyword_boost_weight_;
    bool enable_query_expansion_;

    // Настройки глубокого анализа документа
    int deep_analysis_mode_;  // 0=Disabled, 1=MapReduce, 2=Iterative, 3=Hierarchical
    int deep_analysis_chunks_per_batch_;
    int deep_analysis_max_iterations_;
    bool deep_analysis_enable_progressive_summary_;
    int deep_analysis_final_synthesis_chunks_;
    bool deep_analysis_auto_adjust_context_size_;
    int deep_analysis_target_context_size_;

    // Настройки бесшовного расширения контекста
    bool enable_context_extension_;
    int context_compression_threshold_;
    int keep_recent_messages_;

    // Embedding Proxy settings
    bool enable_embedding_proxy_;
    int embedding_proxy_port_;

    // Для асинхронного диалога
    bool pending_file_dialog_ = false;
    std::string selected_file_path_;
};

} // namespace ui
} // namespace llama_gui
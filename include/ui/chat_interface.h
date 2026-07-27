#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include "core/llama_interface.h"
#include "core/state_manager.h"
#include "core/settings.h"
#include "core/rag_manager.h"
#include "core/context_monitor.h"
#include "core/context_compressor.h"

// Forward declarations
struct ImGuiInputTextCallbackData;
namespace llama_gui { namespace core { class ModelManager; } }
namespace llama_gui { namespace ui { class RagInterface; } }

namespace llama_gui {
namespace ui {

using llama_gui::core::Settings;
using llama_gui::core::StateManager;
using llama_gui::core::LlamaInterface;
using llama_gui::core::ChatMessage;
using llama_gui::core::RagChunk;

// Forward declarations for types from llama_interface.h
namespace core {
    struct ChatCompletionRequest;
    using StreamCallback = std::function<void(const std::string& content, bool is_final)>;
}

// UTF-8 validation function (used in chat_input_handling.cpp)
std::string validate_and_fix_utf8(const std::string& input);

/**
 * Chat interface component for handling conversations
 * Supports message display, input, streaming responses, and file attachments
 */
class ChatInterface {
public:
    ChatInterface(StateManager& state_manager, Settings& settings, LlamaInterface& llama_interface);
    ~ChatInterface();

    // Main rendering
    void render(bool* visible = nullptr);

    // Message management
    void add_user_message(const std::string& content);
    void add_assistant_message(const std::string& content);
    void update_last_message(const std::string& content);

    // File attachments
    void add_file_attachment(const std::string& file_path);
    void remove_attachment(size_t index);
    const std::vector<std::string>& get_attachments() const { return attachments_; }
    void set_attachments(const std::vector<std::string>& attachments);
    
    // Attachment contents access
    const std::unordered_map<std::string, std::string>& get_attachment_contents() const { return attachment_contents_; }
    void clear_attachment_contents() { attachment_contents_.clear(); }
    std::string build_attachments_context() const;  // Построение контекста из всех файлов

    // Model selection
    void open_model_selection_dialog();
    void set_model_selection_callback(std::function<void()> callback);
    
    // Model load callback - вызывается когда модель не загружена при отправке запроса
    using ModelLoadRequestCallback = std::function<void(const std::string& pending_query)>;
    void set_model_load_request_callback(ModelLoadRequestCallback callback);
    
    // Model loading state access
    std::string get_pending_query_for_loading() const { return pending_query_for_loading_; }
    void clear_pending_query_for_loading() { pending_query_for_loading_.clear(); }

    // Input handling
    bool is_input_focused() const { return input_focused_; }
    void focus_input() { input_focused_ = true; }
    void unfocus_input() { input_focused_ = false; }
    void set_input_text(const std::string& text);

    // Conversation title generation
    static std::string generate_conversation_title(const std::string& message_content, size_t max_length = 30);

    // Input selection accessors for callback
    int get_input_cursor_pos() const { return input_cursor_pos_; }
    int get_input_selection_start() const { return input_selection_start_; }
    int get_input_selection_end() const { return input_selection_end_; }
    const std::string& get_input_selected_text() const { return input_selected_text_; }
    void set_input_cursor_pos(int pos) { input_cursor_pos_ = pos; }
    void set_input_selection_start(int start) { input_selection_start_ = start; }
    void set_input_selection_end(int end) { input_selection_end_ = end; }
    void set_input_selected_text(const std::string& text) { input_selected_text_ = text; }

    // RAG methods
    void set_rag_manager(llama_gui::core::RagManager* rag_manager) { 
        rag_manager_ = rag_manager;
        context_compressor_.set_rag_manager(rag_manager);
    }
    void enable_rag(bool enable) {
        rag_enabled_ = enable;
        settings_.rag().enable_rag = enable;
    }
    bool is_rag_enabled() const { return rag_enabled_; }
    void set_window_manager(class WindowManager* wm) { window_manager_ = wm; }
    std::string process_with_rag(const std::string& user_input, bool use_cache = true);
    void cache_current_interaction(const std::string& query, const std::string& response);

    // Context extension methods
    void enable_context_compression(bool enable) { context_compression_enabled_ = enable; }
    bool is_context_compression_enabled() const { return context_compression_enabled_; }
    llama_gui::core::ContextStats get_context_stats() const { return context_monitor_.get_stats(); }
    
    // RAG Prompt Caching - кэширование на основе полного промпта (вопрос + контекст)
    bool check_rag_prompt_cache(const std::string& rag_prompt, std::string& cached_response);
    void update_rag_prompt_cache(const std::string& rag_prompt, const std::string& response, int tokens);

    // Model manager access
    void set_model_manager(llama_gui::core::ModelManager* model_manager) { model_manager_ = model_manager; }
    void set_is_model_loading(std::atomic<bool>* is_loading) { is_model_loading_ = is_loading; }
    bool is_model_loading() const { return is_model_loading_ && is_model_loading_->load(); }
    void set_server_ready(bool ready) { server_ready_ = ready; }
    bool is_server_ready() const { return server_ready_; }

    // RagInterface access for mini indicator
    void set_rag_interface(RagInterface* rag_interface) { rag_interface_ = rag_interface; }
    
    // Model loading progress access (для отображения в чате)
    void set_model_load_progress(float* progress) { model_load_progress_ = progress; }
    void set_model_load_status(std::string* status) { model_load_status_ = status; }

    // Streaming control
    bool is_streaming() const { return streaming_active_; }
    void start_streaming();
    void stop_streaming();

    // Settings access
    const Settings& get_settings() const { return settings_; }

private:
    void render_message_list();
    void render_input_area();
    void render_attachments();
    void render_typing_indicator();
    void render_rag_mini_indicator();

    void send_message();
    void send_message_via_openrouter();
    void clear_input();
    void scroll_to_bottom();
    void process_pending_responses();

    // Callback for streaming responses
    void on_stream_chunk(const std::string& chunk, bool is_final);
    static void streaming_callback(void* user_data, const std::string& chunk, bool is_final);

    // UI state
    char input_buffer_[4096];
    bool input_focused_ = false;
    bool streaming_active_ = false;
    bool processing_rag_document_ = false;  // Флаг обработки RAG документа
    std::string current_stream_content_;
    std::string selected_text_; // For storing selected text from messages
    std::string input_selected_text_; // For storing selected text in input field
    int input_cursor_pos_ = 0;
    int input_selection_start_ = 0;
    int input_selection_end_ = 0;
    std::vector<std::string> attachments_;
    std::unordered_map<std::string, std::string> attachment_contents_;  // Содержимое файлов

    // Thread synchronization
    std::mutex streaming_mutex_;

    // Callbacks
    std::function<void()> model_selection_callback_;
    ModelLoadRequestCallback model_load_request_callback_;

    // Pending query during model loading
    std::string pending_query_for_loading_;

    // Async response handling
    struct PendingResponse {
        std::string content;
        std::string conversation_id;
    };
    std::vector<PendingResponse> pending_responses_;
    std::mutex pending_responses_mutex_;

    // References to core components
    StateManager& state_manager_;
    Settings& settings_;
    LlamaInterface& llama_interface_;
    llama_gui::core::ModelManager* model_manager_ = nullptr;  // Model manager access
    std::atomic<bool>* is_model_loading_ = nullptr;  // Флаг загрузки модели
    bool server_ready_ = false;  // Сервер готов и модель загружена
    float* model_load_progress_ = nullptr;  // Прогресс загрузки
    std::string* model_load_status_ = nullptr;  // Статус загрузки

    // RAG components
    llama_gui::core::RagManager* rag_manager_ = nullptr;
    RagInterface* rag_interface_ = nullptr;
    bool rag_enabled_ = false;

    // Window manager for dock support
    class WindowManager* window_manager_ = nullptr;

    // Context extension components
    llama_gui::core::ContextMonitor context_monitor_;
    llama_gui::core::ContextCompressor context_compressor_;
    bool context_compression_enabled_ = true;
    
    // Scroll position tracking
    bool auto_scroll_ = true;
    float scroll_to_message_id_ = -1.0f;

    // Performance metrics
    struct PerformanceMetrics {
        double response_time_seconds = 0.0;
        int tokens_generated = 0;
        double tokens_per_second = 0.0;
        int remaining_context = 0;
        int total_context = 0;
        int context_used = 0;  // Tokens used by conversation history + generated tokens
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point end_time;
        bool is_measuring = false;
    };

    PerformanceMetrics current_metrics_;
    void update_performance_metrics(const std::string& content, bool is_final);
    void render_performance_metrics();

    // Prompt Caching - кэширование идентичных запросов
    struct PromptCacheEntry {
        std::string response;
        int tokens_saved = 0;
        std::chrono::steady_clock::time_point created_at;
        int hit_count = 0;  // Сколько раз было использовано
    };

    // Статистика кэширования
    struct TokenCacheStats {
        int total_requests = 0;           // Всего запросов
        int prompt_cache_hits = 0;        // Попаданий в prompt cache
        int rag_cache_hits = 0;           // Попаданий в RAG cache
        int total_tokens_generated = 0;   // Сгенерировано токенов
        int total_tokens_saved = 0;       // Сэкономлено токенов
        std::chrono::steady_clock::time_point session_start;

        double get_cache_hit_rate() const {
            if (total_requests == 0) return 0.0;
            return 100.0 * (prompt_cache_hits + rag_cache_hits) / total_requests;
        }

        double get_token_savings_rate() const {
            int total = total_tokens_generated + total_tokens_saved;
            if (total == 0) return 0.0;
            return 100.0 * total_tokens_saved / total;
        }
    };

    TokenCacheStats cache_stats_;

    // Prompt cache методы
    std::unordered_map<size_t, PromptCacheEntry> prompt_cache_;  // hash → entry
    mutable std::shared_mutex prompt_cache_mutex_;
    
    bool check_prompt_cache(const std::string& prompt, std::string& cached_response);
    void update_prompt_cache(const std::string& prompt, const std::string& response, int tokens);
    void reset_cache_stats();
    size_t compute_prompt_hash(const std::string& prompt);
    
    // UI methods for cache statistics
    void render_cache_stats(bool* visible);

    // Text formatting cache for performance optimization
    struct CachedMessage {
        std::string original_content;
        std::string* formatted_content_pooled = nullptr;
        size_t content_hash;
        std::chrono::steady_clock::time_point last_used;
    };

    std::unordered_map<std::string, std::vector<CachedMessage>> message_cache_; // conversation_id -> cached messages
    mutable std::shared_mutex cache_mutex_; // OPTIMIZATION: shared_mutex for read/write separation

    // Cache management
    std::string get_cached_formatted_text(const std::string& conversation_id, size_t message_index, const std::string& content);
    void invalidate_cache_for_conversation(const std::string& conversation_id);
    void cleanup_old_cache_entries();
    static size_t compute_content_hash(const std::string& content);

    // Memory pooling for performance optimization
    // OPTIMIZATION: Use shared_mutex and pre-allocated pool
    struct StringPool {
        std::vector<std::string> pool;
        mutable std::shared_mutex pool_mutex;

        StringPool() {
            // OPTIMIZATION: Pre-allocate pool with reasonable size
            pool.reserve(32);
            for (int i = 0; i < 16; ++i) {
                pool.emplace_back();
                pool.back().reserve(256); // Pre-reserve reasonable capacity
            }
        }

        std::string* acquire(size_t min_capacity = 0) {
            // OPTIMIZATION: Try read-only fast path first
            {
                std::shared_lock<std::shared_mutex> read_lock(pool_mutex);
                for (auto& str : pool) {
                    if (str.capacity() >= min_capacity && str.empty()) {
                        return &str;
                    }
                }
            }
            
            // Need to create new string - acquire write lock
            std::unique_lock<std::shared_mutex> write_lock(pool_mutex);
            
            // Double-check after acquiring write lock
            for (auto& str : pool) {
                if (str.capacity() >= min_capacity && str.empty()) {
                    return &str;
                }
            }
            
            // Create new string if none available
            pool.emplace_back();
            pool.back().reserve(min_capacity > 0 ? min_capacity : 256);
            return &pool.back();
        }

        void release(std::string* str) {
            if (str) {
                str->clear();
            }
        }

        void cleanup() {
            std::unique_lock<std::shared_mutex> lock(pool_mutex);
            // OPTIMIZATION: Keep pool size stable, just clear strings
            size_t keep_count = std::min(pool.size(), static_cast<size_t>(32));
            if (pool.size() > keep_count) {
                pool.resize(keep_count);
            }
            // Clear all strings but keep capacity
            for (auto& str : pool) {
                str.clear();
            }
        }
    };

    StringPool string_pool_;
};

} // namespace ui
} // namespace llama_gui
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <future>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>

#include "llama_interface.h"

namespace llama_gui {
namespace core {

using json = nlohmann::json;

// Using declarations to link impl namespace with core namespace
namespace impl {
    using ChatCompletionRequest = llama_gui::core::ChatCompletionRequest;
    using ChatCompletionResponse = llama_gui::core::ChatCompletionResponse;
    using EmbeddingRequest = llama_gui::core::EmbeddingRequest;
    using EmbeddingResponse = llama_gui::core::EmbeddingResponse;
}

/**
 * @brief Streaming callback for chat completion
 */
using StreamCallback = std::function<void(const std::string& chunk, bool is_final)>;
using HttpResponseCallback = std::function<void(const std::string& response)>;

// Forward declaration for callback
using ChatCompletionCallback = std::function<void(const ChatCompletionResponse& response)>;

/**
 * @brief Internal implementation of LlamaInterface
 *
 * Contains all implementation details with namespace isolation
 * to avoid conflicts and improve code organization.
 */
namespace impl {

// Slot operation result structure

// Slot operation result structure

// ============================================================================
// Slot operation result structure
// ============================================================================

struct SlotOperationResult {
    bool success = false;
    int slot_id = -1;
    std::string filename;
    int n_tokens = 0;
    int n_bytes = 0;
    double processing_ms = 0.0;
    std::string error_message;

    SlotOperationResult() = default;
};

// ============================================================================
// Streaming data structures
// ============================================================================

struct StreamingData {
    std::string buffer;
    StreamCallback callback;
    std::atomic<bool> completed{false};
};

struct StreamingRequestData {
    std::string buffer;
    HttpResponseCallback callback;
    std::mutex buffer_mutex;
    std::condition_variable cv;
    std::atomic<bool> is_active{true};
    std::atomic<bool> completed{false};
    std::string response;
};

// ============================================================================
// LlamaInterfaceImpl - Main implementation class
// ============================================================================

class LlamaInterfaceImpl {
public:
    explicit LlamaInterfaceImpl(const std::string& server_url);
    ~LlamaInterfaceImpl();

    // Initialize connection
    bool initialize(const std::string& server_url);

    // Basic operations
    bool is_server_healthy() const;
    json get_server_info() const;
    json get_models() const;
    json get_slots_status() const;

    // Chat completion
    void create_chat_completion_streaming(const ChatCompletionRequest& request, StreamCallback callback);
    std::future<ChatCompletionResponse> create_chat_completion_async(const ChatCompletionRequest& request);

    // Embedding
    EmbeddingResponse create_embedding(const EmbeddingRequest& request);

    // KV-cache management
    bool save_slot_kv_cache(int slot_id, const std::string& filename);
    bool restore_slot_kv_cache(int slot_id, const std::string& filename);
    SlotOperationResult save_slot_kv_cache_detailed(int slot_id, const std::string& filename);
    SlotOperationResult restore_slot_kv_cache_detailed(int slot_id, const std::string& filename);
    bool reset_slot(int slot_id);
    bool erase_slot(int slot_id);
    SlotOperationResult tokenize_text_in_slot(int slot_id, const std::string& text);
    
    // Async HTTP requests
    void make_async_http_request(const std::string& endpoint, const std::string& method, const json& data, HttpResponseCallback callback);
    void make_streaming_http_request(const std::string& endpoint, const std::string& method, const json& data, StreamCallback callback);

    // Stop streaming requests
    void stop_streaming_requests();

    // Chat completion with callback
    void create_chat_completion_async_callback(const ChatCompletionRequest& request, ChatCompletionCallback callback);

    // Internal utilities
    std::string make_http_request(const std::string& endpoint, const std::string& method, const json& data) const;
    bool parse_streaming_response(const std::string& response, StreamCallback callback) const;
    void process_async_requests();

    // Static utilities
    static std::string validate_and_clean_utf8(const std::string& input);
    static llama_gui::core::json extract_json_from_response(const std::string& response);

    // Public getters/setters
    const std::string& get_server_url() const { return server_url_; }
    void set_server_url(const std::string& url) { server_url_ = url; }
    void set_api_key(const std::string& key) { api_key_ = key; }
    int get_timeout() const { return timeout_seconds_; }
    void set_timeout(int seconds) { timeout_seconds_ = seconds; }

    // Проверка SSL-сертификатов для https:// подключений к бэкенду.
    // По умолчанию отключена (локальные сценарии, см. UI-аудит).
    void set_ssl_verify(bool verify) { ssl_verify_ = verify; }
    bool get_ssl_verify() const { return ssl_verify_; }

private:
    // Применить настройки SSL к curl-хендлу (VERIFYPEER/VERIFYHOST)
    void apply_ssl_options(CURL* curl) const;

    // Connection state
    std::string server_url_;
    std::string api_key_;
    int timeout_seconds_;
    bool ssl_verify_ = false;
    
    // CURL handle
    CURL* curl_handle_;
    bool curl_initialized_;
    
    // Streaming request tracking
    std::atomic<bool> streaming_active_{false};
    std::mutex streaming_mutex_;
    std::vector<std::shared_ptr<StreamingData>> active_streams_;
    
    // Async request tracking
    std::mutex responses_mutex_;
    std::queue<std::pair<std::string, HttpResponseCallback>> pending_responses_;
    std::thread async_processing_thread_;
    std::atomic<bool> async_processing_running_{false};
    
    // HTTP response callback
    ChatCompletionCallback async_callback_;
    
    // Slot management
    std::atomic<int> next_slot_id_{0};
    
    // Internal methods
    void initialize_curl();
    void cleanup_curl();
    
    // Slot operations
    bool save_slot_kv_cache_impl(int slot_id, const std::string& filename);
    bool restore_slot_kv_cache_impl(int slot_id, const std::string& filename);
    bool reset_slot_impl(int slot_id);
    bool erase_slot_impl(int slot_id);
    SlotOperationResult tokenize_text_impl(int slot_id, const std::string& text);
    
    // Slot KV-cache file operations
    bool save_kv_cache_file(int slot_id, const std::string& filename);
    bool load_kv_cache_file(int slot_id, const std::string& filename);
    std::string get_kv_cache_dir() const;
};

} // namespace impl

} // namespace core
} // namespace llama_gui

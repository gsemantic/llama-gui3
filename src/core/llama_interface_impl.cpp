#include "../include/core/llama_interface_impl.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace llama_gui {
namespace core {
namespace impl {

using json = nlohmann::json;

// SlotOperationResult is already defined in header

LlamaInterfaceImpl::LlamaInterfaceImpl(const std::string& server_url)
    : server_url_(server_url)
    , api_key_("")
    , timeout_seconds_(120)  // 120s for local CPU models
    , curl_handle_(nullptr)
    , curl_initialized_(false)
    , streaming_active_(false)
    , next_slot_id_(0)
{
    initialize_curl();
}

LlamaInterfaceImpl::~LlamaInterfaceImpl()
{
    cleanup_curl();
    stop_streaming_requests();

    if (async_processing_thread_.joinable()) {
        async_processing_thread_.join();
    }
}

bool impl::LlamaInterfaceImpl::initialize(const std::string& server_url)
{
    server_url_ = server_url;
    return is_server_healthy();
}

bool impl::LlamaInterfaceImpl::is_server_healthy() const
{
    if (!curl_initialized_) {
        return false;
    }

    // Use a separate, short-lived handle for health checks to avoid blocking the UI
    CURL* health_handle = curl_easy_init();
    if (!health_handle) {
        return false;
    }

    std::string health_url = server_url_ + "/health";
    long http_code = 0;

    curl_easy_setopt(health_handle, CURLOPT_URL, health_url.c_str());
    curl_easy_setopt(health_handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(health_handle, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(health_handle, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(health_handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(health_handle, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(health_handle, CURLOPT_WRITEFUNCTION, +[](char*, size_t, size_t, void*) -> size_t { return 0; });

    CURLcode res = curl_easy_perform(health_handle);

    if (res == CURLE_OK) {
        curl_easy_getinfo(health_handle, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_easy_cleanup(health_handle);

    // Server is healthy only if it responds with 200 OK
    return (res == CURLE_OK && http_code == 200);
}

llama_gui::core::json llama_gui::core::impl::LlamaInterfaceImpl::get_server_info() const
{
    json info;
    info["server_url"] = server_url_;
    info["timeout"] = timeout_seconds_;
    info["api_key_set"] = !api_key_.empty();
    return info;
}

llama_gui::core::json llama_gui::core::impl::LlamaInterfaceImpl::get_models() const
{
    // TODO: Implement actual model listing
    json result;
    result["models"] = json::array();
    return result;
}

llama_gui::core::json llama_gui::core::impl::LlamaInterfaceImpl::get_slots_status() const
{
    // TODO: Implement actual slot status
    json result;
    result["slots"] = json::array();
    result["next_slot_id"] = next_slot_id_.load();
    return result;
}

void impl::LlamaInterfaceImpl::create_chat_completion_streaming(
    const ChatCompletionRequest& request,
    StreamCallback callback)
{
    // Build JSON request body
    json body;
    body["model"] = request.model;
    body["stream"] = true;
    body["max_tokens"] = request.max_tokens;
    body["temperature"] = request.temperature;
    body["top_p"] = request.top_p;
    body["top_k"] = request.top_k;
    body["min_p"] = request.min_p;
    body["repeat_penalty"] = request.repeat_penalty;
    body["presence_penalty"] = request.presence_penalty;
    body["frequency_penalty"] = request.frequency_penalty;
    body["mirostat"] = request.mirostat_mode;
    body["mirostat_tau"] = request.mirostat_tau;
    body["mirostat_eta"] = request.mirostat_eta;

    // Build messages array
    json messages = json::array();
    for (const auto& msg : request.messages) {
        json m;
        switch (msg.role) {
            case MessageRole::User:      m["role"] = "user"; break;
            case MessageRole::Assistant: m["role"] = "assistant"; break;
            case MessageRole::System:    m["role"] = "system"; break;
        }
        m["content"] = msg.content;
        messages.push_back(m);
    }
    body["messages"] = messages;

    if (!request.stop.empty()) {
        body["stop"] = request.stop;
    }

    std::string post_fields = body.dump();
    std::string url = server_url_ + "/v1/chat/completions";

    streaming_active_ = true;

    // Run streaming request in a separate thread to not block UI
    std::thread([this, url, post_fields, callback]() {
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "[LlamaInterface] Failed to init curl for streaming" << std::endl;
            callback("", true);
            streaming_active_ = false;
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields.c_str());
        // Streaming: no timeout at all - connection lives until [DONE] or server closes it
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!api_key_.empty()) {
            std::string auth = "Authorization: Bearer " + api_key_;
            headers = curl_slist_append(headers, auth.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Streaming write callback: parse SSE lines and call the StreamCallback
        struct StreamContext {
            StreamCallback callback;
            std::string line_buffer;
        };

        StreamContext ctx;
        ctx.callback = callback;

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                size_t total = size * nmemb;
                auto* sc = static_cast<StreamContext*>(userdata);
                sc->line_buffer.append(ptr, total);

                // Process complete SSE lines
                while (true) {
                    size_t pos = sc->line_buffer.find('\n');
                    if (pos == std::string::npos) break;

                    std::string line = sc->line_buffer.substr(0, pos);
                    sc->line_buffer.erase(0, pos + 1);

                    // Trim carriage return
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }

                    // Skip empty lines and non-data lines
                    if (line.empty()) continue;
                    if (line.find("data: ") != 0) continue;

                    std::string data = line.substr(6);
                    if (data == "[DONE]") {
                        return total;
                    }

                    // Pass the raw JSON data to the callback
                    // (the caller's callback expects to parse it itself)
                    sc->callback(data, false);
                }
                return total;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

        // Store curl handle so stop_streaming_requests can cancel it
        {
            std::lock_guard<std::mutex> lock(streaming_mutex_);
            // We store the raw pointer temporarily for cancellation
            // The thread owns the handle
        }

        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
            std::cerr << "[LlamaInterface] Streaming curl error: "
                      << curl_easy_strerror(res) << std::endl;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        // Signal completion
        callback("", true);
        streaming_active_ = false;
    }).detach();
}

std::future<ChatCompletionResponse> impl::LlamaInterfaceImpl::create_chat_completion_async(
    const ChatCompletionRequest& request)
{
    auto promise = std::make_shared<std::promise<ChatCompletionResponse>>();

    // Build JSON request body
    json body;
    body["model"] = request.model;
    body["stream"] = false;
    body["max_tokens"] = request.max_tokens;
    body["temperature"] = request.temperature;
    body["top_p"] = request.top_p;
    body["top_k"] = request.top_k;

    json messages = json::array();
    for (const auto& msg : request.messages) {
        json m;
        switch (msg.role) {
            case MessageRole::User:      m["role"] = "user"; break;
            case MessageRole::Assistant: m["role"] = "assistant"; break;
            case MessageRole::System:    m["role"] = "system"; break;
        }
        m["content"] = msg.content;
        messages.push_back(m);
    }
    body["messages"] = messages;

    std::string post_fields = body.dump();
    std::string url = server_url_ + "/v1/chat/completions";

    std::thread([this, url, post_fields, promise]() {
        CURL* curl = curl_easy_init();
        ChatCompletionResponse response;

        if (!curl) {
            response.choices.push_back({});
            response.choices[0].finish_reason = "error";
            promise->set_value(response);
            return;
        }

        std::string response_body;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_seconds_);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!api_key_.empty()) {
            headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key_).c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                auto* body = static_cast<std::string*>(userdata);
                body->append(ptr, size * nmemb);
                return size * nmemb;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

        CURLcode res = curl_easy_perform(curl);

        if (res == CURLE_OK) {
            try {
                auto j = nlohmann::json::parse(response_body);
                if (j.contains("choices") && !j["choices"].empty()) {
                    auto& choice = j["choices"][0];
                    response.id = j.value("id", "");
                    response.model = j.value("model", "");
                    if (choice.contains("message")) {
                        response.choices.push_back({});
                        response.choices[0].message.content = choice["message"].value("content", "");
                        response.choices[0].message.role = MessageRole::Assistant;
                        response.choices[0].finish_reason = choice.value("finish_reason", "stop");
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[LlamaInterface] JSON parse error: " << e.what() << std::endl;
            }
        } else {
            std::cerr << "[LlamaInterface] Async request failed: " << curl_easy_strerror(res) << std::endl;
            response.choices.push_back({});
            response.choices[0].finish_reason = "error";
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        promise->set_value(response);
    }).detach();

    return promise->get_future();
}

EmbeddingResponse impl::LlamaInterfaceImpl::create_embedding(const EmbeddingRequest& request)
{
    // TODO: Implement embedding generation
    std::cerr << "Embedding generation not yet implemented" << std::endl;

    EmbeddingResponse response;
    response.object = "embedding";
    response.data = {llama_gui::core::EmbeddingResponse::EmbeddingData{}};
    return response;
}

bool impl::LlamaInterfaceImpl::save_slot_kv_cache(int slot_id, const std::string& filename)
{
    return save_slot_kv_cache_detailed(slot_id, filename).success;
}

bool impl::LlamaInterfaceImpl::restore_slot_kv_cache(int slot_id, const std::string& filename)
{
    return restore_slot_kv_cache_detailed(slot_id, filename).success;
}

impl::SlotOperationResult impl::LlamaInterfaceImpl::save_slot_kv_cache_detailed(
    int slot_id, const std::string& filename)
{
    impl::SlotOperationResult result;
    result.slot_id = slot_id;
    result.filename = filename;
    
    try {
        if (!save_kv_cache_file(slot_id, filename)) {
            result.success = false;
            result.error_message = "Failed to save KV-cache file";
            return result;
        }
        
        result.success = true;
        result.n_bytes = std::filesystem::file_size(filename);
        result.processing_ms = 0.0; // TODO: Measure actual time
        return result;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
        return result;
    }
}

impl::SlotOperationResult impl::LlamaInterfaceImpl::restore_slot_kv_cache_detailed(
    int slot_id, const std::string& filename)
{
    impl::SlotOperationResult result;
    result.slot_id = slot_id;
    result.filename = filename;
    
    try {
        if (!load_kv_cache_file(slot_id, filename)) {
            result.success = false;
            result.error_message = "Failed to load KV-cache file";
            return result;
        }
        
        result.success = true;
        result.n_bytes = std::filesystem::file_size(filename);
        result.processing_ms = 0.0; // TODO: Measure actual time
        return result;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
        return result;
    }
}

bool impl::LlamaInterfaceImpl::reset_slot(int slot_id)
{
    return reset_slot_impl(slot_id);
}

bool impl::LlamaInterfaceImpl::erase_slot(int slot_id)
{
    return erase_slot_impl(slot_id);
}

impl::SlotOperationResult impl::LlamaInterfaceImpl::tokenize_text_in_slot(
    int slot_id, const std::string& text)
{
    return tokenize_text_impl(slot_id, text);
}

void impl::LlamaInterfaceImpl::make_async_http_request(
    const std::string& endpoint,
    const std::string& method,
    const json& data,
    HttpResponseCallback callback)
{
    // TODO: Implement async HTTP request
    std::cerr << "Async HTTP request not yet implemented" << std::endl;
    
    if (callback) {
        callback("");
    }
}

void impl::LlamaInterfaceImpl::make_streaming_http_request(
    const std::string& endpoint,
    const std::string& method,
    const json& data,
    StreamCallback callback)
{
    // TODO: Implement streaming HTTP request
    std::cerr << "Streaming HTTP request not yet implemented" << std::endl;
}

void impl::LlamaInterfaceImpl::stop_streaming_requests()
{
    streaming_active_ = false;
    
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    for (auto& stream : active_streams_) {
        stream->completed = true;
    }
    active_streams_.clear();
}

void impl::LlamaInterfaceImpl::create_chat_completion_async_callback(
    const ChatCompletionRequest& request,
    ChatCompletionCallback callback)
{
    // TODO: Implement async callback version
    // For now, use the async version
    auto future = create_chat_completion_async(request);

    std::thread([future = std::move(future), callback]() mutable {
        try {
            auto response = future.get();
            callback(response);
        } catch (const std::exception& e) {
            ChatCompletionResponse error_response;
            error_response.choices.push_back({});
            error_response.choices[0].finish_reason = "error";
            error_response.choices[0].message.content = e.what();
            callback(error_response);
        }
    }).detach();
}

std::string llama_gui::core::impl::LlamaInterfaceImpl::make_http_request(
    const std::string& endpoint,
    const std::string& method,
    const json& data) const
{
    // TODO: Implement HTTP request
    std::cerr << "HTTP request not yet implemented" << std::endl;
    return "";
}

bool impl::LlamaInterfaceImpl::parse_streaming_response(
    const std::string& response,
    StreamCallback callback) const
{
    // TODO: Parse streaming response
    return false;
}

void impl::LlamaInterfaceImpl::process_async_requests()
{
    // TODO: Implement async request processing
}

void impl::LlamaInterfaceImpl::initialize_curl()
{
    if (curl_initialized_) {
        return;
    }

    curl_handle_ = curl_easy_init();
    if (!curl_handle_) {
        std::cerr << "Failed to initialize CURL" << std::endl;
        return;
    }
    
    // Set default options
    curl_easy_setopt(curl_handle_, CURLOPT_TIMEOUT, timeout_seconds_);
    curl_easy_setopt(curl_handle_, CURLOPT_NOSIGNAL, 1L);
    
    if (!api_key_.empty()) {
        curl_easy_setopt(curl_handle_, CURLOPT_HTTPHEADER, 
            curl_slist_append(nullptr, 
                std::string("Authorization: Bearer " + api_key_).c_str()));
    }
    
    curl_initialized_ = true;
}

void impl::LlamaInterfaceImpl::cleanup_curl()
{
    if (curl_handle_) {
        curl_easy_cleanup(curl_handle_);
        curl_handle_ = nullptr;
    }
    curl_initialized_ = false;
}

bool impl::LlamaInterfaceImpl::save_kv_cache_file(int slot_id, const std::string& filename)
{
    try {
        std::string kv_cache_dir = get_kv_cache_dir();
        if (!fs::exists(kv_cache_dir)) {
            fs::create_directories(kv_cache_dir);
        }

        std::string filepath = kv_cache_dir + "/" + filename;

        // TODO: Actually save KV-cache data
        std::ofstream ofs(filepath, std::ios::binary);
        if (!ofs) {
            return false;
        }

        // Write placeholder data
        ofs.write("KV-CACHE PLACEHOLDER", 18);

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving KV-cache file: " << e.what() << std::endl;
        return false;
    }
}

bool impl::LlamaInterfaceImpl::load_kv_cache_file(int slot_id, const std::string& filename)
{
    try {
        std::string kv_cache_dir = get_kv_cache_dir();
        std::string filepath = kv_cache_dir + "/" + filename;

        if (!fs::exists(filepath)) {
            return false;
        }

        // TODO: Actually load KV-cache data
        std::ifstream ifs(filepath, std::ios::binary);
        if (!ifs) {
            return false;
        }

        // Read placeholder data
        char buffer[20];
        ifs.read(buffer, sizeof(buffer));

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error loading KV-cache file: " << e.what() << std::endl;
        return false;
    }
}

std::string llama_gui::core::impl::LlamaInterfaceImpl::get_kv_cache_dir() const
{
    // TODO: Make this configurable
    return "./kv_cache";
}

bool impl::LlamaInterfaceImpl::reset_slot_impl(int slot_id)
{
    // TODO: Implement slot reset
    return true;
}

bool impl::LlamaInterfaceImpl::erase_slot_impl(int slot_id)
{
    // TODO: Implement slot erase
    return true;
}

impl::SlotOperationResult impl::LlamaInterfaceImpl::tokenize_text_impl(int slot_id, const std::string& text)
{
    SlotOperationResult result;
    result.slot_id = slot_id;

    try {
        // TODO: Implement actual tokenization
        result.n_tokens = static_cast<int>(text.size() / 4); // Placeholder
        result.n_bytes = text.size();
        result.success = true;
        return result;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
        return result;
    }
}

} // namespace impl
} // namespace core
} // namespace llama_gui

// ============================================================================
// Static utility methods
// ============================================================================

std::string llama_gui::core::impl::LlamaInterfaceImpl::validate_and_clean_utf8(const std::string& input)
{
    // TODO: Implement UTF-8 validation and cleaning
    return input;
}

llama_gui::core::json llama_gui::core::impl::LlamaInterfaceImpl::extract_json_from_response(const std::string& response)
{
    // TODO: Implement JSON extraction
    try {
        return llama_gui::core::json::parse(response);
    } catch (const llama_gui::core::json::exception& e) {
        return llama_gui::core::json::object();
    }
}

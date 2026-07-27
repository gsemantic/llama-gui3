#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace llama_gui {
namespace core {

/**
 * Lightweight HTTP proxy server that exposes an OpenAI-compatible /v1/embeddings
 * endpoint backed by the local embedding server (llama.cpp or any backend).
 *
 * Cloud models (OpenAI, Cohere, etc.) can send embedding requests to this proxy
 * and receive vectors in the local embedding space. The proxy translates between
 * the OpenAI API format and the local server's format.
 *
 * Usage:
 *   EmbeddingProxy proxy("http://localhost:8081", 8082);
 *   proxy.start();  // Listens on port 8082
 *   // Cloud model sends POST http://localhost:8082/v1/embeddings
 *   proxy.stop();
 */
class EmbeddingProxy {
public:
    /**
     * @param local_server_url  URL of the local embedding server (e.g. http://localhost:8081)
     * @param proxy_port        Port to listen on (default 8082)
     */
    EmbeddingProxy(const std::string& local_server_url, int proxy_port = 8082);
    ~EmbeddingProxy();

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }
    int get_port() const { return port_; }
    std::string get_local_server_url() const { return local_server_url_; }

    // Set callback for logging
    using LogCallback = std::function<void(const std::string& message)>;
    void set_log_callback(LogCallback callback) { log_callback_ = callback; }

private:
    std::string local_server_url_;
    int port_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    int server_socket_ = -1;

    LogCallback log_callback_;

    void server_loop();
    void handle_client(int client_socket);
    std::string read_http_request(int client_socket);
    void send_http_response(int client_socket, int status_code,
                            const std::string& status_text,
                            const std::string& body,
                            const std::string& content_type = "application/json");

    // Proxy the /v1/embeddings request to the local server
    std::string proxy_embeddings_request(const std::string& request_body);

    // Parse OpenAI format request body
    struct EmbeddingRequest {
        std::string input;
        std::string model;
        std::string encoding_format; // "float" (default) or "base64"
    };

    EmbeddingRequest parse_request_body(const std::string& body);

    // Forward request to local server via CURL
    std::string forward_to_local_server(const std::string& path, const std::string& body);

    // Build OpenAI-compatible response from local server response
    std::string build_openai_response(const std::string& local_response, int request_count);

    void log(const std::string& msg);
};

} // namespace core
} // namespace llama_gui

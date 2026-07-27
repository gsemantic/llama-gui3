#include "../include/core/embedding_proxy.h"

#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <chrono>

#ifdef USE_CURL
#include <curl/curl.h>
#endif

// POSIX sockets
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace llama_gui {
namespace core {

EmbeddingProxy::EmbeddingProxy(const std::string& local_server_url, int proxy_port)
    : local_server_url_(local_server_url)
    , port_(proxy_port) {
    // Ensure local_server_url doesn't end with /
    while (!local_server_url_.empty() && local_server_url_.back() == '/') {
        local_server_url_.pop_back();
    }
}

EmbeddingProxy::~EmbeddingProxy() {
    stop();
}

bool EmbeddingProxy::start() {
    if (running_.load()) {
        log("Proxy already running on port " + std::to_string(port_));
        return true;
    }

    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        log("ERROR: Failed to create socket");
        return false;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (bind(server_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log("ERROR: Failed to bind to port " + std::to_string(port_));
        close(server_socket_);
        server_socket_ = -1;
        return false;
    }

    if (listen(server_socket_, 16) < 0) {
        log("ERROR: Failed to listen on socket");
        close(server_socket_);
        server_socket_ = -1;
        return false;
    }

    running_.store(true);
    server_thread_ = std::thread(&EmbeddingProxy::server_loop, this);

    log("Embedding Proxy started on port " + std::to_string(port_) +
        " -> forwarding to " + local_server_url_);
    return true;
}

void EmbeddingProxy::stop() {
    if (!running_.load()) return;

    running_.store(false);

    // Close server socket to unblock accept()
    if (server_socket_ >= 0) {
        shutdown(server_socket_, SHUT_RDWR);
        close(server_socket_);
        server_socket_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    log("Embedding Proxy stopped");
}

void EmbeddingProxy::server_loop() {
    while (running_.load()) {
        // Use poll() with timeout so we can check running_ periodically
        struct pollfd pfd;
        pfd.fd = server_socket_;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int poll_result = poll(&pfd, 1, 500); // 500ms timeout
        if (poll_result <= 0) {
            continue; // timeout or error, check running_
        }

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (running_.load()) {
                log("WARNING: accept() failed");
            }
            continue;
        }

        // Handle client in a detached thread for concurrency
        std::thread([this, client_socket]() {
            handle_client(client_socket);
        }).detach();
    }
}

void EmbeddingProxy::handle_client(int client_socket) {
    std::string raw_request = read_http_request(client_socket);
    if (raw_request.empty()) {
        close(client_socket);
        return;
    }

    // Parse the first line: METHOD PATH HTTP/1.x
    std::istringstream request_stream(raw_request);
    std::string method, path, http_version;
    request_stream >> method >> path >> http_version;

    log("Request: " + method + " " + path);

    // CORS preflight
    if (method == "OPTIONS") {
        std::string headers =
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
            "Access-Control-Max-Age: 86400\r\n";
        send_http_response(client_socket, 204, "No Content", "", "text/plain");
        close(client_socket);
        return;
    }

    // Health check endpoint
    if (method == "GET" && (path == "/" || path == "/health")) {
        std::string status = running_.load() ? "ok" : "stopping";
        std::string body = "{\"status\":\"" + status + "\",\"proxy_port\":" +
                          std::to_string(port_) + ",\"local_server\":\"" +
                          local_server_url_ + "\"}";
        send_http_response(client_socket, 200, "OK", body);
        close(client_socket);
        return;
    }

    // Models endpoint (for compatibility)
    if (method == "GET" && path == "/v1/models") {
        std::string body = R"({"object":"list","data":[{"id":"embedding","object":"model","owned_by":"local-proxy"}]})";
        send_http_response(client_socket, 200, "OK", body);
        close(client_socket);
        return;
    }

    // Main embeddings endpoint
    if (method == "POST" && path == "/v1/embeddings") {
        // Extract body (after double newline)
        size_t body_start = raw_request.find("\r\n\r\n");
        if (body_start == std::string::npos) {
            body_start = raw_request.find("\n\n");
        }

        std::string body;
        if (body_start != std::string::npos) {
            size_t skip = (raw_request[body_start] == '\r') ? 4 : 2;
            body = raw_request.substr(body_start + skip);
        }

        // Check Content-Length for remaining data
        size_t content_length = 0;
        size_t cl_pos = raw_request.find("Content-Length:");
        if (cl_pos == std::string::npos) {
            cl_pos = raw_request.find("content-length:");
        }
        if (cl_pos != std::string::npos) {
            size_t val_start = raw_request.find(':', cl_pos) + 1;
            size_t val_end = raw_request.find('\r', val_start);
            if (val_end == std::string::npos) val_end = raw_request.find('\n', val_start);
            std::string val_str = raw_request.substr(val_start, val_end - val_start);
            // Trim whitespace
            val_str.erase(0, val_str.find_first_not_of(" \t"));
            val_str.erase(val_str.find_last_not_of(" \t") + 1);
            content_length = std::stoul(val_str);
        }

        // If body is incomplete, read more
        while (body.size() < content_length && running_.load()) {
            char buf[4096];
            struct pollfd pfd;
            pfd.fd = client_socket;
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 5000) <= 0) break;

            ssize_t n = recv(client_socket, buf, sizeof(buf), 0);
            if (n <= 0) break;
            body.append(buf, n);
        }

        std::string response = proxy_embeddings_request(body);

        // Add CORS headers
        std::string cors_headers =
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";

        // Send response with CORS
        std::string http_response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n" +
            cors_headers +
            "Content-Length: " + std::to_string(response.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + response;

        send(client_socket, http_response.c_str(), http_response.size(), 0);
        close(client_socket);
        return;
    }

    // 404 for everything else
    std::string not_found = "{\"error\":{\"message\":\"Not found\",\"type\":\"invalid_request_error\"}}";
    send_http_response(client_socket, 404, "Not Found", not_found);
    close(client_socket);
}

std::string EmbeddingProxy::read_http_request(int client_socket) {
    std::string request;
    char buf[4096];
    bool headers_complete = false;

    while (!headers_complete && running_.load()) {
        struct pollfd pfd;
        pfd.fd = client_socket;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int poll_result = poll(&pfd, 1, 10000); // 10s timeout for initial data
        if (poll_result <= 0) break;

        ssize_t n = recv(client_socket, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        request.append(buf, n);

        // Check if headers are complete (double newline)
        if (request.find("\r\n\r\n") != std::string::npos ||
            request.find("\n\n") != std::string::npos) {
            headers_complete = true;
        }
    }

    return request;
}

void EmbeddingProxy::send_http_response(int client_socket, int status_code,
                                         const std::string& status_text,
                                         const std::string& body,
                                         const std::string& content_type) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code << " " << status_text << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "\r\n"
             << body;

    std::string response_str = response.str();
    send(client_socket, response_str.c_str(), response_str.size(), 0);
}

EmbeddingProxy::EmbeddingRequest EmbeddingProxy::parse_request_body(const std::string& body) {
    EmbeddingRequest req;

    // Simple JSON parsing for the fields we need
    // "input" field - can be string or array
    size_t input_pos = body.find("\"input\"");
    if (input_pos != std::string::npos) {
        size_t colon = body.find(':', input_pos);
        size_t quote_start = body.find('"', colon + 1);
        if (quote_start != std::string::npos && body[quote_start] == '"') {
            // String input
            size_t quote_end = body.find('"', quote_start + 1);
            req.input = body.substr(quote_start + 1, quote_end - quote_start - 1);
        }
        // Note: array input not handled for simplicity (proxy single strings)
    }

    // "model" field
    size_t model_pos = body.find("\"model\"");
    if (model_pos != std::string::npos) {
        size_t colon = body.find(':', model_pos);
        size_t quote_start = body.find('"', colon + 1);
        if (quote_start != std::string::npos) {
            size_t quote_end = body.find('"', quote_start + 1);
            req.model = body.substr(quote_start + 1, quote_end - quote_start - 1);
        }
    }

    // "encoding_format" field (optional)
    size_t enc_pos = body.find("\"encoding_format\"");
    if (enc_pos != std::string::npos) {
        size_t colon = body.find(':', enc_pos);
        size_t quote_start = body.find('"', colon + 1);
        if (quote_start != std::string::npos) {
            size_t quote_end = body.find('"', quote_start + 1);
            req.encoding_format = body.substr(quote_start + 1, quote_end - quote_start - 1);
        }
    }

    return req;
}

std::string EmbeddingProxy::proxy_embeddings_request(const std::string& request_body) {
    EmbeddingRequest req = parse_request_body(request_body);

    if (req.input.empty()) {
        return "{\"error\":{\"message\":\"Missing 'input' field\",\"type\":\"invalid_request_error\"}}";
    }

    log("Embedding request: model=" + req.model + ", input_len=" + std::to_string(req.input.size()));

    // Forward to local server
    std::string local_response = forward_to_local_server("/v1/embeddings", request_body);

    if (local_response.empty()) {
        return "{\"error\":{\"message\":\"Local embedding server unavailable\",\"type\":\"server_error\"}}";
    }

    // The local server should already return OpenAI-compatible format
    // Just pass it through
    return local_response;
}

std::string EmbeddingProxy::forward_to_local_server(const std::string& path, const std::string& body) {
#ifdef USE_CURL
    std::string url = local_server_url_ + path;

    CURL* curl = curl_easy_init();
    if (!curl) {
        log("ERROR: CURL init failed");
        return "";
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* resp = static_cast<std::string*>(userdata);
            resp->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log("ERROR: CURL request failed: " + std::string(curl_easy_strerror(res)));
        return "";
    }

    if (http_code != 200) {
        log("ERROR: Local server returned HTTP " + std::to_string(http_code));
        return "";
    }

    return response;
#else
    (void)path;
    (void)body;
    log("ERROR: CURL not available for proxy forwarding");
    return "";
#endif
}

std::string EmbeddingProxy::build_openai_response(const std::string& local_response, int request_count) {
    // If local server already returns OpenAI format, pass through
    // This is a passthrough proxy - no transformation needed
    return local_response;
}

void EmbeddingProxy::log(const std::string& msg) {
    std::string timestamped = "[EmbeddingProxy] " + msg;
    if (log_callback_) {
        log_callback_(timestamped);
    } else {
        std::cout << timestamped << std::endl;
    }
}

} // namespace core
} // namespace llama_gui

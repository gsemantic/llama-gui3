#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>

namespace llama_gui {
namespace core {

/**
 * Менеджер выделенного сервера эмбеддингов.
 *
 * Запускает отдельный процесс llama-server с флагом --embeddings
 * (например, с моделью bge-m3-Q5_K_M.gguf, 1024-dim), чтобы RAG получал
 * качественные эмбеддинги независимо от основной чат-модели.
 */
class EmbeddingServer {
public:
    using StatusCallback = std::function<void(const std::string& status, bool is_running)>;

    EmbeddingServer();
    ~EmbeddingServer();

    // Конфигурация
    void set_model_path(const std::string& path) { model_path_ = path; }
    void set_host_port(const std::string& host, int port) {
        server_host_ = host;
        server_port_ = port;
    }
    void set_server_binary_path(const std::string& path) { server_binary_path_ = path; }
    std::string get_server_binary_path() const { return server_binary_path_; }
    void set_status_callback(StatusCallback callback) { status_callback_ = callback; }

    // Управление
    bool start_server();
    bool stop_server(bool blocking = false);
    bool is_server_running() const { return server_running_; }
    bool is_server_ready() const;  // HTTP health check
    std::string get_server_url() const;
    std::string get_server_output() const;
    int get_port() const { return server_port_; }

private:
    void server_thread_function();
    std::string build_server_command() const;

    // Убивает ТОЛЬКО процесс, слушающий server_port_ (не трогает основной сервер)
    bool kill_server_process(bool blocking = false);
    std::string check_http_status(const std::string& url) const;

    std::string model_path_;
    std::string server_host_ = "127.0.0.1";
    int server_port_ = 8083;
    std::string server_binary_path_ = "llama-server";

    std::atomic<bool> server_running_{false};
    std::atomic<bool> shutting_down_{false};
    std::unique_ptr<std::thread> server_thread_;
    std::string server_output_;
    std::string server_status_;
    StatusCallback status_callback_;
    mutable std::mutex output_mutex_;
};

} // namespace core
} // namespace llama_gui

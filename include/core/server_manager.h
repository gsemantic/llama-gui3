#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "core/settings.h"

namespace llama_gui {
namespace core {

class Settings;

/**
 * Server manager for controlling llama.cpp server process
 */
class ServerManager : public std::enable_shared_from_this<ServerManager> {
public:
    using StatusCallback = std::function<void(const std::string& status, bool is_running)>;

    ServerManager(Settings& settings);
    ~ServerManager();

    // Server control
    bool start_server();
    bool stop_server(bool blocking = false);
    bool restart_server();

    // Status
    bool is_server_running() const;
    bool is_server_ready() const;  // Check if server is responding
    std::string get_server_status() const;
    std::string get_server_output() const;

    // Configuration
    void set_model_path(const std::string& model_path);
    void set_server_url(const std::string& url);
    void set_host_port(const std::string& host, int port);
    void set_server_binary_path(const std::string& path) { server_binary_path_ = path; }
    std::string get_server_binary_path() const { return server_binary_path_; }

    // Callbacks
    void set_status_callback(StatusCallback callback);

    // =========================================================================
    // Thread-safe settings update (new for params expansion)
    // =========================================================================

    /// Queue settings update for next server restart
    void queue_settings_update(std::function<void(Settings&)> updater);

    /// Check if there are pending settings changes
    bool has_pending_settings() const;

    /// Apply pending settings (called during server restart)
    void apply_pending_settings();

    /// Get current server URL
    std::string get_server_url() const;

    /// Restart server with new settings
    bool restart_with_settings(std::function<void(Settings&)> new_settings);

private:
    Settings& settings_;
    std::unique_ptr<std::thread> server_thread_;
    std::atomic<bool> server_running_;
    std::atomic<bool> shutting_down_;
    std::string server_status_;
    std::string server_output_;
    StatusCallback status_callback_;

    std::string model_path_;
    std::string server_host_;
    int server_port_;
    std::string server_binary_path_ = "llama-server";

    // Pending settings for thread-safe updates
    mutable std::mutex settings_mutex_;
    std::optional<std::function<void(Settings&)>> pending_settings_;
    std::atomic<bool> settings_changed_{false};

    void server_thread_function();

    // =========================================================================
    // Categorized command builders (refactored from build_server_command)
    // =========================================================================

    std::string build_server_command() const;

    // Category-specific builders (each max 100 lines)
    std::string build_model_args() const;
    std::string build_gpu_args() const;
    std::string build_batch_args() const;
    std::string build_sampling_args() const;
    std::string build_cache_args() const;
    std::string build_rope_args() const;
    std::string build_server_args() const;
    std::string build_security_args() const;
    std::string build_advanced_args() const;
    std::string build_logging_args() const;
    std::string build_grammar_args() const;
    std::string build_output_args() const;

    // Helpers
    std::string format_tensor_split(const std::string& split) const;
    std::string format_cache_type(llama_gui::core::CacheSettings::CacheType type) const;
    std::string format_lora_adapters(const std::vector<llama_gui::core::ModelLoadingSettings::LoRAAdapter>& adapters) const;
    std::string format_control_vectors(const std::vector<llama_gui::core::ControlVectorSettings::ControlVector>& vectors) const;
    std::string format_dry_breakers(const std::vector<std::string>& breakers) const;

    bool kill_server_process(bool blocking = false);
    std::string check_http_status(const std::string& url) const;
};

} // namespace core
} // namespace llama_gui

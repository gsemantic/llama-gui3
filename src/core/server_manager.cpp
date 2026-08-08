#include "../../include/core/server_manager.h"
#include "../../include/core/settings.h"
#include "../../include/core/chat_template_manager.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <csignal>
#include <unistd.h>
#include <fstream>

namespace llama_gui {
namespace core {

// ============================================================================
// ServerManager Public API
// ============================================================================

ServerManager::ServerManager(Settings& settings)
    : settings_(settings)
    , server_running_(false)
    , shutting_down_(false)
    , server_port_(settings.server_runtime().port > 0 ? settings.server_runtime().port : 8081)
    , server_host_(settings.server_runtime().host.empty() ? "127.0.0.1" : settings.server_runtime().host) {
    // Get server binary path from settings, with auto-detect fallback
    server_binary_path_ = settings_.server_runtime().server_binary_path;
    
    // Auto-detect if default value
    if (server_binary_path_ == "llama-server") {
        const char* search_paths[] = {
            "./llama-server",
            "../llama-server",
            "/home/Alex/projects/llama-b7472-bin-ubuntu-x64/llama-b7472/llama-server",
            "/usr/local/bin/llama-server",
            "/usr/bin/llama-server",
            nullptr
        };
        
        for (const char* path : search_paths) {
            if (path && access(path, X_OK) == 0) {
                server_binary_path_ = path;
                break;
            }
        }
    }
}

ServerManager::~ServerManager() {
    stop_server(true);
}

bool ServerManager::start_server() {
    if (server_running_) {
        std::cerr << "Server is already running" << std::endl;
        return false;
    }

    shutting_down_ = false;
    server_running_ = true;

    server_thread_ = std::make_unique<std::thread>(&ServerManager::server_thread_function, this);

    // Non-blocking: return immediately, server will start in background
    // UI can poll is_server_ready() to check status
    return true;
}

bool ServerManager::stop_server(bool blocking) {
    if (!server_running_) {
        return true;
    }

    shutting_down_ = true;

    // Kill the llama-server process so the pipe breaks and fgets returns
    kill_server_process(false);

    if (server_thread_ && server_thread_->joinable()) {
        if (blocking) {
            server_thread_->join();
        } else {
            server_thread_->detach();
        }
    }

    server_running_ = false;
    return true;
}

bool ServerManager::restart_server() {
    // Kill old server process and wait for it to release the port
    if (server_running_) {
        shutting_down_ = true;
        kill_server_process(false);
        if (server_thread_ && server_thread_->joinable()) {
            server_thread_->detach();
        }
        server_running_ = false;
    }

    // Wait for the old process to fully release the port
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return start_server();
}

bool ServerManager::is_server_running() const {
    return server_running_;
}

bool ServerManager::is_server_ready() const {
    return check_http_status(get_server_url()) == "200";
}

std::string ServerManager::get_server_status() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return server_status_;
}

std::string ServerManager::get_server_output() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return server_output_;
}

void ServerManager::set_model_path(const std::string& model_path) {
    model_path_ = model_path;
}

void ServerManager::set_server_url(const std::string& url) {
    // Strip "http://" or "https://" prefix if present
    std::string clean_url = url;
    std::size_t scheme_end = clean_url.find("://");
    if (scheme_end != std::string::npos) {
        clean_url = clean_url.substr(scheme_end + 3);
    }
    // Strip trailing slash
    if (!clean_url.empty() && clean_url.back() == '/') {
        clean_url.pop_back();
    }
    std::size_t pos = clean_url.find_last_of(':');
    if (pos != std::string::npos) {
        server_host_ = clean_url.substr(0, pos);
        server_port_ = std::stoi(clean_url.substr(pos + 1));
    }
}

void ServerManager::set_host_port(const std::string& host, int port) {
    server_host_ = host;
    server_port_ = port;
}

void ServerManager::set_status_callback(StatusCallback callback) {
    status_callback_ = callback;
}

// ============================================================================
// Thread-safe Settings Update
// ============================================================================

void ServerManager::queue_settings_update(std::function<void(Settings&)> updater) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    pending_settings_ = updater;
    settings_changed_ = true;
}

bool ServerManager::has_pending_settings() const {
    return settings_changed_;
}

void ServerManager::apply_pending_settings() {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (pending_settings_) {
        (*pending_settings_)(settings_);
        pending_settings_.reset();
        settings_changed_ = false;
    }
}

bool ServerManager::restart_with_settings(std::function<void(Settings&)> new_settings) {
    queue_settings_update(new_settings);
    return restart_server();
}

// ============================================================================
// Server Thread
// ============================================================================

void ServerManager::server_thread_function() {
    std::string command = build_server_command();

    std::cerr << "Starting server with command:" << std::endl;
    std::cerr << command << std::endl;

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        std::cerr << "Failed to start server process" << std::endl;
        server_running_ = false;
        return;
    }

    char buffer[128];
    std::string output;

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr && !shutting_down_) {
        output += buffer;
        server_output_ = output;
        
        if (status_callback_) {
            status_callback_(buffer, server_running_);
        }
    }

    pclose(pipe);
    server_running_ = false;

    // Cleanup temp chat template file
    std::string temp_template = "/tmp/llama_gui_chat_template_" + std::to_string(getpid()) + ".jinja";
    unlink(temp_template.c_str());
}

// ============================================================================
// Command Builders
// ============================================================================

std::string ServerManager::build_server_command() const {
    std::ostringstream cmd;

    // Get model path from settings if model_path_ is empty
    std::string effective_model_path = model_path_.empty() ? settings_.get_model_path() : model_path_;

    cmd << server_binary_path_ << " --host " << server_host_
        << " --port " << server_port_
        << " --model " << effective_model_path;

    // Model args (alias, lora, mmproj)
    cmd << build_model_args();

    // Auto-detect chat template from GGUF and pass to server
    // This overrides llama-server's auto-detection which may get it wrong
    {
        auto& template_mgr = ChatTemplateManager::instance();
        auto result = template_mgr.extract_from_gguf(effective_model_path);
        if (result.success && !result.template_str.empty()) {
            // Save template to temp file and pass via --chat-template-file
            std::string temp_path = "/tmp/llama_gui_chat_template_" + std::to_string(getpid()) + ".jinja";
            std::ofstream temp_file(temp_path);
            if (temp_file.is_open()) {
                temp_file << result.template_str;
                temp_file.close();
                cmd << " --chat-template-file " << temp_path;
                std::cout << "[ServerManager] Auto-detected chat template from GGUF, saved to: " << temp_path << std::endl;
            }
        } else {
            std::cout << "[ServerManager] No chat template found in GGUF, using server auto-detection" << std::endl;
        }
    }

    // GPU settings
    cmd << build_gpu_args();

    // Batch settings
    cmd << build_batch_args();

    // Sampling settings
    cmd << build_sampling_args();

    // Cache settings
    cmd << build_cache_args();

    // RoPE settings
    cmd << build_rope_args();

    // Server settings
    cmd << build_server_args();

    // Security settings
    cmd << build_security_args();

    // Advanced settings
    cmd << build_advanced_args();

    // Logging settings
    cmd << build_logging_args();

    return cmd.str();
}

std::string ServerManager::build_model_args() const {
    std::ostringstream args;

    // Model alias
    if (!settings_.model_loading().model_alias.empty()) {
        args << " --alias " << settings_.model_loading().model_alias;
    }

    // Model loading settings
    if (!settings_.model_loading().lora_adapters.empty()) {
        args << " --lora-adapters " << settings_.model_loading().lora_adapters[0].path;
    }
    if (!settings_.model_loading().lora_base.empty()) {
        args << " --lora-base " << settings_.model_loading().lora_base;
    }
    if (!settings_.model_loading().mmproj.empty()) {
        args << " --mmproj " << settings_.model_loading().mmproj;
    }

    return args.str();
}

std::string ServerManager::build_gpu_args() const {
    std::ostringstream args;

    if (settings_.gpu().n_gpu_layers != 0) {
        args << " --n-gpu-layers " << settings_.gpu().n_gpu_layers;
    }
    if (!settings_.gpu().tensor_split.empty()) {
        args << " --tensor-split " << settings_.gpu().tensor_split;
    }

    return args.str();
}

std::string ServerManager::build_batch_args() const {
    std::ostringstream args;

    if (settings_.batch().batch_size != 512) {
        args << " --batch-size " << settings_.batch().batch_size;
    }
    // Always pass ctx-size to prevent llama-server from using the model's native
    // maximum context length, which can cause OOM on machines with limited RAM
    args << " --ctx-size " << settings_.batch().ctx_size;
    if (settings_.batch().ubatch_size != 512) {
        args << " --ubatch-size " << settings_.batch().ubatch_size;
    }

    return args.str();
}

std::string ServerManager::build_sampling_args() const {
    std::ostringstream args;

    if (settings_.sampling().temperature != 0.7f) {
        args << " --temp " << settings_.sampling().temperature;
    }
    if (settings_.sampling().top_p != 0.9f) {
        args << " --top-p " << settings_.sampling().top_p;
    }
    if (settings_.sampling().top_k != 40) {
        args << " --top-k " << settings_.sampling().top_k;
    }
    if (settings_.sampling().min_p != 0.05f) {
        args << " --min-p " << settings_.sampling().min_p;
    }
    if (settings_.sampling().repeat_penalty != 1.1f) {
        args << " --repeat-penalty " << settings_.sampling().repeat_penalty;
    }
    if (settings_.sampling().presence_penalty != 0.0f) {
        args << " --presence-penalty " << settings_.sampling().presence_penalty;
    }
    if (settings_.sampling().frequency_penalty != 0.0f) {
        args << " --frequency-penalty " << settings_.sampling().frequency_penalty;
    }

    return args.str();
}

std::string ServerManager::build_cache_args() const {
    std::ostringstream args;

    if (settings_.cache().cache_type_k != CacheSettings::CacheType::F16) {
        args << " --cache-type-k " << format_cache_type(settings_.cache().cache_type_k);
    }
    if (settings_.cache().cache_type_v != CacheSettings::CacheType::F16) {
        args << " --cache-type-v " << format_cache_type(settings_.cache().cache_type_v);
    }

    return args.str();
}

std::string ServerManager::build_rope_args() const {
    std::ostringstream args;

    if (settings_.rope().rope_scale != 1.0f) {
        args << " --rope-scale " << settings_.rope().rope_scale;
    }

    return args.str();
}

std::string ServerManager::build_server_args() const {
    std::ostringstream args;

    // Host and port already in build_server_command
    // Add any additional server settings
    if (settings_.server_runtime().threads_http != 4) {
        args << " --threads " << settings_.server_runtime().threads_http;
    }

    return args.str();
}
std::string llama_gui::core::ServerManager::build_security_args() const {
    std::ostringstream args;

    // Add API keys from server runtime settings
    for (const auto& key : settings_.server_runtime().api_keys) {
        if (!key.empty()) {
            args << " --api-key " << key;
        }
    }

    // Add SSL configuration if available
    if (!settings_.server_runtime().ssl_key_file.empty() && 
        !settings_.server_runtime().ssl_cert_file.empty()) {
        args << " --ssl-key-file " << settings_.server_runtime().ssl_key_file;
        args << " --ssl-cert-file " << settings_.server_runtime().ssl_cert_file;
    }

    return args.str();
}
std::string ServerManager::build_advanced_args() const {
    std::ostringstream args;

    if (settings_.gpu().mlock) {
        args << " --mlock";
    }
    if (settings_.gpu().no_mmap) {
        args << " --no-mmap";
    }

    return args.str();
}

std::string ServerManager::build_logging_args() const {
    std::ostringstream args;

    if (settings_.server_runtime().log_verbosity > 0) {
        args << " --log-verbosity " << settings_.server_runtime().log_verbosity;
    }

    return args.str();
}

std::string ServerManager::build_grammar_args() const {
    std::ostringstream args;

    if (!settings_.grammar().grammar.empty()) {
        args << " --grammar " << settings_.grammar().grammar;
    }

    return args.str();
}

std::string ServerManager::format_cache_type(llama_gui::core::CacheSettings::CacheType type) const {
    return CacheSettings::cache_type_to_string(type);
}

std::string ServerManager::format_lora_adapters(
    const std::vector<ModelLoadingSettings::LoRAAdapter>& adapters) const {
    std::ostringstream result;
    for (size_t i = 0; i < adapters.size(); i++) {
        if (i > 0) result << ",";
        result << adapters[i].path;
    }
    return result.str();
}

std::string ServerManager::format_control_vectors(
    const std::vector<ControlVectorSettings::ControlVector>& vectors) const {
    std::ostringstream result;
    for (size_t i = 0; i < vectors.size(); i++) {
        if (i > 0) result << ",";
        result << vectors[i].path << ":" << vectors[i].scale;
    }
    return result.str();
}

std::string ServerManager::format_dry_breakers(const std::vector<std::string>& breakers) const {
    std::ostringstream result;
    for (size_t i = 0; i < breakers.size(); i++) {
        if (i > 0) result << ",";
        result << breakers[i];
    }
    return result.str();
}

bool ServerManager::kill_server_process(bool blocking) {
    // Try to kill the llama-server process
    std::string pid_cmd = "pkill -f llama-server";
    int result = system(pid_cmd.c_str());
    
    if (result == 0) {
        if (blocking) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        return true;
    }
    
    return false;
}

std::string ServerManager::check_http_status(const std::string& url) const {
    // Simple HTTP check using curl
    std::string curl_cmd = "curl -s -o /dev/null -w \"%{http_code}\" --max-time 5 " + url;
    FILE* pipe = popen(curl_cmd.c_str(), "r");
    
    if (!pipe) {
        return "000";
    }
    
    char buffer[128];
    std::string http_code;
    
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        http_code = buffer;
    }
    
    pclose(pipe);
    return http_code;
}

std::string ServerManager::get_server_url() const {
    return "http://" + server_host_ + ":" + std::to_string(server_port_);
}

} // namespace core
} // namespace llama_gui

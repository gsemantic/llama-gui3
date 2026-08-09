#include "../../include/core/embedding_server.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <cstdio>

namespace llama_gui {
namespace core {

EmbeddingServer::EmbeddingServer() {
    // Auto-detect server binary if still default
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

EmbeddingServer::~EmbeddingServer() {
    stop_server(true);
}

bool EmbeddingServer::start_server() {
    if (server_running_) {
        std::cerr << "[EmbeddingServer] Already running" << std::endl;
        return false;
    }

    if (model_path_.empty()) {
        std::cerr << "[EmbeddingServer] No embedding model path configured" << std::endl;
        return false;
    }

    shutting_down_ = false;
    server_running_ = true;

    server_thread_ = std::make_unique<std::thread>(&EmbeddingServer::server_thread_function, this);
    return true;
}

bool EmbeddingServer::stop_server(bool blocking) {
    if (!server_running_) {
        return true;
    }

    shutting_down_ = true;
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

bool EmbeddingServer::is_server_ready() const {
    return check_http_status(get_server_url()) == "200";
}

std::string EmbeddingServer::get_server_url() const {
    return "http://" + server_host_ + ":" + std::to_string(server_port_);
}

std::string EmbeddingServer::get_server_output() const {
    std::lock_guard<std::mutex> lock(output_mutex_);
    return server_output_;
}

void EmbeddingServer::server_thread_function() {
    std::string command = build_server_command();

    std::cerr << "[EmbeddingServer] Starting with command:" << std::endl;
    std::cerr << command << std::endl;

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        std::cerr << "[EmbeddingServer] Failed to start process" << std::endl;
        server_running_ = false;
        return;
    }

    char buffer[128];
    std::string output;

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr && !shutting_down_) {
        output += buffer;
        {
            std::lock_guard<std::mutex> lock(output_mutex_);
            server_output_ = output;
        }
        if (status_callback_) {
            status_callback_(buffer, server_running_);
        }
    }

    pclose(pipe);
    server_running_ = false;
}

std::string EmbeddingServer::build_server_command() const {
    std::ostringstream cmd;

    cmd << server_binary_path_
        << " --host " << server_host_
        << " --port " << server_port_
        << " --model " << model_path_
        << " --embeddings"
        << " --pooling mean"
        << " --ctx-size 2048"
        << " --batch-size 512"
        << " --ubatch-size 512"
        << " --parallel 1"
        << " --no-webui";

    return cmd.str();
}

bool EmbeddingServer::kill_server_process(bool blocking) {
    // Kill ONLY the process listening on our embedding port.
    // This avoids touching the main chat server (unlike pkill -f llama-server).
    std::string kill_cmd = "fuser -k " + std::to_string(server_port_) + "/tcp 2>/dev/null";
    int result = system(kill_cmd.c_str());

    if (result == 0) {
        if (blocking) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return true;
    }

    return false;
}

std::string EmbeddingServer::check_http_status(const std::string& url) const {
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

} // namespace core
} // namespace llama_gui

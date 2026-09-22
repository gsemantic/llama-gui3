#include "../../include/core/embedding_server.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <sys/wait.h>
#include <cstring>
#include <vector>
#include <algorithm>

namespace llama_gui {
namespace core {

// ============================================================================
// atexit: гарантированное убийство embedding-server при любом выходе
// ============================================================================

/// Глобальный список PID дочерних процессов embedding-server.
/// Заполняется при fork(), читается в atexit-обработчике.
static std::vector<pid_t>& get_embedding_child_pids() {
    static std::vector<pid_t> pids;
    return pids;
}

static std::mutex& get_embedding_pids_mutex() {
    static std::mutex m;
    return m;
}

/// atexit-обработчик: убивает все embedding-server процессы-сироты.
/// Вызывается даже при exit(), _exit() и возврате из main().
static void atexit_kill_embedding_servers() {
    std::lock_guard<std::mutex> lock(get_embedding_pids_mutex());
    for (pid_t pid : get_embedding_child_pids()) {
        if (pid > 0 && kill(pid, 0) == 0) {
            std::cerr << "[EmbeddingServer] atexit: убиваю процесс " << pid << std::endl;
            kill(pid, SIGTERM);
            // Короткая задержка — даём шанс на graceful shutdown
            for (int i = 0; i < 10; ++i) {
                if (kill(pid, 0) != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (kill(pid, 0) == 0) {
                kill(pid, SIGKILL);
            }
        }
    }
    get_embedding_child_pids().clear();
}

/// Регистрируем atexit один раз
static bool register_atexit_handler() {
    static bool registered = false;
    if (!registered) {
        std::atexit(atexit_kill_embedding_servers);
        registered = true;
    }
    return registered;
}

// ============================================================================
// EmbeddingServer
// ============================================================================

EmbeddingServer::EmbeddingServer() {
    register_atexit_handler();

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
    // -----------------------------------------------------------------------
    // Разбиваем команду на argv[] для execvp (без shell)
    // -----------------------------------------------------------------------
    std::string command = build_server_command();
    std::vector<std::string> args_str;
    std::istringstream iss(command);
    std::string token;
    while (iss >> token) {
        args_str.push_back(token);
    }

    std::vector<char*> argv;
    for (auto& a : args_str) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    // -----------------------------------------------------------------------
    // pipe: дочерний процесс пишет stdout → parent читает
    // -----------------------------------------------------------------------
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        std::cerr << "[EmbeddingServer] pipe() failed: " << strerror(errno) << std::endl;
        server_running_ = false;
        return;
    }

    std::cerr << "[EmbeddingServer] Starting with command:" << std::endl;
    std::cerr << command << std::endl;

    pid_t pid = fork();
    if (pid == -1) {
        std::cerr << "[EmbeddingServer] fork() failed: " << strerror(errno) << std::endl;
        close(pipefd[0]);
        close(pipefd[1]);
        server_running_ = false;
        return;
    }

    if (pid == 0) {
        // === CHILD ===
        close(pipefd[0]);  // закрываем read end
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execvp(argv[0], argv.data());

        // execvp returns only on error
        _exit(127);
    }

    // === PARENT ===
    close(pipefd[1]);  // закрываем write end
    child_stdout_fd_ = pipefd[0];
    child_pid_.store(pid);

    // Регистрируем PID в глобальном списке для atexit
    {
        std::lock_guard<std::mutex> lock(get_embedding_pids_mutex());
        get_embedding_child_pids().push_back(pid);
    }

    // -----------------------------------------------------------------------
    // Читаем вывод дочернего процесса
    // -----------------------------------------------------------------------
    char buffer[256];
    std::string output;

    while (!shutting_down_) {
        ssize_t n = read(child_stdout_fd_, buffer, sizeof(buffer) - 1);
        if (n <= 0) break;
        buffer[n] = '\0';
        output += buffer;
        {
            std::lock_guard<std::mutex> lock(output_mutex_);
            server_output_ = output;
        }
        if (status_callback_) {
            status_callback_(buffer, server_running_);
        }
    }

    close(child_stdout_fd_);
    child_stdout_fd_ = -1;

    // Wait for child to avoid zombie
    int status = 0;
    waitpid(pid, &status, 0);

    child_pid_.store(0);

    // Удаляем PID из глобального списка
    {
        std::lock_guard<std::mutex> lock(get_embedding_pids_mutex());
        auto& pids = get_embedding_child_pids();
        pids.erase(std::remove(pids.begin(), pids.end(), pid), pids.end());
    }

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
    pid_t pid = child_pid_.load();

    // 1) Убиваем по PID — SIGTERM (graceful), затем SIGKILL
    if (pid > 0) {
        if (kill(pid, 0) == 0) {
            std::cerr << "[EmbeddingServer] Отправляю SIGTERM процессу " << pid << std::endl;
            kill(pid, SIGTERM);

            // Ждём до 3 секунд для graceful shutdown
            for (int i = 0; i < 30; ++i) {
                if (kill(pid, 0) != 0) {
                    std::cerr << "[EmbeddingServer] Процесс " << pid << " завершился" << std::endl;
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // SIGKILL как last resort
            std::cerr << "[EmbeddingServer] SIGTERM не сработал, отправляю SIGKILL процессу " << pid << std::endl;
            kill(pid, SIGKILL);

            if (blocking) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            return true;
        }
    }

    // 2) Fallback: fuser -k по порту (если PID неизвестен или процесс уже не наш)
    std::string kill_cmd = "fuser -k " + std::to_string(server_port_) + "/tcp 2>/dev/null";
    int result = system(kill_cmd.c_str());
    if (result == 0) {
        std::cerr << "[EmbeddingServer] fuser убил процесс на порту " << server_port_ << std::endl;
        if (blocking) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return true;
    }

    std::cerr << "[EmbeddingServer] Не удалось убить процесс на порту " << server_port_
              << " (PID=" << pid << ", fuser вернул " << result << ")" << std::endl;
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

#include "wp_file_agent.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace wp_coder {

namespace fs = std::filesystem;

// ===========================================================================
// WPFileAgent implementation
// ===========================================================================

WPFileAgent::WPFileAgent() = default;

WPFileAgent::~WPFileAgent() {
    shutdown();
}

const char* WPFileAgent::name() const {
    return "wp_file_agent";
}

const char* WPFileAgent::description() const {
    return "WordPress File agent. Safe file operations with wp-content policies.";
}

const char* WPFileAgent::version() const {
    return "0.1.0";
}

bool WPFileAgent::initialize(agents::AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Инициализация разрешенных расширений
    allowed_extensions_ = {
        ".php", ".js", ".css", ".json", ".md", ".txt", ".html", ".xml", ".yml", ".yaml"
    };
    
    // Базовая директория wp-content
    wp_content_base_ = "wp-content";
    
    if (context_) {
        context_->info(name(), "Initialized with wp-content base: " + wp_content_base_);
    }
    
    return true;
}

agents::AgentResult WPFileAgent::execute(const agents::AgentRequest& request) {
    if (!initialized_) {
        return agents::AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "read") {
        return handle_read(request);
    } else if (action == "write") {
        return handle_write(request);
    } else if (action == "append") {
        return handle_append(request);
    } else if (action == "delete") {
        return handle_delete(request);
    } else if (action == "exists") {
        return handle_exists(request);
    } else if (action == "list") {
        return handle_list(request);
    } else if (action == "copy") {
        return handle_copy(request);
    } else if (action == "move") {
        return handle_move(request);
    } else if (action == "info") {
        return handle_info(request);
    }
    
    return agents::AgentResult::error("Unknown action: " + action);
}

void WPFileAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    context_ = nullptr;
}

agents::AgentCapability WPFileAgent::capabilities() const {
    return agents::AgentCapability::FILE_READ | 
           agents::AgentCapability::FILE_WRITE;
}

bool WPFileAgent::is_ready() const {
    return initialized_;
}

// ===========================================================================
// Обработчики действий
// ===========================================================================

agents::AgentResult WPFileAgent::handle_read(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Reading file");
    }
    
    std::string path = request.get_param<std::string>("path", "");
    if (path.empty()) {
        return agents::AgentResult::error("Path is empty");
    }
    
    path = normalize_path(path);
    if (!is_path_in_wp_content(path)) {
        return agents::AgentResult::error("Path outside wp-content is not allowed");
    }
    if (!is_extension_allowed(path)) {
        return agents::AgentResult::error("File extension not allowed");
    }
    
    std::ifstream file(path);
    if (!file.is_open()) {
        return agents::AgentResult::error("Cannot open file: " + path);
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    
    return agents::AgentResult::success({
        {"content", buffer.str()},
        {"path", path},
        {"size", static_cast<int>(buffer.str().length())}
    });
}

agents::AgentResult WPFileAgent::handle_write(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Writing file");
    }
    
    std::string path = request.get_param<std::string>("path", "");
    std::string content = request.get_param<std::string>("content", "");
    
    if (path.empty()) {
        return agents::AgentResult::error("Path is empty");
    }
    
    path = normalize_path(path);
    if (!is_path_in_wp_content(path)) {
        return agents::AgentResult::error("Path outside wp-content is not allowed");
    }
    if (!is_extension_allowed(path)) {
        return agents::AgentResult::error("File extension not allowed");
    }
    if (!is_file_size_allowed(path)) {
        return agents::AgentResult::error("File size exceeds limit");
    }
    
    // Создаем директории если нужно
    fs::path dir = fs::path(path).parent_path();
    if (!dir.empty()) {
        fs::create_directories(dir);
    }
    
    std::ofstream file(path);
    if (!file.is_open()) {
        return agents::AgentResult::error("Cannot write file: " + path);
    }
    
    file << content;
    
    return agents::AgentResult::success({
        {"path", path},
        {"size", static_cast<int>(content.length())},
        {"written", true}
    });
}

agents::AgentResult WPFileAgent::handle_append(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Appending to file");
    }
    
    std::string path = request.get_param<std::string>("path", "");
    std::string content = request.get_param<std::string>("content", "");
    
    if (path.empty()) {
        return agents::AgentResult::error("Path is empty");
    }
    
    path = normalize_path(path);
    if (!is_path_in_wp_content(path)) {
        return agents::AgentResult::error("Path outside wp-content is not allowed");
    }
    if (!is_extension_allowed(path)) {
        return agents::AgentResult::error("File extension not allowed");
    }
    
    std::ofstream file(path, std::ios::app);
    if (!file.is_open()) {
        return agents::AgentResult::error("Cannot append to file: " + path);
    }
    
    file << content;
    
    return agents::AgentResult::success({
        {"path", path},
        {"appended", true}
    });
}

agents::AgentResult WPFileAgent::handle_delete(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Deleting file");
    }
    
    std::string path = request.get_param<std::string>("path", "");
    if (path.empty()) {
        return agents::AgentResult::error("Path is empty");
    }
    
    path = normalize_path(path);
    if (!is_path_in_wp_content(path)) {
        return agents::AgentResult::error("Path outside wp-content is not allowed");
    }
    
    if (!fs::exists(path)) {
        return agents::AgentResult::error("File does not exist: " + path);
    }
    
    try {
        fs::remove(path);
        return agents::AgentResult::success({
            {"path", path},
            {"deleted", true}
        });
    } catch (const std::exception& e) {
        return agents::AgentResult::error("Cannot delete file: " + std::string(e.what()));
    }
}

agents::AgentResult WPFileAgent::handle_exists(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Checking file existence");
    }
    
    std::string path = request.get_param<std::string>("path", "");
    if (path.empty()) {
        return agents::AgentResult::error("Path is empty");
    }
    
    path = normalize_path(path);
    
    bool exists = fs::exists(path);
    
    return agents::AgentResult::success({
        {"path", path},
        {"exists", exists}
    });
}

agents::AgentResult WPFileAgent::handle_list(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Listing directory");
    }
    
    std::string path = request.get_param<std::string>("path", ".");
    path = normalize_path(path);
    if (!is_path_in_wp_content(path)) {
        return agents::AgentResult::error("Path outside wp-content is not allowed");
    }
    
    if (!fs::exists(path) || !fs::is_directory(path)) {
        return agents::AgentResult::error("Directory does not exist: " + path);
    }
    
    nlohmann::json files = nlohmann::json::array();
    
    for (const auto& entry : fs::directory_iterator(path)) {
        nlohmann::json file_info;
        file_info["path"] = entry.path().string();
        file_info["filename"] = entry.path().filename().string();
        file_info["is_directory"] = entry.is_directory();
        if (entry.is_regular_file()) {
            file_info["size"] = static_cast<int>(entry.file_size());
        }
        files.push_back(file_info);
    }
    
    return agents::AgentResult::success({
        {"path", path},
        {"files", files},
        {"count", static_cast<int>(files.size())}
    });
}

agents::AgentResult WPFileAgent::handle_copy(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Copying file");
    }
    
    std::string from = request.get_param<std::string>("from", "");
    std::string to = request.get_param<std::string>("to", "");
    
    if (from.empty() || to.empty()) {
        return agents::AgentResult::error("Source or destination path is empty");
    }
    
    from = normalize_path(from);
    to = normalize_path(to);
    
    if (!is_path_in_wp_content(from) || !is_path_in_wp_content(to)) {
        return agents::AgentResult::error("Paths outside wp-content are not allowed");
    }
    
    try {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing);
        return agents::AgentResult::success({
            {"from", from},
            {"to", to},
            {"copied", true}
        });
    } catch (const std::exception& e) {
        return agents::AgentResult::error("Cannot copy file: " + std::string(e.what()));
    }
}

agents::AgentResult WPFileAgent::handle_move(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Moving file");
    }
    
    std::string from = request.get_param<std::string>("from", "");
    std::string to = request.get_param<std::string>("to", "");
    
    if (from.empty() || to.empty()) {
        return agents::AgentResult::error("Source or destination path is empty");
    }
    
    from = normalize_path(from);
    to = normalize_path(to);
    
    if (!is_path_in_wp_content(from) || !is_path_in_wp_content(to)) {
        return agents::AgentResult::error("Paths outside wp-content are not allowed");
    }
    
    try {
        fs::rename(from, to);
        return agents::AgentResult::success({
            {"from", from},
            {"to", to},
            {"moved", true}
        });
    } catch (const std::exception& e) {
        return agents::AgentResult::error("Cannot move file: " + std::string(e.what()));
    }
}

agents::AgentResult WPFileAgent::handle_info(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Getting file info");
    }
    
    std::string path = request.get_param<std::string>("path", "");
    if (path.empty()) {
        return agents::AgentResult::error("Path is empty");
    }
    
    path = normalize_path(path);
    
    if (!fs::exists(path)) {
        return agents::AgentResult::error("File does not exist: " + path);
    }
    
    nlohmann::json info;
    info["path"] = path;
    info["is_directory"] = fs::is_directory(path);
    info["is_regular_file"] = fs::is_regular_file(path);
    
    if (fs::is_regular_file(path)) {
        info["size"] = static_cast<int>(fs::file_size(path));
        info["extension"] = fs::path(path).extension().string();
    }
    
    auto ftime = fs::last_write_time(path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    info["modified"] = std::chrono::system_clock::to_time_t(sctp);
    
    return agents::AgentResult::success({
        {"info", info}
    });
}

// ===========================================================================
// Вспомогательные функции
// ===========================================================================

bool WPFileAgent::is_extension_allowed(const std::string& path) const {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return allowed_extensions_.find(ext) != allowed_extensions_.end();
}

bool WPFileAgent::is_file_size_allowed(const std::string& path) const {
    if (!fs::exists(path)) {
        return true; // Файл не существует, размер не проверяем
    }
    
    size_t size_mb = fs::file_size(path) / (1024 * 1024);
    return size_mb <= max_file_size_mb_;
}

std::string WPFileAgent::normalize_path(const std::string& path) const {
    fs::path p(path);
    fs::path normalized = fs::weakly_canonical(p);
    return normalized.string();
}

bool WPFileAgent::is_path_in_wp_content(const std::string& path) const {
    fs::path p(path);
    fs::path wp_content(wp_content_base_);
    
    // Проверяем, что путь начинается с wp-content
    auto rel = fs::relative(p, wp_content);
    return rel.string().find("..") == std::string::npos;
}

} // namespace wp_coder
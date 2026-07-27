/**
 * @file file_agent.cpp
 * @brief Реализация агента для работы с файлами
 */

#include "file_agent.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace agents {

// ============================================================================
// FileAgent implementation
// ============================================================================

FileAgent::FileAgent() {
    // Расширения по умолчанию
    allowed_extensions_ = {
        ".txt", ".md", ".json", ".csv",
        ".cpp", ".h", ".hpp", ".cxx",
        ".py", ".js", ".ts", ".java",
        ".xml", ".yaml", ".yml", ".toml",
        ".log", ".cfg", ".conf", ".ini"
    };
}

FileAgent::~FileAgent() {
    shutdown();
}

const char* FileAgent::name() const {
    return "file_agent";
}

const char* FileAgent::description() const {
    return "File system agent for reading, writing, and managing files. "
           "Supports text files, code files, and structured data formats.";
}

const char* FileAgent::version() const {
    return "1.0.0";
}

bool FileAgent::initialize(AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Загрузка настроек
    auto config = context_->get_agent_config(name());
    if (config.contains("allowed_extensions")) {
        allowed_extensions_.clear();
        for (const auto& ext : config["allowed_extensions"]) {
            allowed_extensions_.insert(ext.get<std::string>());
        }
    }
    if (config.contains("max_file_size_mb")) {
        max_file_size_mb_ = config["max_file_size_mb"].get<size_t>();
    }
    if (config.contains("base_dir")) {
        base_dir_ = config["base_dir"].get<std::string>();
    }
    
    context_->info(name(), "Initialized with " + 
                   std::to_string(allowed_extensions_.size()) + " extensions");
    
    return true;
}

AgentResult FileAgent::execute(const AgentRequest& request) {
    if (!initialized_) {
        return AgentResult::error("Agent not initialized");
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
    
    return AgentResult::error("Unknown action: " + action);
}

void FileAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability FileAgent::capabilities() const {
    return AgentCapability::FILE_READ | 
           AgentCapability::FILE_WRITE |
           AgentCapability::FILE_DELETE |
           AgentCapability::DIRECTORY_LIST;
}

bool FileAgent::is_ready() const {
    return initialized_;
}

// ============================================================================
// Обработчики действий
// ============================================================================

AgentResult FileAgent::handle_read(const AgentRequest& request) {
    std::string file_path = request.file_path();
    
    if (file_path.empty()) {
        return AgentResult::error("File path is empty");
    }
    
    file_path = normalize_path(file_path);
    
    // Проверка расширения
    if (!is_extension_allowed(file_path)) {
        return AgentResult::error("File extension not allowed");
    }
    
    // Проверка существования
    if (!fs::exists(file_path)) {
        return AgentResult::not_found("File not found: " + file_path);
    }
    
    // Проверка размера
    if (!is_file_size_allowed(file_path)) {
        return AgentResult::error("File too large");
    }
    
    try {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return AgentResult::error("Cannot open file: " + file_path);
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        
        if (context_) {
            context_->debug(name(), "Read " + 
                           std::to_string(content.size()) + " bytes from " + 
                           file_path);
        }
        
        return AgentResult::success({
            {"content", content},
            {"file_path", file_path},
            {"size", static_cast<int>(content.size())}
        });
        
    } catch (const std::exception& e) {
        return AgentResult::error(std::string("Read error: ") + e.what());
    }
}

AgentResult FileAgent::handle_write(const AgentRequest& request) {
    std::string file_path = request.file_path();
    std::string content = request.get_param<std::string>("content", "");
    
    if (file_path.empty()) {
        return AgentResult::error("File path is empty");
    }
    
    file_path = normalize_path(file_path);
    
    // Проверка расширения
    if (!is_extension_allowed(file_path)) {
        return AgentResult::error("File extension not allowed");
    }
    
    try {
        // Создаём директорию если нужно
        fs::path p(file_path);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }
        
        std::ofstream file(file_path);
        if (!file.is_open()) {
            return AgentResult::error("Cannot create file: " + file_path);
        }
        
        file << content;
        
        if (context_) {
            context_->debug(name(), "Wrote " + 
                           std::to_string(content.size()) + " bytes to " + 
                           file_path);
        }
        
        return AgentResult::success({
            {"file_path", file_path},
            {"bytes_written", static_cast<int>(content.size())}
        });
        
    } catch (const std::exception& e) {
        return AgentResult::error(std::string("Write error: ") + e.what());
    }
}

AgentResult FileAgent::handle_append(const AgentRequest& request) {
    std::string file_path = request.file_path();
    std::string content = request.get_param<std::string>("content", "");
    
    if (file_path.empty()) {
        return AgentResult::error("File path is empty");
    }
    
    file_path = normalize_path(file_path);
    
    try {
        std::ofstream file(file_path, std::ios::app);
        if (!file.is_open()) {
            return AgentResult::error("Cannot open file for append: " + file_path);
        }
        
        file << content;
        
        return AgentResult::success({
            {"file_path", file_path},
            {"bytes_appended", static_cast<int>(content.size())}
        });
        
    } catch (const std::exception& e) {
        return AgentResult::error(std::string("Append error: ") + e.what());
    }
}

AgentResult FileAgent::handle_delete(const AgentRequest& request) {
    std::string file_path = request.file_path();
    
    if (file_path.empty()) {
        return AgentResult::error("File path is empty");
    }
    
    file_path = normalize_path(file_path);
    
    if (!fs::exists(file_path)) {
        return AgentResult::not_found("File not found: " + file_path);
    }
    
    try {
        fs::remove(file_path);
        
        if (context_) {
            context_->info(name(), "Deleted file: " + file_path);
        }
        
        return AgentResult::success({
            {"file_path", file_path},
            {"deleted", true}
        });
        
    } catch (const std::exception& e) {
        return AgentResult::error(std::string("Delete error: ") + e.what());
    }
}

AgentResult FileAgent::handle_exists(const AgentRequest& request) {
    std::string file_path = request.file_path();
    
    if (file_path.empty()) {
        return AgentResult::error("File path is empty");
    }
    
    file_path = normalize_path(file_path);
    
    bool exists = fs::exists(file_path);
    
    return AgentResult::success({
        {"file_path", file_path},
        {"exists", exists}
    });
}

AgentResult FileAgent::handle_list(const AgentRequest& request) {
    std::string dir_path = request.get_param<std::string>("dir", "");
    std::string pattern = request.get_param<std::string>("pattern", "*");
    
    if (dir_path.empty()) {
        dir_path = normalize_path(".");
    } else {
        dir_path = normalize_path(dir_path);
    }
    
    if (!fs::is_directory(dir_path)) {
        return AgentResult::error("Not a directory: " + dir_path);
    }
    
    nlohmann::json files = nlohmann::json::array();
    
    try {
        for (const auto& entry : fs::directory_iterator(dir_path)) {
            nlohmann::json file_info;
            file_info["name"] = entry.path().filename().string();
            file_info["path"] = entry.path().string();
            file_info["is_directory"] = entry.is_directory();
            
            if (entry.is_regular_file()) {
                file_info["size"] = static_cast<int64_t>(entry.file_size());
            }
            
            files.push_back(file_info);
        }
    } catch (const std::exception& e) {
        return AgentResult::error(std::string("List error: ") + e.what());
    }
    
    return AgentResult::success({
        {"directory", dir_path},
        {"files", files},
        {"count", static_cast<int>(files.size())}
    });
}

AgentResult FileAgent::handle_copy(const AgentRequest& request) {
    std::string src = request.get_param<std::string>("src", "");
    std::string dst = request.get_param<std::string>("dst", "");
    
    if (src.empty() || dst.empty()) {
        return AgentResult::error("Source or destination path is empty");
    }
    
    src = normalize_path(src);
    dst = normalize_path(dst);
    
    if (!fs::exists(src)) {
        return AgentResult::not_found("Source not found: " + src);
    }
    
    try {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        
        return AgentResult::success({
            {"source", src},
            {"destination", dst},
            {"copied", true}
        });
        
    } catch (const std::exception& e) {
        return AgentResult::error(std::string("Copy error: ") + e.what());
    }
}

AgentResult FileAgent::handle_move(const AgentRequest& request) {
    std::string src = request.get_param<std::string>("src", "");
    std::string dst = request.get_param<std::string>("dst", "");
    
    if (src.empty() || dst.empty()) {
        return AgentResult::error("Source or destination path is empty");
    }
    
    src = normalize_path(src);
    dst = normalize_path(dst);
    
    if (!fs::exists(src)) {
        return AgentResult::not_found("Source not found: " + src);
    }
    
    try {
        fs::rename(src, dst);
        
        return AgentResult::success({
            {"source", src},
            {"destination", dst},
            {"moved", true}
        });
        
    } catch (const std::exception& e) {
        return AgentResult::error(std::string("Move error: ") + e.what());
    }
}

AgentResult FileAgent::handle_info(const AgentRequest& request) {
    std::string file_path = request.file_path();
    
    if (file_path.empty()) {
        return AgentResult::error("File path is empty");
    }
    
    file_path = normalize_path(file_path);
    
    if (!fs::exists(file_path)) {
        return AgentResult::not_found("File not found: " + file_path);
    }
    
    nlohmann::json info;
    info["path"] = file_path;
    info["exists"] = true;
    info["filename"] = fs::path(file_path).filename().string();
    info["extension"] = fs::path(file_path).extension().string();
    
    if (fs::is_regular_file(file_path)) {
        info["is_file"] = true;
        info["is_directory"] = false;
        info["size"] = static_cast<int64_t>(fs::file_size(file_path));
    } else if (fs::is_directory(file_path)) {
        info["is_file"] = false;
        info["is_directory"] = true;
    }
    
    info["absolute_path"] = fs::absolute(file_path).string();
    
    return AgentResult::success(info);
}

// ============================================================================
// Вспомогательные функции
// ============================================================================

bool FileAgent::is_extension_allowed(const std::string& path) const {
    std::string ext = fs::path(path).extension().string();
    
    // Приводим к нижнему регистру
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    
    // Если расширения нет в списке запрещённых - разрешаем
    if (allowed_extensions_.empty()) {
        return true;
    }
    
    return allowed_extensions_.count(ext) > 0;
}

bool FileAgent::is_file_size_allowed(const std::string& path) const {
    try {
        auto size = fs::file_size(path);
        size_t max_size = max_file_size_mb_ * 1024 * 1024;
        return size <= max_size;
    } catch (...) {
        return false;
    }
}

std::string FileAgent::normalize_path(const std::string& path) const {
    if (path.empty()) {
        return "";
    }
    
    // Если задана базовая директория и путь относительный
    if (!base_dir_.empty() && path[0] != '/' && path[0] != '~') {
        return base_dir_ + "/" + path;
    }
    
    // Обработка ~
    if (path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            return std::string(home) + path.substr(1);
        }
        return path;
    }
    
    return path;
}

} // namespace agents

// ============================================================================
// C-API экспорт
// ============================================================================

extern "C" {

AGENT_PLUGIN_EXPORT const char* plugin_get_name() {
    return "file_agent";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_version() {
    return "1.0.0";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_api_version() {
    return AGENT_PLUGIN_API_VERSION;
}

AGENT_PLUGIN_EXPORT agents::IAgent* plugin_create_agent() {
    return new agents::FileAgent();
}

AGENT_PLUGIN_EXPORT void plugin_destroy_agent(agents::IAgent* agent) {
    delete agent;
}

AGENT_PLUGIN_EXPORT PluginExports* plugin_get_exports() {
    static PluginExports exports = {
        AGENT_PLUGIN_API_VERSION,
        plugin_get_name,
        plugin_get_version,
        plugin_get_api_version,
        reinterpret_cast<plugin_create_agent_fn>(plugin_create_agent),
        reinterpret_cast<plugin_destroy_agent_fn>(plugin_destroy_agent),
        nullptr,
        nullptr
    };
    return &exports;
}

} // extern "C"

#include "agents/security_manager.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <filesystem>

namespace agents {

/**
 * @brief Внутренняя реализация SecurityManager
 */
class SecurityManager::Impl {
public:
    SecurityLevel level = SecurityLevel::STANDARD;
    
    std::unordered_map<std::string, Permissions> permissions_map;
    std::unordered_set<std::string> trusted_paths;
    std::unordered_set<std::string> blocked_paths;
    
    std::unordered_map<std::string, std::unordered_set<std::string>> 
        allowed_commands_map;
    std::unordered_map<std::string, std::unordered_set<std::string>> 
        allowed_hosts_map;
    
    std::unordered_map<std::string, int> violation_stats;
    
    mutable std::mutex mutex;
    
    // Пути по умолчанию для блокировки
    std::vector<std::string> default_blocked = {
        "/etc/passwd",
        "/etc/shadow",
        "/etc/sudoers",
        "/root",
        "/boot",
        "/proc",
        "/sys",
    };
    
    Impl() {
        // Добавляем заблокированные пути по умолчанию
        for (const auto& path : default_blocked) {
            blocked_paths.insert(path);
        }
    }
};

// ============================================================================

SecurityManager::SecurityManager() : impl_(std::make_unique<Impl>()) {}

SecurityManager::~SecurityManager() = default;

void SecurityManager::set_security_level(SecurityLevel level) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->level = level;
    
    std::cout << "[SecurityManager] Security level set to " 
              << (level == SecurityLevel::NONE ? "NONE" :
                  level == SecurityLevel::STANDARD ? "STANDARD" :
                  level == SecurityLevel::STRICT ? "STRICT" : "SANDBOX")
              << std::endl;
}

SecurityLevel SecurityManager::get_security_level() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->level;
}

void SecurityManager::register_permissions(const std::string& agent_name,
                                            const Permissions& permissions) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->permissions_map[agent_name] = permissions;
    
    std::cout << "[SecurityManager] Registered permissions for: " 
              << agent_name << std::endl;
}

const Permissions* SecurityManager::get_permissions(
        const std::string& agent_name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    auto it = impl_->permissions_map.find(agent_name);
    if (it != impl_->permissions_map.end()) {
        return &it->second;
    }
    return nullptr;
}

SecurityCheckResult SecurityManager::check_file_access(
        const std::string& agent_name,
        const std::string& path,
        bool write) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    // Уровень NONE - всё разрешено
    if (impl_->level == SecurityLevel::NONE) {
        return SecurityCheckResult::allow();
    }
    
    // Проверка на заблокированные пути
    for (const auto& blocked : impl_->blocked_paths) {
        if (path.find(blocked) == 0) {
            impl_->violation_stats[agent_name]++;
            return SecurityCheckResult::deny(
                "Path is blocked: " + path);
        }
    }
    
    // Проверка разрешений агента
    auto it = impl_->permissions_map.find(agent_name);
    if (it != impl_->permissions_map.end()) {
        const Permissions& perms = it->second;
        
        // Проверка явного запрета
        for (const auto& denied : perms.denied_paths) {
            if (path.find(denied) == 0) {
                impl_->violation_stats[agent_name]++;
                return SecurityCheckResult::deny(
                    "Path explicitly denied: " + path);
            }
        }
        
        // Проверка явного разрешения
        if (!perms.allowed_paths.empty()) {
            bool allowed = false;
            for (const auto& allowed_path : perms.allowed_paths) {
                if (path.find(allowed_path) == 0) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                impl_->violation_stats[agent_name]++;
                return SecurityCheckResult::deny(
                    "Path not in allowed list: " + path);
            }
        }
        
        // Проверка прав на запись
        if (write && !perms.allow_filesystem) {
            impl_->violation_stats[agent_name]++;
            return SecurityCheckResult::deny(
                "Filesystem write access not permitted");
        }
    }
    
    // Проверка доверенных путей
    for (const auto& trusted : impl_->trusted_paths) {
        if (path.find(trusted) == 0) {
            return SecurityCheckResult::allow();
        }
    }
    
    return SecurityCheckResult::allow();
}

SecurityCheckResult SecurityManager::check_directory_access(
        const std::string& agent_name,
        const std::string& path) {
    return check_file_access(agent_name, path, false);
}

SecurityCheckResult SecurityManager::check_command(
        const std::string& agent_name,
        const std::string& command) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    if (impl_->level == SecurityLevel::NONE) {
        return SecurityCheckResult::allow();
    }
    
    // Проверка разрешений агента
    auto it = impl_->permissions_map.find(agent_name);
    if (it != impl_->permissions_map.end()) {
        const Permissions& perms = it->second;
        
        if (!perms.allow_terminal) {
            impl_->violation_stats[agent_name]++;
            return SecurityCheckResult::deny(
                "Terminal access not permitted");
        }
        
        // Проверка белого списка команд
        auto cmd_it = impl_->allowed_commands_map.find(agent_name);
        if (cmd_it != impl_->allowed_commands_map.end() &&
            !cmd_it->second.empty()) {
            
            // Извлекаем базовую команду (первое слово)
            std::string base_cmd = command;
            auto space_pos = command.find(' ');
            if (space_pos != std::string::npos) {
                base_cmd = command.substr(0, space_pos);
            }
            
            if (cmd_it->second.find(base_cmd) == cmd_it->second.end()) {
                impl_->violation_stats[agent_name]++;
                return SecurityCheckResult::deny(
                    "Command not in allowed list: " + base_cmd);
            }
        }
    }
    
    // Блокировка опасных команд
    static const std::vector<std::string> dangerous_commands = {
        "rm -rf /",
        "mkfs",
        "dd if=/dev/zero",
        ":(){:|:&};:",
        "chmod -R 777",
        "wget",
        "curl",
    };
    
    for (const auto& dangerous : dangerous_commands) {
        if (command.find(dangerous) == 0) {
            impl_->violation_stats[agent_name]++;
            return SecurityCheckResult::deny(
                "Dangerous command detected");
        }
    }
    
    return SecurityCheckResult::allow();
}

SecurityCheckResult SecurityManager::check_url(
        const std::string& agent_name,
        const std::string& url) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    if (impl_->level == SecurityLevel::NONE) {
        return SecurityCheckResult::allow();
    }
    
    // Проверка разрешений агента
    auto it = impl_->permissions_map.find(agent_name);
    if (it != impl_->permissions_map.end()) {
        const Permissions& perms = it->second;
        
        if (!perms.allow_network) {
            impl_->violation_stats[agent_name]++;
            return SecurityCheckResult::deny(
                "Network access not permitted");
        }
        
        // Проверка разрешённых хостов
        auto hosts_it = impl_->allowed_hosts_map.find(agent_name);
        if (hosts_it != impl_->allowed_hosts_map.end() &&
            !hosts_it->second.empty()) {
            
            bool allowed = false;
            for (const auto& host : hosts_it->second) {
                if (url.find(host) != std::string::npos) {
                    allowed = true;
                    break;
                }
            }
            
            if (!allowed) {
                impl_->violation_stats[agent_name]++;
                return SecurityCheckResult::deny(
                    "Host not in allowed list");
            }
        }
    }
    
    // Блокировка опасных URL
    if (url.find("file://") == 0) {
        impl_->violation_stats[agent_name]++;
        return SecurityCheckResult::deny(
            "file:// URLs are not allowed");
    }
    
    return SecurityCheckResult::allow();
}

bool SecurityManager::check_capability(const std::string& agent_name,
                                        AgentCapability capability) const {
    // В текущей реализации все возможности разрешены
    // В STRICT/SANDBOX режиме можно добавить дополнительную проверку
    return true;
}

SecurityCheckResult SecurityManager::check_memory_allocation(
        const std::string& agent_name,
        int size_mb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    
    auto it = impl_->permissions_map.find(agent_name);
    if (it != impl_->permissions_map.end()) {
        const Permissions& perms = it->second;
        
        if (size_mb > perms.max_memory_mb) {
            impl_->violation_stats[agent_name]++;
            return SecurityCheckResult::deny(
                "Memory limit exceeded: " + std::to_string(size_mb) +
                " MB > " + std::to_string(perms.max_memory_mb) + " MB");
        }
    }
    
    // Лимит по умолчанию
    if (size_mb > 1024) {
        impl_->violation_stats[agent_name]++;
        return SecurityCheckResult::deny(
            "Memory allocation too large: " + std::to_string(size_mb) + " MB");
    }
    
    return SecurityCheckResult::allow();
}

void SecurityManager::add_trusted_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->trusted_paths.insert(path);
}

void SecurityManager::add_blocked_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->blocked_paths.insert(path);
}

void SecurityManager::add_allowed_command(const std::string& agent_name,
                                           const std::string& command) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->allowed_commands_map[agent_name].insert(command);
}

void SecurityManager::add_allowed_host(const std::string& agent_name,
                                        const std::string& host) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->allowed_hosts_map[agent_name].insert(host);
}

SecurityCheckResult SecurityManager::scan_file(const std::string& path) {
    // Простая проверка на существование и читаемость
    if (!std::filesystem::exists(path)) {
        return SecurityCheckResult::deny("File does not exist");
    }
    
    // Проверка размера файла
    try {
        auto size = std::filesystem::file_size(path);
        if (size > 100 * 1024 * 1024) {  // 100 MB
            return SecurityCheckResult::deny("File too large");
        }
    } catch (const std::exception& e) {
        return SecurityCheckResult::deny("Cannot read file size");
    }
    
    return SecurityCheckResult::allow();
}

SecurityCheckResult SecurityManager::validate_plugin(
        const std::string& plugin_path) {
    // Проверка существования
    if (!std::filesystem::exists(plugin_path)) {
        return SecurityCheckResult::deny("Plugin file does not exist");
    }
    
    // Проверка расширения
    std::string ext = std::filesystem::path(plugin_path).extension().string();
    if (ext != ".so" && ext != ".dll") {
        return SecurityCheckResult::deny(
            "Invalid plugin extension (expected .so or .dll)");
    }
    
    // Проверка размера
    try {
        auto size = std::filesystem::file_size(plugin_path);
        if (size > 50 * 1024 * 1024) {  // 50 MB
            return SecurityCheckResult::deny("Plugin file too large");
        }
    } catch (const std::exception& e) {
        return SecurityCheckResult::deny("Cannot read plugin file size");
    }
    
    return SecurityCheckResult::allow();
}

std::unordered_map<std::string, int> SecurityManager::get_violation_stats() 
        const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->violation_stats;
}

void SecurityManager::reset_violation_stats() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->violation_stats.clear();
}

} // namespace agents

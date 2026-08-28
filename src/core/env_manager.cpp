#include "../../include/core/env_manager.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <vector>
#include <algorithm>

namespace llama_gui {
namespace core {

std::string EnvManager::get_env_path(const std::string& profiles_dir) {
    return profiles_dir + "/.env";
}

std::string EnvManager::read_key(const std::string& key_name,
                                 const std::string& profiles_dir) {
    std::string path = get_env_path(profiles_dir);
    std::ifstream file(path);
    if (!file.is_open()) return "";

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Find '=' separator
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Trim whitespace from key
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);

        if (key == key_name) {
            // Trim surrounding quotes from value
            if (value.size() >= 2 &&
                ((value.front() == '"' && value.back() == '"') ||
                 (value.front() == '\'' && value.back() == '\''))) {
                value = value.substr(1, value.size() - 2);
            }
            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            return value;
        }
    }
    return "";
}

void EnvManager::write_key(const std::string& key_name,
                           const std::string& value,
                           const std::string& profiles_dir) {
    std::string path = get_env_path(profiles_dir);

    // Read existing lines
    std::vector<std::string> lines;
    bool found = false;

    {
        std::ifstream file(path);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                auto eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string key = line.substr(0, eq);
                    key.erase(0, key.find_first_not_of(" \t\r\n"));
                    key.erase(key.find_last_not_of(" \t\r\n") + 1);
                    if (key == key_name) {
                        lines.push_back(key_name + "=" + value);
                        found = true;
                        continue;
                    }
                }
                lines.push_back(line);
            }
        }
    }

    if (!found) {
        lines.push_back(key_name + "=" + value);
    }

    // Ensure parent directory exists
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "EnvManager: Failed to open " << path << " for writing" << std::endl;
        return;
    }

    for (const auto& line : lines) {
        file << line << "\n";
    }

    std::cout << "EnvManager: Wrote key '" << key_name << "' to " << path << std::endl;
}

void EnvManager::remove_key(const std::string& key_name,
                            const std::string& profiles_dir) {
    std::string path = get_env_path(profiles_dir);
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            key.erase(0, key.find_first_not_of(" \t\r\n"));
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            if (key == key_name) continue; // skip this line
        }
        lines.push_back(line);
    }
    file.close();

    std::ofstream out(path);
    if (!out.is_open()) return;
    for (const auto& l : lines) {
        out << l << "\n";
    }
}

std::vector<std::pair<std::string, std::string>> EnvManager::read_all_keys(
    const std::string& profiles_dir) {
    std::vector<std::pair<std::string, std::string>> result;
    std::ifstream file(get_env_path(profiles_dir));
    if (!file.is_open()) return result;

    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    };

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string value = line.substr(eq + 1);

        // Trim surrounding quotes from value (same as read_key)
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        value = trim(value);

        // Later duplicates win, matching write_key overwrite semantics
        bool replaced = false;
        for (auto& entry : result) {
            if (entry.first == key) {
                entry.second = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            result.emplace_back(key, value);
        }
    }
    return result;
}

std::string EnvManager::cloud_provider_api_key_name(const std::string& provider_name,
                                                    const std::string& endpoint_url) {
    // Each built-in provider gets a dedicated key slot so switching providers
    // never reuses or overwrites another provider's key.
    const std::string provider_lower = provider_name;
    const std::string url_lower = endpoint_url;
    bool is_opencode_zen =
        provider_lower.find("opencode zen") != std::string::npos ||
        provider_lower.find("opencode") != std::string::npos ||
        url_lower.find("opencode.ai") != std::string::npos;
    bool is_zhipu =
        provider_lower.find("zhipu") != std::string::npos ||
        provider_lower.find("glm") != std::string::npos ||
        url_lower.find("bigmodel.cn") != std::string::npos;
    bool is_pollinations =
        provider_lower.find("pollinations") != std::string::npos ||
        url_lower.find("pollinations.ai") != std::string::npos;
    bool is_ovh =
        provider_lower.find("ovh") != std::string::npos ||
        url_lower.find("kepler.ai.cloud.ovh.net") != std::string::npos;
    bool is_qwen =
        provider_lower.find("qwen") != std::string::npos ||
        url_lower.find("dashscope") != std::string::npos;

    if (is_opencode_zen) {
        return "OPENCODE_ZEN_API_KEY";
    }
    if (is_zhipu) {
        return "ZHIPU_GLM_API_KEY";
    }
    if (is_pollinations) {
        return "POLLINATIONS_API_KEY";
    }
    if (is_ovh) {
        return "OVH_AI_API_KEY";
    }
    if (is_qwen) {
        return "QWEN_API_KEY";
    }

    return "CLOUD_PROVIDER_API_KEY";
}

} // namespace core
} // namespace llama_gui

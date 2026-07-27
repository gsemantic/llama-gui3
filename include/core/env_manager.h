#pragma once

#include <string>

namespace llama_gui {
namespace core {

/**
 * @brief Utility for reading/writing .env files
 *
 * Stores secrets (API keys) outside of profile JSON files.
 * File location: <profiles_dir>/.env (sibling to profile JSONs)
 */
class EnvManager {
public:
    /**
     * @brief Get the path to the .env file
     * @param profiles_dir Directory containing profile JSONs (default: "profiles")
     */
    static std::string get_env_path(const std::string& profiles_dir = "profiles");

    /**
     * @brief Read a key from the .env file
     * @return The value, or empty string if key not found
     */
    static std::string read_key(const std::string& key_name,
                                const std::string& profiles_dir = "profiles");

    /**
     * @brief Write/update a key in the .env file
     * Creates the file if it doesn't exist.
     */
    static void write_key(const std::string& key_name,
                          const std::string& value,
                          const std::string& profiles_dir = "profiles");

    /**
     * @brief Remove a key from the .env file
     */
    static void remove_key(const std::string& key_name,
                           const std::string& profiles_dir = "profiles");
};

} // namespace core
} // namespace llama_gui

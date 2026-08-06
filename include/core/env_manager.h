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

    /**
     * @brief Get the .env key name used to store the API key for a cloud provider
     *
     * Keyless providers (e.g. OpenCode Zen) map to a dedicated key so that
     * switching providers never overwrites/removes another provider's key.
     * @param provider_name Provider display name (e.g. "OpenCode Zen")
     * @param endpoint_url  Provider endpoint URL
     * @return Env key name to use for this provider
     */
    static std::string cloud_provider_api_key_name(const std::string& provider_name,
                                                   const std::string& endpoint_url);
};

} // namespace core
} // namespace llama_gui

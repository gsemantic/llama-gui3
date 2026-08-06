#include "../include/core/settings.h"
#include <iostream>
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

// =========================================================================
// Основная сериализация - вызывает модульные функции
// =========================================================================

std::string Settings::serialize_to_json() const {
    json j;

    // Основные настройки
    serializeDisplaySettings(j);
    serializeServerSettings(j);
    serializeChatSettings(j);
    serializeFileSettings(j);
    serializePerformanceSettings(j);
    serializeRagSettings(j);

    // Расширенные настройки llama.cpp
    serializeSamplingSettings(j);
    serializeModelLoadingSettings(j);
    serializeGpuSettings(j);
    serializeCacheSettings(j);
    serializeRopeSettings(j);
    serializeControlVectorSettings(j);
    serializeTensorOverrideSettings(j);

    // Настройки сервера и выполнения
    serializeServerRuntimeSettings(j);
    serializeBatchSettings(j);
    serializeGrammarSettings(j);
    serializeOutputSettings(j);

    // OpenRouter настройки
    serializeOpenRouterSettings(j);

    // Статистика производительности моделей
    j["model_performance"] = json::parse(model_performance_manager_.to_json());

    // Custom settings
    j["custom"] = custom_settings_;

    return j.dump(4);
}

bool Settings::deserialize_from_json(const std::string& json_data) {
    try {
        auto j = json::parse(json_data);

        // Основные настройки
        deserializeDisplaySettings(j);
        deserializeServerSettings(j);
        deserializeChatSettings(j);
        deserializeFileSettings(j);
        deserializePerformanceSettings(j);
        deserializeRagSettings(j);

        // Расширенные настройки llama.cpp
        deserializeSamplingSettings(j);
        deserializeModelLoadingSettings(j);
        deserializeGpuSettings(j);
        deserializeCacheSettings(j);
        deserializeRopeSettings(j);
        deserializeControlVectorSettings(j);
        deserializeTensorOverrideSettings(j);

        // Настройки сервера и выполнения
        deserializeServerRuntimeSettings(j);
        deserializeBatchSettings(j);
        deserializeGrammarSettings(j);
        deserializeOutputSettings(j);

        // OpenRouter настройки
        deserializeOpenRouterSettings(j);

        // Статистика производительности моделей
        if (j.contains("model_performance")) {
            std::string perf_json = j["model_performance"].dump();
            model_performance_manager_.from_json(perf_json);
        }

        // Custom settings
        if (j.contains("custom")) {
            custom_settings_.clear();
            for (auto& [key, value] : j["custom"].items()) {
                custom_settings_[key] = value.get<std::string>();
            }
        }

        std::cout << "Settings successfully deserialized from JSON" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to deserialize settings: " << e.what() << std::endl;
        return false;
    }
}

} // namespace core
} // namespace llama_gui

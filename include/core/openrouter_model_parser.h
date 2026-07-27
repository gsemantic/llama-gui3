#pragma once

#include "openrouter_types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace llama_gui {
namespace core {

/**
 * @brief Парсер моделей OpenRouter API
 *
 * Инкапсулирует парсинг JSON ответов и фильтрацию моделей.
 */
class OpenRouterModelParser {
public:
    OpenRouterModel parse_model(const nlohmann::json& json) const;
    OpenRouterModelsResponse parse_models_response(const std::string& json_str) const;

    std::vector<OpenRouterModel> filter_models(
        const std::vector<OpenRouterModel>& models,
        const std::string& query,
        bool free_only
    ) const;

    OpenRouterCompletionResponse parse_completion_response(const std::string& json_str) const;

private:
    static std::string sanitize_response_text(const std::string& input);
};

} // namespace core
} // namespace llama_gui

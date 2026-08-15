#include "../../include/core/openrouter_model_parser.h"
#include "../../include/core/logger.h"
#include <algorithm>
#include <iostream>

using json = nlohmann::json;

namespace llama_gui {
namespace core {

// ============================================================================
// sanitize_response_text
// ============================================================================

std::string OpenRouterModelParser::sanitize_response_text(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ) {
        unsigned char c = static_cast<unsigned char>(input[i]);

        if (c >= 0x20 && c <= 0x7E) {
            output += c;
            i++;
            continue;
        }

        if (c == '\n' || c == '\r' || c == '\t') {
            output += c;
            i++;
            continue;
        }

        if (c >= 0xC0) {
            int seq_len = 0;

            if ((c & 0xE0) == 0xC0) seq_len = 2;
            else if ((c & 0xF0) == 0xE0) seq_len = 3;
            else if ((c & 0xF8) == 0xF0) seq_len = 4;
            else {
                i++;
                continue;
            }

            bool valid = true;
            if (i + seq_len <= input.size()) {
                for (int j = 1; j < seq_len; j++) {
                    if ((static_cast<unsigned char>(input[i + j]) & 0xC0) != 0x80) {
                        valid = false;
                        break;
                    }
                }
            } else {
                valid = false;
            }

            if (valid) {
                if (seq_len == 1 || (seq_len == 2 && c < 0xC2)) {
                    i += seq_len;
                    continue;
                }
                output += input.substr(i, seq_len);
                i += seq_len;
            } else {
                i++;
            }
            continue;
        }

        i++;
    }

    return output;
}

// ============================================================================
// parse_model
// ============================================================================

OpenRouterModel OpenRouterModelParser::parse_model(const json& j) const {
    OpenRouterModel model;

    model.id = j.value("id", "");
    model.name = j.value("name", model.id);

    if (j.contains("provider")) {
        const auto& provider = j["provider"];
        model.provider = provider.value("name", "");
    }

    model.description = j.value("description", "");
    model.context_length = j.value("context_length", static_cast<int64_t>(0));

    model.is_free = false;

    if (j.contains("top_provider") && j["top_provider"].is_object()) {
        model.is_free = j["top_provider"].value("is_free", false);
    }

    if (j.contains("pricing") && j["pricing"].is_object()) {
        const auto& pricing = j["pricing"];
        std::string prompt_price = pricing.value("prompt", "0");
        if (prompt_price == "0" || prompt_price == "0.0" || prompt_price.empty()) {
            model.is_free = true;
        }
    }

    if (j.contains("pricing")) {
        const auto& pricing = j["pricing"];
        try {
            std::string prompt_price = pricing.value("prompt", "0");
            std::string completion_price = pricing.value("completion", "0");
            model.prompt_price_usd_per_million = std::stod(prompt_price);
            model.completion_price_usd_per_million = std::stod(completion_price);
        } catch (...) {
            model.prompt_price_usd_per_million = 0.0;
            model.completion_price_usd_per_million = 0.0;
        }
    }

    if (j.contains("topology")) {
        const auto& topology = j["topology"];
        model.topology = topology.value("type", "");
    }

    if (j.contains("modality")) {
        const auto& modality = j["modality"];
        if (modality.is_array()) {
            for (const auto& m : modality) {
                model.modality.push_back(m.get<std::string>());
            }
        }
    }

    return model;
}

// ============================================================================
// parse_models_response
// ============================================================================

OpenRouterModelsResponse OpenRouterModelParser::parse_models_response(const std::string& json_str) const {
    OpenRouterModelsResponse response;

    try {
        json data = json::parse(json_str);

        if (data.contains("data") && data["data"].is_array()) {
            for (const auto& model_json : data["data"]) {
                OpenRouterModel model = parse_model(model_json);
                response.models.push_back(model);
            }

            std::sort(response.models.begin(), response.models.end());
            response.success = true;
        } else {
            response.error = "Неверный формат ответа API";
        }

    } catch (const json::parse_error& e) {
        response.error = "Ошибка парсинга JSON: " + std::string(e.what());
    } catch (const std::exception& e) {
        response.error = "Ошибка обработки: " + std::string(e.what());
    }

    return response;
}

// ============================================================================
// filter_models
// ============================================================================

std::vector<OpenRouterModel> OpenRouterModelParser::filter_models(
    const std::vector<OpenRouterModel>& models,
    const std::string& query,
    bool free_only
) const {
    std::vector<OpenRouterModel> result;

    std::string query_lower = query;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

    for (const auto& model : models) {
        if (free_only && !model.is_free) {
            continue;
        }

        if (query.empty()) {
            result.push_back(model);
            continue;
        }

        std::string name_lower = model.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

        std::string id_lower = model.id;
        std::transform(id_lower.begin(), id_lower.end(), id_lower.begin(), ::tolower);

        std::string provider_lower = model.provider;
        std::transform(provider_lower.begin(), provider_lower.end(), provider_lower.begin(), ::tolower);

        if (name_lower.find(query_lower) != std::string::npos ||
            id_lower.find(query_lower) != std::string::npos ||
            provider_lower.find(query_lower) != std::string::npos) {
            result.push_back(model);
        }
    }

    return result;
}

// ============================================================================
// parse_completion_response
// ============================================================================

// Извлекает текст из поля content, которое у разных провайдеров может быть
// строкой либо массивом частей вида [{"type":"text","text":"..."}, ...]
// (Zhipu/GLM возвращают именно массив). Иначе — пусто.
std::string extract_content_text(const json& content) {
    if (content.is_string()) return content.get<std::string>();
    if (content.is_array()) {
        std::string out;
        for (const auto& part : content) {
            if (part.is_string()) {
                out += part.get<std::string>();
                continue;
            }
            if (part.is_object()) {
                const std::string t = part.value("text", "");
                if (!t.empty()) {
                    out += t;
                } else if (part.contains("content") && part["content"].is_string()) {
                    out += part["content"].get<std::string>();
                }
            }
        }
        return out;
    }
    return "";
}

OpenRouterCompletionResponse OpenRouterModelParser::parse_completion_response(const std::string& json_str) const {
    OpenRouterCompletionResponse response;

    // Handle empty response (timeout, network error, etc.)
    if (json_str.empty()) {
        response.success = false;
        response.error = "Empty response from server (timeout or network error)";
        return response;
    }

    try {
        json data = json::parse(json_str);

        if (data.contains("error")) {
            const auto& error = data["error"];
            std::string error_msg = error.value("message", "Неизвестная ошибка API");

            // Handle both string and number error codes (Zhipu API returns string "1305")
            int error_code = -1;
            if (error.contains("code")) {
                const auto& code_val = error["code"];
                if (code_val.is_number_integer()) {
                    error_code = code_val.get<int>();
                } else if (code_val.is_string()) {
                    try {
                        error_code = std::stoi(code_val.get<std::string>());
                    } catch (...) {
                        error_code = -1;
                    }
                }
            }

            std::string user_friendly_msg;

            if (error_code == 429) {
                std::string provider_name = "API";
                std::string model_name = "";

                if (error.contains("metadata")) {
                    const auto& metadata = error["metadata"];
                    if (metadata.contains("provider_name")) {
                        provider_name = metadata.value("provider_name", "API");
                    }
                    if (metadata.contains("raw")) {
                        std::string raw_msg = metadata.value("raw", "");
                        size_t pos = raw_msg.find(":");
                        if (pos != std::string::npos) {
                            model_name = raw_msg.substr(0, pos);
                        }
                    }
                }

                user_friendly_msg = "Превышен лимит запросов (Rate Limit)\n\n";
                user_friendly_msg += "Модель: " + model_name + "\n";
                user_friendly_msg += "Провайдер: " + provider_name + "\n\n";
                user_friendly_msg += "Причины:\n";
                user_friendly_msg += "- Бесплатные модели имеют ограничения по количеству запросов\n";
                user_friendly_msg += "- Слишком много запросов за короткое время\n\n";
                user_friendly_msg += "Решения:\n";
                user_friendly_msg += "1. Подождите 5-10 минут и попробуйте снова\n";
                user_friendly_msg += "2. Используйте локальную модель (llama-server)\n";
                user_friendly_msg += "3. Добавьте свой API ключ OpenRouter (платно)\n";

                LOG_ERROR("[OpenRouter] Rate limit exceeded: " + error_msg);

            } else if (error_code == 401) {
                user_friendly_msg = "Ошибка авторизации\n\n";
                user_friendly_msg += "Проверьте API ключ OpenRouter:\n";
                user_friendly_msg += "- Настройки -> OpenRouter -> API Key\n";
                user_friendly_msg += "- Или используйте локальную модель";

                LOG_ERROR("[OpenRouter] Authentication failed: " + error_msg);

            } else if (error_code == 403) {
                user_friendly_msg = "Доступ запрещён\n\n";
                user_friendly_msg += "Возможные причины:\n";
                user_friendly_msg += "- Модель недоступна в вашем регионе\n";
                user_friendly_msg += "- Требуется платная подписка\n\n";
                user_friendly_msg += "Попробуйте другую модель или локальную.";

                LOG_ERROR("[OpenRouter] Access forbidden: " + error_msg);

            } else if (error_code == 404) {
                user_friendly_msg = "Модель не найдена\n\n";
                user_friendly_msg += "Проверьте название модели или выберите другую.";

                LOG_ERROR("[OpenRouter] Model not found: " + error_msg);

            } else if (error_code == 1305) {
                // Zhipu-specific: rate limit exceeded ("该模型当前访问量过大")
                user_friendly_msg = "Превышен лимит запросов (Rate Limit)\n\n";
                user_friendly_msg += "Модель временно перегружена.\n\n";
                user_friendly_msg += "Решения:\n";
                user_friendly_msg += "1. Подождите 1-2 минуты и попробуйте снова\n";
                user_friendly_msg += "2. Используйте другую бесплатную модель (GLM-4.7-Flash)\n";
                user_friendly_msg += "3. Используйте локальную модель (llama-server)";

                LOG_ERROR("[CloudProvider] Rate limit (1305): " + error_msg);

            } else if (error_code >= 500) {
                user_friendly_msg = "Ошибка сервера (" + std::to_string(error_code) + ")\n\n";
                user_friendly_msg += "Проблема на стороне провайдера.\n";
                user_friendly_msg += "Попробуйте позже или используйте другую модель.";

                LOG_ERROR("[OpenRouter] Server error " + std::to_string(error_code) + ": " + error_msg);

            } else {
                user_friendly_msg = "Ошибка API (код: " + std::to_string(error_code) + ")\n\n";
                user_friendly_msg += error_msg;

                LOG_ERROR("[OpenRouter] API error " + std::to_string(error_code) + ": " + error_msg);
            }

            response.error = user_friendly_msg;
            response.success = false;
            return response;
        }

        response.id = data.value("id", "");
        response.model = data.value("model", "");

        if (data.contains("choices") && data["choices"].is_array() && !data["choices"].empty()) {
            const auto& choice = data["choices"][0];
            // Контент может быть строкой, массивом частей (Zhipu/GLM:
            // [{"type":"text","text":"..."}]) либо лежать в delta, если
            // провайдер вернул стриминг-формат даже для нестримингового
            // запроса. Собираем текст из любого варианта.
            std::string text;
            if (choice.contains("message") && choice["message"].contains("content")) {
                text = extract_content_text(choice["message"]["content"]);
            } else if (choice.contains("delta") && choice["delta"].contains("content")) {
                text = extract_content_text(choice["delta"]["content"]);
            }
            response.content = sanitize_response_text(text);
            response.finish_reason = choice.value("finish_reason", "");
        }

        if (response.content.empty() && !data.contains("error")) {
            // Нестандартный/неожиданный ответ: нет контента и нет поля error
            // в обычном формате. Например, OpenCode Zen при HTTP 403 Forbidden
            // возвращает просто {"model":"<id>"}. Формируем понятное сообщение
            // вместо сброса сырого JSON пользователю.
            if (!data.contains("choices")) {
                std::string model_hint = data.value("model", "");
                std::string msg = "Ошибка провайдера: ответ не содержит контент и не является "
                                  "ошибкой в стандартном формате.\n\n";
                if (!model_hint.empty()) {
                    msg += "Модель: " + model_hint + "\n";
                }
                msg += "Возможные причины:\n";
                msg += "- HTTP 403 Forbidden: доступ к модели запрещён (требуется API-ключ, "
                       "либо модель недоступна для вашего ключа/тарифа)\n";
                msg += "- Провайдер вернул нестандартный ответ\n\n";
                msg += "Проверьте API-ключ в настройках облачного провайдера и попробуйте "
                       "другую модель.";
                response.success = false;
                response.error = msg;
                LOG_ERROR("[CloudParser] Нестандартный ответ без контента: " +
                          (json_str.size() > 400 ? json_str.substr(0, 400) : json_str));
                return response;
            }
            LOG_WARNING("[CloudParser] content пуст при успешном парсинге; head: " +
                        (json_str.size() > 400 ? json_str.substr(0, 400) : json_str));
        }

        if (data.contains("usage")) {
            const auto& usage = data["usage"];
            response.prompt_tokens = usage.value("prompt_tokens", 0);
            response.completion_tokens = usage.value("completion_tokens", 0);
            response.total_tokens = usage.value("total_tokens", 0);
        }

        if (data.contains("cost")) {
            response.cost_usd = data.value("cost", 0.0);
        }

        response.success = true;

    } catch (const json::parse_error& e) {
        response.error = "Ошибка парсинга ответа\n\n";
        response.error += "Не удалось обработать ответ от сервера.\n";
        response.error += "Проверьте подключение к интернету.\n\n";
        response.error += "Детали: " + std::string(e.what());
        response.success = false;
    } catch (const std::exception& e) {
        response.error = "Внутренняя ошибка\n\n";
        response.error += "Произошла непредвиденная ошибка.\n\n";
        response.error += "Детали: " + std::string(e.what());
        response.success = false;
    }

    return response;
}

} // namespace core
} // namespace llama_gui

#include "../../include/core/openrouter_client.h"
#include "../../include/core/logger.h"
#include <thread>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>

using namespace llama_gui::core;

namespace llama_gui {
namespace core {

// ============================================================================
// Конструктор/деструктор
// ============================================================================

OpenRouterClient::OpenRouterClient(const std::string& api_key)
    : rate_limiter_(http_client_) {
    http_client_.set_api_key(api_key);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::cout << "[CloudClient] Initialized with key: "
              << (api_key.empty() ? "NONE" : api_key.substr(0, 8) + "...") << std::endl;
}

OpenRouterClient::~OpenRouterClient() {
    curl_global_cleanup();
}

// ============================================================================
// Настройки
// ============================================================================

void OpenRouterClient::set_api_key(const std::string& api_key) {
    http_client_.set_api_key(api_key);
}

void OpenRouterClient::set_base_url(const std::string& url) {
    http_client_.set_base_url(url);
}

void OpenRouterClient::set_timeout(int timeout_ms) {
    http_client_.set_timeout(timeout_ms);
}

// ============================================================================
// Получение моделей
// ============================================================================

OpenRouterModelsResponse OpenRouterClient::get_models() {
    std::string response = http_client_.make_request("models");
    return model_parser_.parse_models_response(response);
}

bool OpenRouterClient::get_models_async(ModelsCallback callback) {
    if (!callback) return false;

    std::thread([this, callback]() {
        auto response = get_models();
        callback(response);
    }).detach();

    return true;
}

OpenRouterModelsResponse OpenRouterClient::get_free_models() {
    auto response = get_models();

    if (response.success) {
        std::vector<OpenRouterModel> free_models;
        for (const auto& model : response.models) {
            if (model.is_free) {
                free_models.push_back(model);
            }
        }
        response.models = free_models;
    }

    return response;
}

bool OpenRouterClient::get_free_models_async(ModelsCallback callback) {
    if (!callback) return false;

    std::thread([this, callback]() {
        auto response = get_free_models();
        callback(response);
    }).detach();

    return true;
}

// ============================================================================
// Поиск моделей
// ============================================================================

OpenRouterModelsResponse OpenRouterClient::search_models(const std::string& query, bool free_only) {
    auto response = get_models();

    if (response.success) {
        response.models = model_parser_.filter_models(response.models, query, free_only);
    }

    return response;
}

bool OpenRouterClient::search_models_async(const std::string& query, bool free_only, ModelsCallback callback) {
    if (!callback) return false;

    std::thread([this, query, free_only, callback]() {
        auto response = search_models(query, free_only);
        callback(response);
    }).detach();

    return true;
}

// ============================================================================
// Детали модели
// ============================================================================

OpenRouterModelDetails OpenRouterClient::get_model_details(const std::string& model_id) {
    OpenRouterModelDetails details;
    details.id = model_id;

    auto response = get_models();

    if (!response.success) {
        details.error = response.error;
        details.success = false;
        return details;
    }

    for (const auto& model : response.models) {
        if (model.id == model_id) {
            details.name = model.name;
            details.description = model.description;
            details.provider_name = model.provider;
            details.context_length = model.context_length;
            details.prompt_price_usd_per_million = model.prompt_price_usd_per_million;
            details.completion_price_usd_per_million = model.completion_price_usd_per_million;
            details.is_free = model.is_free;
            details.modality = model.modality;
            details.success = true;
            return details;
        }
    }

    details.error = "Модель не найдена";
    details.success = false;
    return details;
}

// ============================================================================
// Генерация текста
// ============================================================================

OpenRouterCompletionResponse OpenRouterClient::complete(const OpenRouterRequestParams& params) {
    nlohmann::json request_body;

    request_body["model"] = params.model;
    request_body["messages"] = nlohmann::json::array();

    if (!params.messages.empty()) {
        for (const auto& msg : params.messages) {
            nlohmann::json message;
            message["role"] = msg.role;
            message["content"] = msg.content;
            request_body["messages"].push_back(message);
        }
    } else if (!params.prompt.empty()) {
        if (!params.system_prompt.empty()) {
            nlohmann::json system_msg;
            system_msg["role"] = "system";
            system_msg["content"] = params.system_prompt;
            request_body["messages"].push_back(system_msg);
        }
        nlohmann::json user_msg;
        user_msg["role"] = "user";
        user_msg["content"] = params.prompt;
        request_body["messages"].push_back(user_msg);
    }

    if (params.max_tokens != 1024) {
        request_body["max_tokens"] = params.max_tokens;
    }
    if (params.temperature != 0.7f) {
        request_body["temperature"] = params.temperature;
    }
    if (params.top_p != 0.9f) {
        request_body["top_p"] = params.top_p;
    }
    if (params.presence_penalty != 0.0f) {
        request_body["presence_penalty"] = params.presence_penalty;
    }
    if (params.frequency_penalty != 0.0f) {
        request_body["frequency_penalty"] = params.frequency_penalty;
    }

    request_body["stream"] = params.stream;

    std::string response_str = http_client_.make_request("chat/completions", request_body.dump());
    return model_parser_.parse_completion_response(response_str);
}

bool OpenRouterClient::complete_streaming_async(const OpenRouterRequestParams& params, StreamCallback callback) {
    if (!callback) return false;

    std::thread([this, params, callback]() {
        auto response = complete(params);
        if (response.success) {
            callback(response.content, true);
        }
    }).detach();

    return true;
}

bool OpenRouterClient::complete_streaming_with_retry_async(const OpenRouterRequestParams& params, 
                                                            StreamCallback callback,
                                                            int max_retries) {
    if (!callback) return false;

    std::thread([this, params, callback, max_retries]() {
        int attempt = 0;
        bool success = false;
        
        while (attempt < max_retries && !success) {
            try {
                // Создаем параметры запроса с включенным стримингом
                OpenRouterRequestParams stream_params = params;
                stream_params.stream = true;
                
                std::cout << "[CloudClient] Streaming attempt " << (attempt + 1) << "/" << max_retries << std::endl;
                
                std::string accumulated_content;
                std::mutex content_mutex;
                
                // Выполняем streaming запрос
                std::string error = http_client_.make_streaming_request(
                    "chat/completions",
                    build_request_body(stream_params),
                    [&accumulated_content, &content_mutex, callback](const std::string& chunk) {
                        // Парсим JSON чанк и извлекаем контент
                        try {
                            auto json_chunk = nlohmann::json::parse(chunk);
                            
                            if (json_chunk.contains("choices") && !json_chunk["choices"].empty()) {
                                auto& choice = json_chunk["choices"][0];
                                std::string token;
                                
                                // Пробуем получить токен из delta (для streaming)
                                if (choice.contains("delta") && choice["delta"].contains("content")) {
                                    auto content = choice["delta"]["content"];
                                    if (!content.is_null()) {
                                        token = content.get<std::string>();
                                    }
                                }
                                // Или из message (для некоторых провайдеров)
                                else if (choice.contains("message") && choice["message"].contains("content")) {
                                    auto content = choice["message"]["content"];
                                    if (!content.is_null()) {
                                        token = content.get<std::string>();
                                    }
                                }
                                
                                if (!token.empty()) {
                                    std::lock_guard<std::mutex> lock(content_mutex);
                                    accumulated_content += token;
                                    // Вызываем callback с токеном (не финальный)
                                    callback(token, false);
                                }
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "[CloudClient] Error parsing chunk: " << e.what() << std::endl;
                        }
                    }
                );
                
                if (error.empty()) {
                    // Успешно завершено
                    success = true;
                    // Сигнализируем о завершении
                    callback("", true);
                    std::cout << "[CloudClient] Streaming completed successfully" << std::endl;
                } else {
                    // Ошибка - увеличиваем счетчик попыток
                    attempt++;
                    std::cerr << "[CloudClient] Streaming error: " << error << std::endl;
                    
                    if (attempt < max_retries) {
                        // Экспоненциальная задержка перед повторной попыткой
                        int delay_ms = 1000 * attempt;  // 1s, 2s, 3s...
                        std::cout << "[CloudClient] Retrying in " << delay_ms << "ms..." << std::endl;
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                        
                        // Сообщаем пользователю о повторной попытке
                        std::string retry_msg = "\n[Попытка " + std::to_string(attempt + 1) + "/" + std::to_string(max_retries) + "]... ";
                        callback(retry_msg, false);
                    }
                }
            } catch (const std::exception& e) {
                attempt++;
                std::cerr << "[CloudClient] Exception: " << e.what() << std::endl;
                
                if (attempt >= max_retries) {
                    callback("\n[Ошибка: " + std::string(e.what()) + "]", true);
                }
            }
        }
        
        if (!success && attempt >= max_retries) {
            callback("\n[Не удалось выполнить запрос после " + std::to_string(max_retries) + " попыток]", true);
        }
    }).detach();

    return true;
}

// Вспомогательный метод для построения тела запроса
std::string OpenRouterClient::build_request_body(const OpenRouterRequestParams& params) {
    nlohmann::json request_body;

    request_body["model"] = params.model;
    request_body["messages"] = nlohmann::json::array();

    if (!params.messages.empty()) {
        for (const auto& msg : params.messages) {
            nlohmann::json message;
            message["role"] = msg.role;
            message["content"] = msg.content;
            request_body["messages"].push_back(message);
        }
    } else if (!params.prompt.empty()) {
        if (!params.system_prompt.empty()) {
            nlohmann::json system_msg;
            system_msg["role"] = "system";
            system_msg["content"] = params.system_prompt;
            request_body["messages"].push_back(system_msg);
        }
        nlohmann::json user_msg;
        user_msg["role"] = "user";
        user_msg["content"] = params.prompt;
        request_body["messages"].push_back(user_msg);
    }

    if (params.max_tokens != 1024) {
        request_body["max_tokens"] = params.max_tokens;
    }
    if (params.temperature != 0.7f) {
        request_body["temperature"] = params.temperature;
    }
    if (params.top_p != 0.9f) {
        request_body["top_p"] = params.top_p;
    }
    if (params.presence_penalty != 0.0f) {
        request_body["presence_penalty"] = params.presence_penalty;
    }
    if (params.frequency_penalty != 0.0f) {
        request_body["frequency_penalty"] = params.frequency_penalty;
    }

    // Включаем стриминг
    request_body["stream"] = true;
    
    // Добавляем stream_options для получения статистики токенов
    request_body["stream_options"] = {
        {"include_usage", true}
    };

    return request_body.dump();
}

// ============================================================================
// API
// ============================================================================

bool OpenRouterClient::is_api_available() {
    return rate_limiter_.is_api_available();
}

OpenRouterRateLimit OpenRouterClient::get_rate_limit() {
    return rate_limiter_.get_rate_limit();
}

} // namespace core
} // namespace llama_gui

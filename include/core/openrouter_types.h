#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace llama_gui {
namespace core {

/**
 * @brief Информация о модели OpenRouter
 */
struct OpenRouterModel {
    std::string id;                    // Уникальный идентификатор модели (например, "meta-llama/llama-3-8b-instruct")
    std::string name;                  // Отображаемое имя
    std::string provider;              // Провайдер модели
    std::string description;           // Описание модели
    int64_t context_length = 0;        // Размер контекстного окна
    bool is_free = false;              // Бесплатная ли модель
    
    // Цены (в долларах за 1M токенов)
    double prompt_price_usd_per_million = 0.0;
    double completion_price_usd_per_million = 0.0;
    
    // Дополнительные параметры
    std::string topology;              // Тип архитектуры (например, "transformer")
    std::vector<std::string> modality; // Модальности (например, ["text", "image"])
    
    // Для сортировки и фильтрации
    bool operator<(const OpenRouterModel& other) const {
        return name < other.name;
    }
};

/**
 * @brief Ответ API OpenRouter со списком моделей
 */
struct OpenRouterModelsResponse {
    std::vector<OpenRouterModel> models;
    std::string error;
    bool success = false;
};

/**
 * @brief Детальная информация о модели
 */
struct OpenRouterModelDetails {
    std::string id;
    std::string name;
    std::string description;
    std::string provider_name;
    int64_t context_length = 0;
    
    // Цены
    double prompt_price_usd_per_million = 0.0;
    double completion_price_usd_per_million = 0.0;
    
    // Параметры
    std::string architecture;
    std::string instruct_type;
    std::vector<std::string> modality;
    std::vector<std::string> tools;
    
    // Токенизация
    std::string tokenizer;
    
    // Статус
    bool is_free = false;
    std::string status;  // "live", "beta", "deprecated"
    
    std::string error;
    bool success = false;
};

/**
 * @brief Параметры для запроса к OpenRouter API
 */
struct OpenRouterRequestParams {
    std::string model;
    std::string prompt;
    int max_tokens = 1024;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;  // Не используется в OpenRouter API
    bool stream = false;

    // Дополнительные параметры (поддерживаются OpenAI-совместимыми API)
    float presence_penalty = 0.0f;
    float frequency_penalty = 0.0f;

    // Системный промпт
    std::string system_prompt;
    
    // История диалога (для chat completion)
    struct Message {
        std::string role;  // "system", "user", "assistant"
        std::string content;
    };
    std::vector<Message> messages;
};

/**
 * @brief Ответ от OpenRouter на запрос генерации
 */
struct OpenRouterCompletionResponse {
    std::string id;
    std::string model;
    std::string content;
    std::string finish_reason;
    
    // Статистика использования
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    
    // Стоимость (если доступна)
    double cost_usd = 0.0;
    
    std::string error;
    bool success = false;
};

/**
 * @brief Статус операции
 */
enum class OpenRouterStatus {
    Success,
    NetworkError,
    ApiError,
    AuthError,
    RateLimitError,
    InvalidRequest,
    Timeout
};

/**
 * @brief Информация о лимитах OpenRouter
 */
struct OpenRouterRateLimit {
    int total_requests = 0;        // Всего запросов за период
    int remaining_requests = 0;    // Осталось запросов
    int limit = 50;                // Дневной лимит
    std::string reset_time;        // Время сброса
    bool is_free_tier = true;      // Бесплатный тариф
    
    std::string get_status_text() const {
        if (remaining_requests <= 0) {
            return "Лимит исчерпан";
        } else if (remaining_requests <= 10) {
            return "Мало запросов";
        } else {
            return "Нормально";
        }
    }
};

/**
 * @brief Результат операции с OpenRouter
 */
struct OpenRouterResult {
    OpenRouterStatus status;
    std::string message;
    
    static OpenRouterResult success(const std::string& msg = "OK") {
        return {OpenRouterStatus::Success, msg};
    }
    
    static OpenRouterResult error(OpenRouterStatus s, const std::string& msg) {
        return {s, msg};
    }
};

} // namespace core
} // namespace llama_gui

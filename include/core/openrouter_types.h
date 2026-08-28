#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <algorithm>
#include <cctype>

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

    // Режим размышлений/thinking для поддерживающих моделей (GLM, DeepSeek, o-серия)
    bool reasoning_enabled = false;
    int reasoning_budget = 0;  // Бюджет токенов на reasoning (0 = по умолчанию провайдера)

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

/**
 * @brief Пресеты цен (USD за 1 000 000 токенов) для популярных моделей.
 *
 * Используются при включённом "auto_price", если модель совпадает по имени.
 * Цены ориентировочные (международные тарифы DashScope/DeepSeek) и могут
 * меняться — их всегда можно переопределить вручную в настройках провайдера.
 * Для бесплатных моделей цена = 0 (расход $0.00).
 *
 * @return true, если для model_id найден пресет цены.
 */
inline bool get_preset_price(const std::string& model_id,
                             double& out_prompt_per_1m,
                             double& out_completion_per_1m) {
    struct Preset { std::string key; double in; double out; };
    static const std::vector<Preset> presets = {
        // --- Qwen (DashScope / Alibaba) ---
        {"qwen-max",           1.60, 4.00},
        {"qwen-plus",          0.40, 1.20},
        {"qwen-turbo",         0.05, 0.15},
        {"qwen3-max",          1.60, 4.00},
        {"qwen3-plus",         0.40, 1.20},
        {"qwen3-turbo",        0.05, 0.15},
        {"qwen3-235b",         0.40, 1.20},
        {"qwen3-32b",          0.10, 0.30},
        {"qwen3-30b",          0.10, 0.30},
        {"qwen3-14b",          0.07, 0.21},
        {"qwen3-8b",           0.03, 0.09},
        {"qwen2.5-72b",        0.40, 1.20},
        {"qwen2.5-32b",        0.20, 0.60},
        {"qwen2.5-14b",        0.10, 0.30},
        {"qwen2.5-7b",         0.05, 0.15},
        {"qwen-long",          0.20, 0.60},
        {"qwen-vl",            0.40, 1.20},
        // --- DeepSeek (через DashScope / напрямую) ---
        {"deepseek-v3",        0.27, 1.10},
        {"deepseek-chat",      0.27, 1.10},
        {"deepseek-r1",        0.55, 2.19},
        {"deepseek-reasoner",  0.55, 2.19},
        {"deepseek-v4",        0.27, 1.10},
        {"deepseek-v4-flash",  0.00, 0.00},   // часто бесплатно в рамках free tier
        {"deepseek-lite",      0.07, 0.28},
    };
    std::string m = model_id;
    std::transform(m.begin(), m.end(), m.begin(), ::tolower);
    for (const auto& p : presets) {
        std::string key = p.key;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (m.find(key) != std::string::npos) {
            out_prompt_per_1m = p.in;
            out_completion_per_1m = p.out;
            return true;
        }
    }
    return false;
}

} // namespace core
} // namespace llama_gui

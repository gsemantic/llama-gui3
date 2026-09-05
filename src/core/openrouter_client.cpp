#include "../../include/core/openrouter_client.h"
#include "../../include/core/logger.h"
#include <cmath>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace llama_gui::core;

namespace llama_gui {
namespace core {

static bool is_valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        int seq_len = 0;
        if (c <= 0x7F) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) seq_len = 2;
        else if ((c & 0xF0) == 0xE0) seq_len = 3;
        else if ((c & 0xF8) == 0xF0) seq_len = 4;
        else return false;
        if (i + seq_len > s.size()) return false;
        for (int j = 1; j < seq_len; j++)
            if ((s[i + j] & 0xC0) != 0x80) return false;
        i += seq_len;
    }
    return true;
}

static const wchar_t kCp1251ToUtf8[128] = {
    0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,
    0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
    0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
    0x0000,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
    0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,
    0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
    0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,
    0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,
    0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,
    0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
    0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,
    0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
    0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,
    0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
    0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,
    0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F,
};

static std::string cp1251_to_utf8(const std::string& s) {
    std::string result;
    result.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if (c < 0x80) {
            result += (char)c;
        } else {
            wchar_t wc = kCp1251ToUtf8[c - 0x80];
            if (wc == 0) { result += '?'; continue; }
            if (wc < 0x80) { result += (char)wc; }
            else if (wc < 0x800) {
                result += (char)(0xC0 | (wc >> 6));
                result += (char)(0x80 | (wc & 0x3F));
            } else {
                result += (char)(0xE0 | (wc >> 12));
                result += (char)(0x80 | ((wc >> 6) & 0x3F));
                result += (char)(0x80 | (wc & 0x3F));
            }
        }
    }
    return result;
}

static std::string sanitize_utf8(const std::string& s) {
    if (is_valid_utf8(s)) return s;
    return cp1251_to_utf8(s);
}

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

void OpenRouterClient::abort_stream() {
    aborted_.store(true);
    http_client_.abort_stream();
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

namespace {

double round_sampling_param(double v) {
    return std::round(v * 100.0) / 100.0;
}

std::string build_completion_body(const OpenRouterRequestParams& params) {
    nlohmann::json request_body;

    request_body["model"] = params.model;
    request_body["messages"] = nlohmann::json::array();

    if (!params.messages.empty()) {
        for (const auto& msg : params.messages) {
            nlohmann::json message;
            message["role"] = msg.role;
            message["content"] = sanitize_utf8(msg.content);
            request_body["messages"].push_back(message);
        }
    } else if (!params.prompt.empty()) {
        if (!params.system_prompt.empty()) {
            nlohmann::json system_msg;
            system_msg["role"] = "system";
            system_msg["content"] = sanitize_utf8(params.system_prompt);
            request_body["messages"].push_back(system_msg);
        }
        nlohmann::json user_msg;
        user_msg["role"] = "user";
        user_msg["content"] = sanitize_utf8(params.prompt);
        request_body["messages"].push_back(user_msg);
    }

    if (params.max_tokens > 0) {
        request_body["max_tokens"] = params.max_tokens;
    }
    if (params.temperature != 0.7f) {
        request_body["temperature"] = round_sampling_param(params.temperature);
    }
    if (params.top_p != 0.9f) {
        request_body["top_p"] = round_sampling_param(params.top_p);
    }
    if (params.presence_penalty != 0.0f) {
        request_body["presence_penalty"] = round_sampling_param(params.presence_penalty);
    }
    if (params.frequency_penalty != 0.0f) {
        request_body["frequency_penalty"] = round_sampling_param(params.frequency_penalty);
    }

    // Режим размышлений/thinking.
    // - reasoning_enabled = true: включаем thinking для поддерживающих моделей
    //   (GLM, DeepSeek, o-серия OpenAI и т.п.), опционально с бюджетом токенов.
    // - reasoning_enabled = false (по умолчанию): thinking выключен. Это нужно
    //   моделям вроде GLM-4.7, которые включают его автоматически и тратят лимит
    //   токенов на reasoning_content, из-за чего поле content приходит пустым
    //   (finish_reason:"length"). Для них же гарантируем достаточный бюджет
    //   выходных токенов, чтобы ответ поместился целиком.
    if (params.reasoning_enabled) {
        nlohmann::json thinking = nlohmann::json::object({{"type", "enabled"}});
        if (params.reasoning_budget > 0) {
            thinking["budget_tokens"] = params.reasoning_budget;
        }
        request_body["thinking"] = thinking;
    } else if (params.model.find("glm") != std::string::npos ||
               params.model.find("hy3") != std::string::npos) {
        // GLM-4.7 и OpenCode hy3-free по умолчанию включают thinking и тратят
        // лимит на reasoning_content, оставляя content пустым (finish_reason:
        // "length"). Явно отключаем размышления и гарантируем достаточный
        // бюджет выходных токенов, чтобы ответ поместился целиком в content.
        request_body["thinking"] = nlohmann::json::object({{"type", "disabled"}});
        if (params.max_tokens > 0 && params.max_tokens < 8192) {
            request_body["max_tokens"] = 8192;
        }
    }

    request_body["stream"] = params.stream;

    return request_body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace

OpenRouterCompletionResponse OpenRouterClient::complete(const OpenRouterRequestParams& params) {
    std::string body = build_completion_body(params);
    std::string response_str = http_client_.make_request("chat/completions", body);
    OpenRouterCompletionResponse resp = model_parser_.parse_completion_response(response_str);
    if (http_client_.last_http_code() == 429) {
        resp.success = false;
        resp.content.clear();
        resp.error = "HTTP 429: лимит запросов провайдера исчерпан — подождите около минуты "
                     "и повторите (анонимный лимит учитывает и max_tokens)";
    }
    return resp;
}

bool OpenRouterClient::complete_streaming_async(const OpenRouterRequestParams& params, StreamCallback callback,
                                                OpenRouterCompletionResponse* out_response) {
    if (!callback) return false;

    OpenRouterCompletionResponse acc_usage;  // Аккумулятор usage (токены) за всю генерацию

    // Метод выполняется синхронно: поток должен предоставить вызывающий код.
    // (Запуск отдельного detached-потока здесь привёл бы к использованию после
    // уничтожения объекта клиента, т.к. поток захватывал бы 'this'.)
    OpenRouterRequestParams stream_params = params;
    stream_params.stream = true;
    std::string body = build_completion_body(stream_params);

    // Стратегия ретраев:
    //  - до 3 попыток суммарно;
    //  - каждая попытка с бо́льшим таймаутом соединения (fail fast, затем терпеливее);
    //  - экспоненциальный backoff с джиттером между попытками (1s -> 2s);
    //  - ретраим только транзиентные ошибки (таймаут/сеть, 408/429/5xx, пустой ответ);
    //  - не ретраим, если контент уже начал приходить (задублируется).
    const int kMaxAttempts = 3;
    const int kConnectTimeoutsMs[kMaxAttempts] = {10000, 20000, 30000};
    const int kBaseBackoffMs = 1000;

    bool any_content_delivered = false;
    bool gave_up = false;
    std::string last_error_message;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        // Прерывание по требованию пользователя — выходим без повторов и фолбэков
        if (aborted_.load()) {
            return false;
        }

        bool delivered_content = false;
        bool failed = false;
        bool retryable = false;
        std::string error_message;

        http_client_.make_streaming_request("chat/completions", body,
            [this, &callback, &delivered_content, &any_content_delivered, &failed, &retryable, &error_message, &acc_usage](
                const std::string& data, bool is_error, bool is_done, bool is_retryable) {
                if (is_done) {
                    if (is_error) {
                        failed = true;
                        retryable = is_retryable;
                        if (!data.empty()) {
                            OpenRouterCompletionResponse parsed = model_parser_.parse_completion_response(data);
                            error_message = parsed.error.empty() ? data : parsed.error;
                        } else {
                            error_message = "Network error or timeout";
                        }
                    } else if (!delivered_content) {
                        // Сервер ответил, но контента нет - считаем временным сбоем
                        failed = true;
                        retryable = true;
                        error_message = "Empty response from server";
                    } else {
                        callback("", true);
                    }
                    return;
                }

                if (is_error) {
                    return;  // Для чанков данных не ожидается
                }

                // Извлекаем контент из OpenAI-совместимого SSE-чанка
                try {
                    nlohmann::json json_chunk = nlohmann::json::parse(data);
                    if (json_chunk.contains("choices") && !json_chunk["choices"].empty()) {
                        const auto& choice = json_chunk["choices"][0];
                        if (choice.contains("delta") && choice["delta"].contains("content") &&
                            choice["delta"]["content"].is_string()) {
                            std::string token = choice["delta"]["content"].get<std::string>();
                            if (!token.empty()) {
                                delivered_content = true;
                                any_content_delivered = true;
                                callback(token, false);
                            }
                        }
                    }
                    // Аккумулируем usage (обычно приходит в последнем чанке)
                    if (json_chunk.contains("usage")) {
                        const auto& u = json_chunk["usage"];
                        if (u.contains("prompt_tokens"))     acc_usage.prompt_tokens     = u.value("prompt_tokens", 0);
                        if (u.contains("completion_tokens")) acc_usage.completion_tokens = u.value("completion_tokens", 0);
                        if (u.contains("total_tokens"))      acc_usage.total_tokens      = u.value("total_tokens", 0);
                    }
                } catch (const std::exception&) {
                    // Пропускаем повреждённый чанк
                }
            },
            kConnectTimeoutsMs[attempt]);

        if (!failed) {
            if (out_response) *out_response = acc_usage;
            return true;
        }

        // Ошибка не транзиентная или попытки закончились - выходим из цикла,
        // возможно попробуем не-streaming запрос (см. ниже)
        if (!retryable || attempt == kMaxAttempts - 1) {
            gave_up = true;
            last_error_message = error_message;
            break;
        }

        // Экспоненциальный backoff с джиттером перед следующей попыткой
        int backoff_ms = kBaseBackoffMs * (1 << attempt) + (std::rand() % 500);
        std::cout << "[CloudClient] Попытка " << (attempt + 2) << "/" << kMaxAttempts
                  << " через " << backoff_ms << " мс" << std::endl;
        // Прерываемое ожидание: выходим по требованию пользователя ("Стоп")
        for (int waited = 0; waited < backoff_ms && !aborted_.load(); waited += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (aborted_.load()) {
            return false;
        }
    }

    // Фолбэк: некоторые провайдеры (например, OVH AI Endpoints для тяжёлых
    // моделей) отклоняют анонимные streaming-запросы сразу с 429, хотя обычный
    // запрос проходит. Если стриминг так и не дал контента — пробуем один раз
    // без stream и отдаём ответ одним куском.
    if (!any_content_delivered && !aborted_.load()) {
        std::cout << "[CloudClient] Streaming failed - retrying as non-streaming request" << std::endl;
        OpenRouterRequestParams fb = params;
        fb.stream = false;
        OpenRouterCompletionResponse fallback = complete(fb);
        if (fallback.success && !fallback.content.empty()) {
            acc_usage.prompt_tokens = fallback.prompt_tokens;
            acc_usage.completion_tokens = fallback.completion_tokens;
            acc_usage.total_tokens = fallback.total_tokens;
            callback(fallback.content, false);
            callback("", true);
            if (out_response) *out_response = acc_usage;
            return true;
        }
        if (http_client_.last_http_code() == 429) {
            // Не-streaming тоже отклонён по лимиту — понятное сообщение вместо
            // сырого тела ответа
            callback(fallback.error.empty()
                         ? "HTTP 429: лимит запросов провайдера — подождите около минуты"
                         : fallback.error,
                     true);
            return false;
        }
    }

    if (gave_up) {
        if (http_client_.last_http_code() == 429) {
            last_error_message =
                "HTTP 429: лимит запросов провайдера — подождите около минуты";
        }
        callback(last_error_message, true);
    }
    return false;
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

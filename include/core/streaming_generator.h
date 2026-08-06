#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace llama_gui {
namespace core {

/**
 * @brief Callback для streaming ответа
 * @param chunk Текущий чанк текста
 * @param is_final true если это последний чанк
 */
using StreamCallback = std::function<void(const std::string& chunk, bool is_final)>;

/**
 * @brief Streaming генератор ответов
 * 
 * Позволяет получать ответ от LLM по мере генерации,
 * а не ждать полного ответа. Улучшает UX на слабых системах.
 */
class StreamingGenerator {
public:
    StreamingGenerator();
    ~StreamingGenerator();

    /**
     * @brief Установить URL сервера для генерации
     */
    void set_server_url(const std::string& url) { server_url_ = url; }

    /**
     * @brief Сгенерировать ответ с streaming
     * @param prompt Промпт для генерации
     * @param callback Callback для получения чанков
     * @param max_tokens Максимальное количество токенов
     * @param temperature Температура генерации
     * @return Полный ответ (собранный из чанков)
     */
    std::string generate_streaming(const std::string& prompt,
                                   StreamCallback callback,
                                   int max_tokens = 500,
                                   float temperature = 0.3f);

    /**
     * @brief Проверить доступность сервера
     */
    bool is_server_available() const;

    /**
     * @brief Получить статистику генерации
     */
    struct Stats {
        int total_tokens = 0;
        double generation_time_ms = 0;
        double tokens_per_second = 0;
    };
    Stats get_last_stats() const { return last_stats_; }

private:
    std::string server_url_;
    Stats last_stats_;
    std::atomic<bool> generating_{false};

    std::string generate_sync(const std::string& prompt, int max_tokens, float temperature);
};

} // namespace core
} // namespace llama_gui

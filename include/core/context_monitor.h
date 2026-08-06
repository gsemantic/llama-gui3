#pragma once

#include <string>
#include <vector>
#include <functional>
#include <ctime>

namespace llama_gui {
namespace core {

/**
 * @brief Уровни использования контекста
 */
enum class ContextUsageLevel {
    Normal,     // < 70% — всё хорошо
    Warning,    // 70-85% — приближаемся к лимиту
    Critical,   // 85-95% — нужно сжимать
    Overflow    // > 95% — критически мало места
};

/**
 * @brief Статистика использования контекста
 */
struct ContextStats {
    int total_tokens = 0;       // Общее количество токенов в диалоге
    int max_tokens = 4096;      // Максимальный размер контекста (из настроек модели)
    int used_tokens = 0;        // Использованные токены (история + системный промпт)
    int available_tokens = 0;   // Доступные токены для генерации
    ContextUsageLevel level = ContextUsageLevel::Normal;
    
    double usage_percent() const {
        if (max_tokens <= 0) return 0.0;
        return 100.0 * used_tokens / max_tokens;
    }
};

/**
 * @brief Монитор использования контекста
 * 
 * Отслеживает размер контекста в диалоге и генерирует сигналы
 * при приближении к лимиту. Используется для принятия решения
 * о суммаризации старых сообщений.
 */
class ContextMonitor {
public:
    using ThresholdCallback = std::function<void(ContextUsageLevel, const ContextStats&)>;

    ContextMonitor();
    ~ContextMonitor();

    /**
     * @brief Обновить размер контекста
     * @param messages Сообщения диалога
     * @param system_prompt Системный промпт (если есть)
     * @param rag_context RAG-контекст (если есть)
     * @param max_context_size Максимальный размер контекста модели
     */
    void update(const std::vector<std::string>& messages,
                const std::string& system_prompt = "",
                const std::string& rag_context = "",
                int max_context_size = 4096);

    /**
     * @brief Получить текущую статистику
     */
    ContextStats get_stats() const { return stats_; }

    /**
     * @brief Проверить, нужно ли сжимать контекст
     */
    bool needs_compression() const { 
        return stats_.level >= ContextUsageLevel::Critical; 
    }

    /**
     * @brief Проверить, в предупреждении ли мы
     */
    bool is_warning() const { 
        return stats_.level >= ContextUsageLevel::Warning; 
    }

    /**
     * @brief Установить callback для уведомления о порогах
     */
    void set_threshold_callback(ThresholdCallback callback) {
        threshold_callback_ = callback;
    }

    /**
     * @brief Получить количество сообщений, которые нужно суммаризировать
     * @param keep_recent Сколько последних сообщений оставлять без суммаризации
     * @return Количество сообщений для суммаризации (0 если не нужно)
     */
    int get_messages_to_summarize(int keep_recent = 4) const;

    /**
     * @brief Приблизительная оценка токенов в строке
     * @param text Текст для подсчёта
     * @return Приблизительное количество токенов
     */
    static int estimate_tokens(const std::string& text);

private:
    ContextStats stats_;
    ThresholdCallback threshold_callback_;
    ContextUsageLevel last_reported_level_ = ContextUsageLevel::Normal;

    void check_thresholds();
};

} // namespace core
} // namespace llama_gui

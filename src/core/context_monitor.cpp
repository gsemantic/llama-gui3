#include "context_monitor.h"
#include <algorithm>
#include <cmath>

namespace llama_gui {
namespace core {

ContextMonitor::ContextMonitor() = default;
ContextMonitor::~ContextMonitor() = default;

void ContextMonitor::update(const std::vector<std::string>& messages,
                            const std::string& system_prompt,
                            const std::string& rag_context,
                            int max_context_size) {
    stats_.max_tokens = max_context_size;
    stats_.used_tokens = 0;

    // Подсчитываем токены в системном промпте
    if (!system_prompt.empty()) {
        stats_.used_tokens += estimate_tokens(system_prompt);
    }

    // Подсчитываем токены в RAG-контексте
    if (!rag_context.empty()) {
        stats_.used_tokens += estimate_tokens(rag_context);
    }

    // Подсчитываем токены во всех сообщениях
    for (const auto& msg : messages) {
        stats_.used_tokens += estimate_tokens(msg);
    }

    // Добавляем overhead на формат сообщений (роли, разделители)
    // Примерно 4 токена на сообщение (role + formatting)
    stats_.used_tokens += static_cast<int>(messages.size()) * 4;

    stats_.available_tokens = std::max(0, stats_.max_tokens - stats_.used_tokens);
    stats_.total_tokens = stats_.max_tokens;

    // Определяем уровень использования
    double usage = stats_.usage_percent();
    ContextUsageLevel new_level;
    
    if (usage >= 95.0) {
        new_level = ContextUsageLevel::Overflow;
    } else if (usage >= 85.0) {
        new_level = ContextUsageLevel::Critical;
    } else if (usage >= 70.0) {
        new_level = ContextUsageLevel::Warning;
    } else {
        new_level = ContextUsageLevel::Normal;
    }

    stats_.level = new_level;

    // Проверяем пороги и вызываем callback если уровень изменился
    if (new_level != last_reported_level_) {
        check_thresholds();
        last_reported_level_ = new_level;
    }
}

int ContextMonitor::get_messages_to_summarize(int keep_recent) const {
    if (!needs_compression()) {
        return 0;
    }

    // При критическом уровне — суммаризуем больше сообщений
    // При overflow — суммаризуем максимальное количество
    int total_messages = stats_.used_tokens / 50; // приблизительно
    
    if (stats_.level == ContextUsageLevel::Overflow) {
        // Суммаризуем всё кроме последних 2 сообщений
        return std::max(0, total_messages - 2);
    } else if (stats_.level == ContextUsageLevel::Critical) {
        // Суммаризуем старые сообщения, оставляя recent
        return std::max(0, total_messages - keep_recent);
    }
    
    return 0;
}

int ContextMonitor::estimate_tokens(const std::string& text) {
    if (text.empty()) return 0;

    // Приблизительная оценка: 1 токен ≈ 4 символа для английского
    // Для русского текста: 1 токен ≈ 2-3 символа
    // Используем смешанную эвристику
    
    int ascii_chars = 0;
    int non_ascii_chars = 0;
    
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 128) {
            ascii_chars++;
        } else if (c >= 0xC0) {
            non_ascii_chars++;
        }
        // Пропускаем continuation bytes (10xxxxxx)
    }
    
    // ASCII: ~4 символа на токен, не-ASCII: ~2 символа на токен
    int tokens = (ascii_chars / 4) + (non_ascii_chars / 2);
    
    // Минимум 1 токен для непустой строки
    return std::max(1, tokens);
}

void ContextMonitor::check_thresholds() {
    if (threshold_callback_) {
        threshold_callback_(stats_.level, stats_);
    }
}

} // namespace core
} // namespace llama_gui

#pragma once

#include <string>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки вывода (Output) для llama.cpp
 * 
 * Включает все параметры вывода токенов:
 * - Prediction (предсказание)
 * - Special tokens (специальные токены)
 * - Verbose (подробный вывод)
 * - TTS (синтез речи)
 * - Pooling (пулинг для эмбеддингов)
 */
struct OutputSettings {
    // =========================================================================
    // Prediction (предсказание)
    // =========================================================================
    
    /// Количество токенов для предсказания (-n, --n-predict)
    /// -1 = неограниченно (до EOS или context limit)
    int n_predict = -1;
    
    /// Количество токенов для сохранения (--keep)
    int keep = 0;

    // =========================================================================
    // Special tokens (специальные токены)
    // =========================================================================
    
    /// Выводить специальные токены (-sp, --special)
    bool special_tokens = false;
    
    /// SPM infill (--spm-infill)
    bool spm_infill = false;

    // =========================================================================
    // Verbose (подробный вывод)
    // =========================================================================
    
    /// Подробный вывод промпта (--verbose-prompt)
    bool verbose_prompt = false;
    
    /// Экранировать последовательности (-e, --escape)
    bool escape_sequences = true;

    // =========================================================================
    // TTS (синтез речи)
    // =========================================================================
    
    /// Использовать направляющие токены для TTS (--tts-use-guide-tokens)
    bool tts_use_guide_tokens = false;

    // =========================================================================
    // Pooling (пулинг для эмбеддингов)
    // =========================================================================
    
    /// Тип пулинга (--pooling)
    /// Варианты: "model", "none", "mean", "cls", "last", "rank"
    std::string pooling_type = "model";

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        if (n_predict < -1) return false;
        if (keep < 0) return false;
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;
        
        if (n_predict < -1) {
            errors += "N_predict must be -1 or greater. ";
        }
        if (keep < 0) {
            errors += "Keep must be non-negative. ";
        }
        
        return errors;
    }

    // =========================================================================
    // Helper methods (вспомогательные методы)
    // =========================================================================
    
    /**
     * @brief Проверка неограниченного вывода
     * @return true если количество токенов не ограничено
     */
    bool is_unlimited_prediction() const {
        return n_predict == -1;
    }

    /**
     * @brief Проверка использования пулинга
     * @return true если используется пулинг отличный от "none"
     */
    bool uses_pooling() const {
        return pooling_type != "none";
    }

    /**
     * @brief Получить эффективное количество токенов для предсказания
     * @param default_value Значение по умолчанию если -1
     * @return Количество токенов
     */
    int get_effective_n_predict(int default_value = 2048) const {
        return n_predict == -1 ? default_value : n_predict;
    }

    /**
     * @brief Проверка использования SPM infill
     * @return true если используется SPM infill
     */
    bool uses_spm_infill() const {
        return spm_infill;
    }

    /**
     * @brief Проверка использования TTS
     * @return true если используются TTS направляющие токены
     */
    bool uses_tts() const {
        return tts_use_guide_tokens;
    }
};

} // namespace core
} // namespace llama_gui

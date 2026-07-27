#pragma once

#include <string>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки RoPE (Rotary Positional Embeddings) для llama.cpp
 * 
 * Включает все параметры позиционных эмбеддингов:
 * - RoPE scaling (масштабирование)
 * - RoPE parameters (частотные параметры)
 * - YaRN parameters (параметры YaRN)
 */
struct RoPESettings {
    // =========================================================================
    // RoPE scaling (масштабирование)
    // =========================================================================
    
    /// Режим масштабирования RoPE
    enum class RopeScaling {
        None,   /// Без масштабирования
        Linear, /// Линейное масштабирование (по умолчанию)
        Yarn    /// YaRN (Yet another RoPE)
    };
    
    /// Режим масштабирования (--rope-scaling)
    RopeScaling rope_scaling = RopeScaling::Linear;

    // =========================================================================
    // RoPE parameters (параметры RoPE)
    // =========================================================================
    
    /// Масштаб RoPE (--rope-scale)
    float rope_scale = 1.0f;
    
    /// Базовая частота RoPE (--rope-freq-base), 0 = из модели
    float rope_freq_base = 0.0f;
    
    /// Шкала частоты RoPE (--rope-freq-scale)
    float rope_freq_scale = 1.0f;

    // =========================================================================
    // YaRN parameters (параметры YaRN)
    // =========================================================================
    
    /// Оригинальный размер контекста (--yarn-orig-ctx), 0 = из модели
    int yarn_orig_ctx = 0;
    
    /// Фактор расширения (--yarn-ext-factor), -1.0 = автоматический
    float yarn_ext_factor = -1.0f;
    
    /// Фактор внимания (--yarn-attn-factor)
    float yarn_attn_factor = 1.0f;
    
    /// Бета для медленных частот (--yarn-beta-slow)
    float yarn_beta_slow = 1.0f;
    
    /// Бета для быстрых частот (--yarn-beta-fast)
    float yarn_beta_fast = 32.0f;

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        if (rope_scale <= 0.0f) return false;
        if (rope_freq_base < 0.0f) return false;
        if (rope_freq_scale <= 0.0f) return false;
        if (yarn_orig_ctx < 0) return false;
        if (yarn_beta_slow < 0.0f) return false;
        if (yarn_beta_fast < 0.0f) return false;
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;
        
        if (rope_scale <= 0.0f) {
            errors += "RoPE scale must be positive. ";
        }
        if (rope_freq_base < 0.0f) {
            errors += "RoPE freq base must be non-negative. ";
        }
        if (rope_freq_scale <= 0.0f) {
            errors += "RoPE freq scale must be positive. ";
        }
        if (yarn_orig_ctx < 0) {
            errors += "YaRN original context must be non-negative. ";
        }
        if (yarn_beta_slow < 0.0f) {
            errors += "YaRN beta slow must be non-negative. ";
        }
        if (yarn_beta_fast < 0.0f) {
            errors += "YaRN beta fast must be non-negative. ";
        }
        
        return errors;
    }

    /**
     * @brief Получить строковое представление режима масштабирования
     * @return "none", "linear" или "yarn"
     */
    std::string get_scaling_string() const {
        switch (rope_scaling) {
            case RopeScaling::None:   return "none";
            case RopeScaling::Linear: return "linear";
            case RopeScaling::Yarn:   return "yarn";
            default:                  return "linear";
        }
    }

    /**
     * @brief Установить режим масштабирования из строки
     * @param scaling Строка "none", "linear" или "yarn"
     */
    void set_scaling(const std::string& scaling) {
        if (scaling == "none") {
            rope_scaling = RopeScaling::None;
        } else if (scaling == "yarn") {
            rope_scaling = RopeScaling::Yarn;
        } else {
            rope_scaling = RopeScaling::Linear;
        }
    }

    /**
     * @brief Проверка использования YaRN
     * @return true если используется YaRN
     */
    bool is_yarn_enabled() const {
        return rope_scaling == RopeScaling::Yarn;
    }

    /**
     * @brief Проверка использования линейного масштабирования
     * @return true если используется линейное масштабирование
     */
    bool is_linear_scaling() const {
        return rope_scaling == RopeScaling::Linear;
    }
};

} // namespace core
} // namespace llama_gui

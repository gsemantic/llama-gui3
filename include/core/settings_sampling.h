#pragma once

#include <string>
#include <vector>
#include <map>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки сэмплирования для llama.cpp
 * 
 * Включает все параметры управления сэмплированием:
 * - Basic sampling (temperature, top-k, top-p, min-p)
 * - Advanced sampling (typical, TFS, XTC)
 * - DRY sampling (anti-repetition)
 * - Dynamic temperature
 * - Penalties (repeat, presence, frequency)
 * - Mirostat (adaptive entropy control)
 * - Logit bias
 * - Sampler order
 */
struct SamplingSettings {
    // =========================================================================
    // Basic sampling parameters
    // =========================================================================
    
    /// Температура сэмплирования (--temp)
    float temperature = 0.8f;
    
    /// Top-K сэмплирование (--top-k)
    int top_k = 40;
    
    /// Top-P (nucleus) сэмплирование (--top-p)
    float top_p = 0.9f;
    
    /// Min-P сэмплирование (--min-p)
    float min_p = 0.1f;

    // =========================================================================
    // Advanced sampling parameters
    // =========================================================================
    
    /// Typical sampling (--typical) - локальная типичность
    float typical_p = 1.0f;
    
    /// Tail-free sampling (--tfs-z) - отсечение хвоста распределения
    float tfs_z = 1.0f;

    // =========================================================================
    // XTC sampling (exclusion-based)
    // =========================================================================
    
    /// Вероятность применения XTC (--xtc-probability)
    float xtc_probability = 0.0f;
    
    /// Порог XTC (--xtc-threshold) - минимальная вероятность для исключения
    float xtc_threshold = 1.0f;

    // =========================================================================
    // DRY sampling (Don't Repeat Yourself)
    // =========================================================================
    
    /// Множитель DRY (--dry-multiplier)
    float dry_multiplier = 0.0f;
    
    /// Базовое значение DRY (--dry-base)
    float dry_base = 1.75f;
    
    /// Допустимая длина повторения (--dry-allowed-length)
    int dry_allowed_length = 2;
    
    /// Количество токенов для проверки (--dry-penalty-last-n), -1 = context
    int dry_penalty_last_n = -1;
    
    /// Разделители последовательностей (--dry-sequence-breaker)
    std::vector<std::string> dry_sequence_breakers = {"\n", ":", "\"", "*"};

    // =========================================================================
    // Dynamic temperature
    // =========================================================================
    
    /// Диапазон динамической температуры (--dynatemp-range)
    float dynatemp_range = 0.0f;
    
    /// Экспонента динамической температуры (--dynatemp-exp)
    float dynatemp_exp = 1.0f;

    // =========================================================================
    // Penalties (штрафы)
    // =========================================================================
    
    /// Штраф за повторения (--repeat-penalty)
    float repeat_penalty = 1.1f;
    
    /// Штраф за присутствие (--presence-penalty)
    float presence_penalty = 0.0f;
    
    /// Штраф за частоту (--frequency-penalty)
    float frequency_penalty = 0.0f;
    
    /// Количество токенов для проверки повторений (--repeat-last-n)
    int repeat_last_n = 64;
    
    /// Штрафовать за новые строки (--penalize-nl)
    bool penalize_nl = true;
    
    /// Игнорировать EOS токен (--ignore-eos)
    bool ignore_eos = false;

    // =========================================================================
    // Mirostat (адаптивный контроль энтропии)
    // =========================================================================
    
    /// Режим Mirostat (--mirostat): 0=off, 1=v1, 2=v2
    int mirostat_mode = 0;
    
    /// Целевая энтропия Mirostat (--mirostat-tau)
    float mirostat_tau = 5.0f;
    
    /// Скорость обучения Mirostat (--mirostat-eta)
    float mirostat_eta = 0.1f;

    // =========================================================================
    // Logit bias
    // =========================================================================
    
    /// Смещение логитов для конкретных токенов (--logit-bias)
    /// Ключ: token ID, Значение: bias
    std::map<int, float> logit_bias;

    // =========================================================================
    // Sampler order
    // =========================================================================
    
    /// Порядок сэмплеров (--sampling-seq)
    /// e=entropy, d=dry, s=softmax, k=top-k, p=top-p, y=typical, m=min-p, x=xtc
    std::string samplers_order = "edskypmxt";
    
    /// Использовать пользовательский порядок сэмплеров
    bool use_custom_sampler_order = false;

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        if (temperature < 0.0f || temperature > 2.0f) return false;
        if (top_k < 0) return false;
        if (top_p < 0.0f || top_p > 1.0f) return false;
        if (min_p < 0.0f || min_p > 1.0f) return false;
        if (typical_p < 0.0f || typical_p > 1.0f) return false;
        if (tfs_z < 0.0f || tfs_z > 1.0f) return false;
        if (xtc_probability < 0.0f || xtc_probability > 1.0f) return false;
        if (xtc_threshold < 0.0f || xtc_threshold > 1.0f) return false;
        if (dry_multiplier < 0.0f) return false;
        if (dry_base < 1.0f) return false;
        if (dry_allowed_length < 1) return false;
        if (repeat_penalty < 0.0f) return false;
        if (mirostat_mode < 0 || mirostat_mode > 2) return false;
        if (mirostat_tau < 0.0f) return false;
        if (mirostat_eta < 0.0f || mirostat_eta > 1.0f) return false;
        if (dynatemp_range < 0.0f) return false;
        if (dynatemp_exp < 0.0f) return false;
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;
        
        if (temperature < 0.0f || temperature > 2.0f) {
            errors += "Temperature must be between 0 and 2. ";
        }
        if (top_p < 0.0f || top_p > 1.0f) {
            errors += "Top-P must be between 0 and 1. ";
        }
        if (min_p < 0.0f || min_p > 1.0f) {
            errors += "Min-P must be between 0 and 1. ";
        }
        if (mirostat_mode < 0 || mirostat_mode > 2) {
            errors += "Mirostat mode must be 0, 1, or 2. ";
        }
        if (mirostat_eta < 0.0f || mirostat_eta > 1.0f) {
            errors += "Mirostat eta must be between 0 and 1. ";
        }
        
        return errors;
    }
};

} // namespace core
} // namespace llama_gui

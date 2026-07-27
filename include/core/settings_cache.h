#pragma once

#include <string>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки кэша для llama.cpp
 * 
 * Включает все параметры кэширования:
 * - KV cache types (типы кэша ключей и значений)
 * - Draft model cache (кэш для черновой модели)
 * - Cache options (опции кэширования)
 * - Slot management (управление слотами)
 */
struct CacheSettings {
    // =========================================================================
    // KV cache types (типы кэша)
    // =========================================================================
    
    /// Типы кэша
    enum class CacheType {
        F32,    /// 32-bit float (точность FP32)
        F16,    /// 16-bit float (точность FP16, по умолчанию)
        BF16,   /// 16-bit brain float
        Q8_0,   /// 8-bit quantized
        Q4_0,   /// 4-bit quantized (метод 0)
        Q4_1,   /// 4-bit quantized (метод 1)
        IQ4_NL, /// 4-bit quantized (new method)
        Q5_0,   /// 5-bit quantized (метод 0)
        Q5_1    /// 5-bit quantized (метод 1)
    };
    
    /// Тип кэша ключей (-ctk, --cache-type-k)
    CacheType cache_type_k = CacheType::F16;
    
    /// Тип кэша значений (-ctv, --cache-type-v)
    CacheType cache_type_v = CacheType::F16;

    // =========================================================================
    // Draft model cache (кэш черновой модели)
    // =========================================================================
    
    /// Тип кэша ключей draft (-ctkd, --cache-type-k-draft)
    CacheType cache_type_k_draft = CacheType::F16;
    
    /// Тип кэша значений draft (-ctvd, --cache-type-v-draft)
    CacheType cache_type_v_draft = CacheType::F16;

    // =========================================================================
    // Cache options (опции кэширования)
    // =========================================================================
    
    /// Кэшировать промпт (--cache-prompt)
    bool cache_prompt = true;
    
    /// Повторное использование кэша (--cache-reuse)
    int cache_reuse = 0;
    
    /// Полный SWA кэш (--swa-full)
    bool swa_full = false;
    
    /// Не использовать сдвиг контекста (--no-context-shift)
    bool no_context_shift = false;

    // =========================================================================
    // Slot management (управление слотами)
    // =========================================================================
    
    /// Путь для сохранения слотов (--slot-save-path)
    std::string slot_save_path = "";
    
    /// Порог схожести промпта слота (-sps, --slot-prompt-similarity)
    float slot_prompt_similarity = 0.5f;
    
    /// Включить endpoint слотов (--slots / --no-slots)
    bool slots_endpoint_enabled = true;

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        if (cache_reuse < 0) return false;
        if (slot_prompt_similarity < 0.0f || slot_prompt_similarity > 1.0f) return false;
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;
        
        if (cache_reuse < 0) {
            errors += "Cache reuse must be non-negative. ";
        }
        if (slot_prompt_similarity < 0.0f || slot_prompt_similarity > 1.0f) {
            errors += "Slot prompt similarity must be between 0 and 1. ";
        }
        
        return errors;
    }

    /**
     * @brief Получить строковое представление типа кэша
     * @param type Тип кэша
     * @return Строковое представление ("f32", "f16", "bf16", "q8_0", etc.)
     */
    static std::string cache_type_to_string(CacheType type) {
        switch (type) {
            case CacheType::F32:    return "f32";
            case CacheType::F16:    return "f16";
            case CacheType::BF16:   return "bf16";
            case CacheType::Q8_0:   return "q8_0";
            case CacheType::Q4_0:   return "q4_0";
            case CacheType::Q4_1:   return "q4_1";
            case CacheType::IQ4_NL: return "iq4_nl";
            case CacheType::Q5_0:   return "q5_0";
            case CacheType::Q5_1:   return "q5_1";
            default:                return "f16";
        }
    }

    /**
     * @brief Получить тип кэша из строки
     * @param str Строковое представление
     * @return Тип кэша
     */
    static CacheType cache_type_from_string(const std::string& str) {
        if (str == "f32")    return CacheType::F32;
        if (str == "f16")    return CacheType::F16;
        if (str == "bf16")   return CacheType::BF16;
        if (str == "q8_0")   return CacheType::Q8_0;
        if (str == "q4_0")   return CacheType::Q4_0;
        if (str == "q4_1")   return CacheType::Q4_1;
        if (str == "iq4_nl") return CacheType::IQ4_NL;
        if (str == "q5_0")   return CacheType::Q5_0;
        if (str == "q5_1")   return CacheType::Q5_1;
        return CacheType::F16; // по умолчанию
    }

    /**
     * @brief Получить строковое представление типа кэша ключей
     * @return Строковое представление
     */
    std::string get_cache_type_k_string() const {
        return cache_type_to_string(cache_type_k);
    }

    /**
     * @brief Получить строковое представление типа кэша значений
     * @return Строковое представление
     */
    std::string get_cache_type_v_string() const {
        return cache_type_to_string(cache_type_v);
    }
};

} // namespace core
} // namespace llama_gui

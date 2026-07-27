#pragma once

#include <string>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки GPU для llama.cpp
 * 
 * Включает все параметры использования GPU:
 * - GPU layers (количество слоёв на GPU)
 * - Split mode (режим разделения)
 * - Tensor split (разделение тензоров)
 * - Offload options (опции выгрузки)
 * - Flash Attention
 */
struct GPUSettings {
    // =========================================================================
    // GPU layers (количество слоёв)
    // =========================================================================
    
    /// Количество слоёв на GPU (-ngl, --n-gpu-layers)
    int n_gpu_layers = 0;
    
    /// Количество слоёв draft модели на GPU (-ngld, --n-gpu-layers-draft)
    int n_gpu_layers_draft = -1;

    // =========================================================================
    // Split mode (режим разделения между GPU)
    // =========================================================================
    
    /// Режим разделения
    enum class SplitMode {
        None,   /// Не разделять
        Layer,  /// Разделение по слоям (по умолчанию)
        Row     /// Разделение по строкам
    };
    
    /// Режим разделения (-sm, --split-mode)
    SplitMode split_mode = SplitMode::Layer;

    // =========================================================================
    // Tensor split (разделение тензоров между GPU)
    // =========================================================================
    
    /// Разделение тензоров (-ts, --tensor-split)
    /// Формат: "3,2,1" - пропорции для каждого GPU
    std::string tensor_split = "";
    
    /// Основной GPU (-mg, --main-gpu)
    int main_gpu = 0;

    // =========================================================================
    // Offload options (опции выгрузки памяти)
    // =========================================================================
    
    /// Не выгружать операции (--no-op-offload)
    bool no_op_offload = false;
    
    /// Не выгружать KV кэш (-nkvo, --no-kv-offload)
    bool no_kv_offload = false;
    
    /// Не выполнять прогрев (--no-warmup)
    bool no_warmup = false;
    
    /// Блокировать память (--mlock) - по умолчанию false для экономии RAM
    bool mlock = false;
    
    /// Запретить mmap (--no-mmap) - по умолчанию false
    bool no_mmap = false;

    // =========================================================================
    // Flash Attention
    // =========================================================================
    
    /// Режим Flash Attention
    enum class FlashAttention {
        Auto,     /// Автоматически (по умолчанию)
        Enabled,  /// Включено
        Disabled  /// Выключено
    };
    
    /// Flash Attention (-fa, --flash-attn)
    FlashAttention flash_attn = FlashAttention::Auto;

    // =========================================================================
    // Defrag (дефрагментация памяти)
    // =========================================================================
    
    /// Порог дефрагментации (-dt, --defrag-thold)
    float defrag_thold = 0.1f;

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        if (n_gpu_layers < 0) return false;
        if (n_gpu_layers_draft < -1) return false;
        if (main_gpu < 0) return false;
        if (defrag_thold < 0.0f || defrag_thold > 1.0f) return false;
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;
        
        if (n_gpu_layers < 0) {
            errors += "GPU layers must be non-negative. ";
        }
        if (n_gpu_layers_draft < -1) {
            errors += "Draft GPU layers must be -1 or greater. ";
        }
        if (main_gpu < 0) {
            errors += "Main GPU must be non-negative. ";
        }
        if (defrag_thold < 0.0f || defrag_thold > 1.0f) {
            errors += "Defrag threshold must be between 0 and 1. ";
        }
        
        return errors;
    }

    /**
     * @brief Проверка включённости Flash Attention
     * @return true если Flash Attention включён
     */
    bool is_flash_attn_enabled() const {
        return flash_attn == FlashAttention::Enabled;
    }

    /**
     * @brief Проверка автоматического режима Flash Attention
     * @return true если режим автоматический
     */
    bool is_flash_attn_auto() const {
        return flash_attn == FlashAttention::Auto;
    }

    /**
     * @brief Получить строковое представление split mode
     * @return "none", "layer" или "row"
     */
    std::string get_split_mode_string() const {
        switch (split_mode) {
            case SplitMode::None:   return "none";
            case SplitMode::Layer:  return "layer";
            case SplitMode::Row:    return "row";
            default:                return "layer";
        }
    }

    /**
     * @brief Установить split mode из строки
     * @param mode Строка "none", "layer" или "row"
     */
    void set_split_mode(const std::string& mode) {
        if (mode == "none") {
            split_mode = SplitMode::None;
        } else if (mode == "row") {
            split_mode = SplitMode::Row;
        } else {
            split_mode = SplitMode::Layer;
        }
    }
};

} // namespace core
} // namespace llama_gui

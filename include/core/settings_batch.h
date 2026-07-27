#pragma once

#include <string>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки пакетной обработки (Batch) для llama.cpp
 * 
 * Включает все параметры обработки пакетов:
 * - Batch sizes (размеры пакетов)
 * - Context (размер контекста)
 * - Threading (потоки)
 * - CPU affinity (привязка к CPU)
 * - Options (дополнительные опции)
 */
struct BatchSettings {
    // =========================================================================
    // Batch sizes (размеры пакетов)
    // =========================================================================
    
    /// Размер пакета (-b, --batch-size)
    int batch_size = 2048;
    
    /// Размер микро-пакета (-ub, --ubatch-size)
    int ubatch_size = 512;

    // =========================================================================
    // Context (контекст)
    // =========================================================================
    
    /// Размер контекста (-c, --ctx-size)
    int ctx_size = 4096;
    
    /// Размер контекста draft модели (-cd, --ctx-size-draft), 0 = auto
    int ctx_size_draft = 0;

    // =========================================================================
    // Threading (потоки)
    // =========================================================================
    
    /// Количество потоков (-t, --threads), -1 = по умолчанию
    int threads = -1;
    
    /// Количество потоков для батча (-TB, --threads-batch), -1 = как threads
    int threads_batch = -1;

    // =========================================================================
    // CPU affinity (привязка к CPU) - основные
    // =========================================================================
    
    /// Маска CPU (-C, --cpu-mask)
    std::string cpu_mask = "";
    
    /// Диапазон CPU (-Cr, --cpu-range)
    std::string cpu_range = "";
    
    /// Строгая привязка CPU (--cpu-strict)
    bool cpu_strict = false;
    
    /// Приоритет (--prio)
    int priority = 0;
    
    /// Уровень опроса (--poll)
    int poll_level = 50;

    // =========================================================================
    // CPU affinity (привязка к CPU) - для батча
    // =========================================================================
    
    /// Маска CPU для батча (-Cb, --cpu-mask-batch)
    std::string cpu_mask_batch = "";
    
    /// Диапазон CPU для батча (-Crb, --cpu-range-batch)
    std::string cpu_range_batch = "";
    
    /// Строгая привязка CPU для батча (--cpu-strict-batch)
    bool cpu_strict_batch = false;
    
    /// Приоритет для батча (--prio-batch)
    int priority_batch = 0;
    
    /// Уровень опроса для батча (--poll-batch)
    int poll_batch = 50;

    // =========================================================================
    // Options (опции)
    // =========================================================================
    
    /// Непрерывная батчинг (-cb, --cont-batching)
    bool cont_batching = true;
    
    /// Отключить оптимизации производительности (--no-perf)
    bool no_perf = false;

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        if (batch_size < 1) return false;
        if (ubatch_size < 1) return false;
        if (ubatch_size > batch_size) return false;
        if (ctx_size < 1) return false;
        if (ctx_size_draft < 0) return false;
        if (threads < -1) return false;
        if (threads_batch < -1) return false;
        if (priority < -20 || priority > 20) return false;
        if (priority_batch < -20 || priority_batch > 20) return false;
        if (poll_level < 0 || poll_level > 100) return false;
        if (poll_batch < 0 || poll_batch > 100) return false;
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;
        
        if (batch_size < 1) {
            errors += "Batch size must be positive. ";
        }
        if (ubatch_size < 1) {
            errors += "Ubatch size must be positive. ";
        }
        if (ubatch_size > batch_size) {
            errors += "Ubatch size must be <= batch size. ";
        }
        if (ctx_size < 1) {
            errors += "Context size must be positive. ";
        }
        if (ctx_size_draft < 0) {
            errors += "Draft context size must be non-negative. ";
        }
        if (threads < -1) {
            errors += "Threads must be -1 or greater. ";
        }
        if (priority < -20 || priority > 20) {
            errors += "Priority must be between -20 and 20. ";
        }
        if (poll_level < 0 || poll_level > 100) {
            errors += "Poll level must be between 0 and 100. ";
        }
        
        return errors;
    }

    // =========================================================================
    // Helper methods (вспомогательные методы)
    // =========================================================================
    
    /**
     * @brief Проверка использования отдельных потоков для батча
     * @return true если заданы отдельные потоки для батча
     */
    bool uses_separate_batch_threads() const {
        return threads_batch != -1;
    }

    /**
     * @brief Проверка использования CPU affinity для батча
     * @return true если задана привязка к CPU для батча
     */
    bool uses_batch_cpu_affinity() const {
        return !cpu_mask_batch.empty() || !cpu_range_batch.empty();
    }

    /**
     * @brief Получить эффективное количество потоков
     * @return Количество потоков (с учётом -1 = default)
     */
    int get_effective_threads() const {
        return threads == -1 ? 4 : threads; // 4 - разумное значение по умолчанию
    }

    /**
     * @brief Получить эффективное количество потоков для батча
     * @return Количество потоков для батча
     */
    int get_effective_batch_threads() const {
        if (threads_batch != -1) return threads_batch;
        return get_effective_threads();
    }
};

} // namespace core
} // namespace llama_gui

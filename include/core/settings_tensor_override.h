#pragma once

#include <string>
#include <vector>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки переопределения тензоров (Tensor Override) для llama.cpp
 * 
 * Позволяют переопределять тип буфера для конкретных тензоров
 * по шаблону имени.
 * 
 * Включает:
 * - Tensor overrides (список переопределений)
 * - Buffer types (типы буферов: VRAM, RAM)
 */
struct TensorOverrideSettings {
    // =========================================================================
    // Tensor override structure (структура переопределения)
    // =========================================================================
    
    /// Структура переопределения тензора
    struct TensorOverride {
        std::string pattern;      /// Шаблон имени тензора
        std::string buffer_type;  /// Тип буфера ("VRAM", "RAM", etc.)
        
        bool operator==(const TensorOverride& other) const {
            return pattern == other.pattern && buffer_type == other.buffer_type;
        }
    };

    // =========================================================================
    // Tensor overrides list (список переопределений)
    // =========================================================================
    
    /// Список переопределений тензоров (-ot, --override-tensor)
    std::vector<TensorOverride> overrides;

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        for (const auto& override : overrides) {
            if (override.pattern.empty()) return false;
            if (override.buffer_type.empty()) return false;
        }
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;
        
        for (size_t i = 0; i < overrides.size(); ++i) {
            if (overrides[i].pattern.empty()) {
                errors += "Override " + std::to_string(i) + " has no pattern. ";
            }
            if (overrides[i].buffer_type.empty()) {
                errors += "Override " + std::to_string(i) + " has no buffer type. ";
            }
        }
        
        return errors;
    }

    // =========================================================================
    // Helper methods (вспомогательные методы)
    // =========================================================================
    
    /**
     * @brief Добавить переопределение тензора
     * @param pattern Шаблон имени тензора
     * @param buffer_type Тип буфера ("VRAM", "RAM", etc.)
     */
    void add_override(const std::string& pattern, const std::string& buffer_type) {
        overrides.push_back({pattern, buffer_type});
    }

    /**
     * @brief Удалить переопределение по индексу
     * @param index Индекс переопределения
     * @return true если успешно удалено
     */
    bool remove_override(size_t index) {
        if (index >= overrides.size()) return false;
        overrides.erase(overrides.begin() + index);
        return true;
    }

    /**
     * @brief Удалить все переопределения
     */
    void clear_overrides() {
        overrides.clear();
    }

    /**
     * @brief Получить количество переопределений
     * @return Количество переопределений
     */
    size_t get_override_count() const {
        return overrides.size();
    }

    /**
     * @brief Проверка наличия переопределений
     * @return true если есть хотя бы одно переопределение
     */
    bool has_overrides() const {
        return !overrides.empty();
    }

    /**
     * @brief Найти переопределение по шаблону
     * @param pattern Шаблон для поиска
     * @return Указатель на переопределение или nullptr если не найдено
     */
    const TensorOverride* find_override(const std::string& pattern) const {
        for (const auto& override : overrides) {
            if (override.pattern == pattern) {
                return &override;
            }
        }
        return nullptr;
    }
};

} // namespace core
} // namespace llama_gui

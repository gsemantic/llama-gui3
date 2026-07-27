#pragma once

#include <string>
#include <vector>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки векторов управления (Control Vectors) для llama.cpp
 * 
 * Векторы управления позволяют направлять поведение модели
 * путём добавления векторных смещений к активациям.
 * 
 * Включает:
 * - Control vectors (список векторов)
 * - Layer range (диапазон слоёв применения)
 */
struct ControlVectorSettings {
    // =========================================================================
    // Control vector structure (структура вектора управления)
    // =========================================================================
    
    /// Структура вектора управления
    struct ControlVector {
        std::string path;   /// Путь к файлу вектора
        float scale = 1.0f; /// Масштаб применения (--control-vector-scaled)
        
        bool operator==(const ControlVector& other) const {
            return path == other.path && scale == other.scale;
        }
    };

    // =========================================================================
    // Control vectors list (список векторов)
    // =========================================================================
    
    /// Список векторов управления (--control-vector, --control-vector-scaled)
    std::vector<ControlVector> control_vectors;

    // =========================================================================
    // Layer range (диапазон слоёв)
    // =========================================================================
    
    /// Начальный слой применения (--control-vector-layer-range START)
    int control_vector_layer_start = 0;
    
    /// Конечный слой применения (--control-vector-layer-range END), -1 = все
    int control_vector_layer_end = -1;

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        if (control_vector_layer_start < 0) return false;
        if (control_vector_layer_end < -1) return false;
        if (control_vector_layer_end != -1 && 
            control_vector_layer_end < control_vector_layer_start) return false;
        
        for (const auto& cv : control_vectors) {
            if (cv.path.empty()) return false;
            if (cv.scale < 0.0f) return false;
        }
        
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;
        
        if (control_vector_layer_start < 0) {
            errors += "Layer start must be non-negative. ";
        }
        if (control_vector_layer_end < -1) {
            errors += "Layer end must be -1 or greater. ";
        }
        if (control_vector_layer_end != -1 && 
            control_vector_layer_end < control_vector_layer_start) {
            errors += "Layer end must be >= layer start or -1. ";
        }
        
        for (size_t i = 0; i < control_vectors.size(); ++i) {
            if (control_vectors[i].path.empty()) {
                errors += "Control vector " + std::to_string(i) + " has no path. ";
            }
            if (control_vectors[i].scale < 0.0f) {
                errors += "Control vector " + std::to_string(i) + " scale must be non-negative. ";
            }
        }
        
        return errors;
    }

    // =========================================================================
    // Helper methods (вспомогательные методы)
    // =========================================================================
    
    /**
     * @brief Добавить вектор управления
     * @param path Путь к файлу вектора
     * @param scale Масштаб (по умолчанию 1.0)
     */
    void add_control_vector(const std::string& path, float scale = 1.0f) {
        control_vectors.push_back({path, scale});
    }

    /**
     * @brief Удалить вектор управления по индексу
     * @param index Индекс вектора
     * @return true если успешно удален
     */
    bool remove_control_vector(size_t index) {
        if (index >= control_vectors.size()) return false;
        control_vectors.erase(control_vectors.begin() + index);
        return true;
    }

    /**
     * @brief Проверка использования диапазона слоёв
     * @return true если задан конкретный диапазон (не все слои)
     */
    bool uses_layer_range() const {
        return control_vector_layer_end != -1;
    }

    /**
     * @brief Получить количество векторов управления
     * @return Количество векторов
     */
    size_t get_control_vector_count() const {
        return control_vectors.size();
    }

    /**
     * @brief Проверка наличия векторов управления
     * @return true если есть хотя бы один вектор
     */
    bool has_control_vectors() const {
        return !control_vectors.empty();
    }
};

} // namespace core
} // namespace llama_gui

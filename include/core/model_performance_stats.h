#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <map>
#include <chrono>

namespace llama_gui {
namespace core {

/**
 * @brief Статистика производительности для одной модели
 */
struct ModelPerformanceStats {
    std::string model_path;           // Путь к модели
    std::string model_name;           // Имя модели (извлекается из пути)

    // Статистика генерации
    size_t total_generations = 0;     // Количество генераций
    double avg_tokens_per_second = 0.0; // Средняя скорость (токенов/сек)
    double avg_response_time_ms = 0.0;  // Среднее время ответа
    size_t avg_tokens_generated = 0;    // Среднее количество токенов

    // Статистика контекста
    size_t avg_context_used = 0;      // Среднее использование контекста
    size_t total_context = 0;         // Полный размер контекста

    // Временные метки
    std::chrono::system_clock::time_point first_used;  // Первое использование
    std::chrono::system_clock::time_point last_used;   // Последнее использование

    /**
     * @brief Обновить скользящее среднее для токенов/сек
     */
    void update_tokens_per_second(double new_tps) {
        double alpha = 0.1;
        avg_tokens_per_second = alpha * new_tps + (1.0 - alpha) * avg_tokens_per_second;
    }

    /**
     * @brief Обновить скользящее среднее для времени ответа
     */
    void update_response_time(double new_time_ms) {
        double alpha = 0.1;
        avg_response_time_ms = alpha * new_time_ms + (1.0 - alpha) * avg_response_time_ms;
    }

    /**
     * @brief Обновить скользящее среднее для количества токенов
     * @param new_tokens Новое количество токенов
     * @param current_generation Номер текущей генерации (для определения первой инициализации)
     */
    void update_tokens_generated(size_t new_tokens, size_t current_generation) {
        if (current_generation == 1) {
            avg_tokens_generated = new_tokens;
        } else {
            double alpha = 0.1;
            avg_tokens_generated = static_cast<size_t>(
                alpha * new_tokens + (1.0 - alpha) * avg_tokens_generated
            );
        }
    }

    /**
     * @brief Получить ключ для хранения (хэш пути)
     */
    std::string get_key() const {
        return model_path;
    }
};

/**
 * @brief Менеджер статистики производительности моделей
 *
 * Собирает и хранит статистику для каждой используемой модели.
 * Данные сохраняются между сессиями.
 */
class ModelPerformanceManager {
public:
    /**
     * @brief Конструктор менеджера
     */
    ModelPerformanceManager();

    /**
     * @brief Деструктор
     */
    ~ModelPerformanceManager();

    // Запрет копирования (из-за std::mutex)
    ModelPerformanceManager(const ModelPerformanceManager&) = delete;
    ModelPerformanceManager& operator=(const ModelPerformanceManager&) = delete;

    // =========================================================================
    // Запись метрик
    // =========================================================================

    /**
     * @brief Записать результат генерации для модели
     * @param model_path Путь к модели
     * @param tokens_per_second Токенов в секунду
     * @param response_time_ms Время ответа
     * @param tokens_generated Количество сгенерированных токенов
     * @param context_used Использовано контекста
     * @param total_context Полный размер контекста
     */
    void record_generation(
        const std::string& model_path,
        double tokens_per_second,
        double response_time_ms,
        size_t tokens_generated,
        size_t context_used,
        size_t total_context
    );

    // =========================================================================
    // Чтение метрик
    // =========================================================================

    /**
     * @brief Получить статистику для модели
     * @param model_path Путь к модели
     * @return Статистика модели (пустая если не найдена)
     */
    ModelPerformanceStats get_model_stats(const std::string& model_path) const;

    /**
     * @brief Получить статистику всех моделей
     * @return Карта статистики по всем моделям
     */
    std::map<std::string, ModelPerformanceStats> get_all_stats() const;

    /**
     * @brief Получить количество отслеживаемых моделей
     * @return Количество моделей
     */
    size_t get_model_count() const;

    /**
     * @brief Проверить наличие статистики для модели
     * @param model_path Путь к модели
     * @return true если статистика есть
     */
    bool has_model_stats(const std::string& model_path) const;

    // =========================================================================
    // Управление
    // =========================================================================

    /**
     * @brief Сбросить статистику для конкретной модели
     * @param model_path Путь к модели
     */
    void reset_model_stats(const std::string& model_path);

    /**
     * @brief Сбросить всю статистику
     */
    void reset_all_stats();

    // =========================================================================
    // Сериализация
    // =========================================================================

    /**
     * @brief Сериализовать в JSON
     * @return JSON строка
     */
    std::string to_json() const;

    /**
     * @brief Десериализовать из JSON
     * @param json_str JSON строка
     * @return true если успешно
     */
    bool from_json(const std::string& json_str);

    /**
     * @brief Сгенерировать текстовый отчёт
     * @return Форматированный отчёт
     */
    std::string generate_report() const;

private:
    std::map<std::string, ModelPerformanceStats> models_stats_;
    mutable std::mutex mutex_;

    /**
     * @brief Извлечь имя модели из пути
     */
    std::string extract_model_name(const std::string& model_path) const;
};

} // namespace core
} // namespace llama_gui

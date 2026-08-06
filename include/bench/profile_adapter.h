#pragma once

#include "bench_types.h"
#include <string>
#include <nlohmann/json.hpp>

namespace llama_gui {
namespace bench {

/**
 * @class ProfileAdapter
 * @brief Адаптер профилей проекта для llama-bench
 * 
 * Конвертирует профили из формата проекта в параметры llama-bench
 */
class ProfileAdapter {
public:
    /**
     * @brief Конвертировать профиль проекта в параметры бенчмарка
     * @param profile_path Путь к JSON файлу профиля
     * @return Параметры теста или пустые если ошибка
     */
    static BenchTestParams profileToBenchParams(const std::string& profile_path);
    
    /**
     * @brief Конвертировать JSON профиля в параметры бенчмарка
     * @param profile_json JSON объект профиля
     * @return Параметры теста
     */
    static BenchTestParams profileToBenchParams(const nlohmann::json& profile_json);
    
    /**
     * @brief Извлечь путь к модели из профиля
     * @param profile_json JSON объект профиля
     * @return Путь к модели или пустая строка
     */
    static std::string extractModelPath(const nlohmann::json& profile_json);
    
    /**
     * @brief Извлечь имя модели из пути
     * @param model_path Путь к модели
     * @return Короткое имя
     */
    static std::string extractModelName(const std::string& model_path);
    
    /**
     * @brief Извлечь параметры из профиля
     */
    static int extractThreads(const nlohmann::json& json);
    static int extractBatchSize(const nlohmann::json& json);
    static int extractGpuLayers(const nlohmann::json& json);
    static int extractContextSize(const nlohmann::json& json);
    
    /**
     * @brief Получить список доступных профилей
     * @param profiles_dir Директория с профилями
     * @return Список имён профилей (без .json)
     */
    static std::vector<std::string> getAvailableProfiles(const std::string& profiles_dir);
    
    /**
     * @brief Загрузить профиль по имени
     * @param profile_name Имя профиля (без .json)
     * @param profiles_dir Директория с профилями
     * @return JSON объект или пустой если ошибка
     */
    static nlohmann::json loadProfile(const std::string& profile_name,
                                     const std::string& profiles_dir);
    
    /**
     * @brief Валидировать профиль для бенчмарка
     * @param profile_json JSON объект профиля
     * @param error_message Сообщение об ошибке (если есть)
     * @return true если профиль корректен
     */
    static bool validateProfileForBenchmark(const nlohmann::json& profile_json,
                                           std::string& error_message);
};

} // namespace bench
} // namespace llama_gui

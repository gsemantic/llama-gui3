#include "../include/core/model_performance_stats.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

// ============================================================================
// Constructor/Destructor
// ============================================================================

ModelPerformanceManager::ModelPerformanceManager() : models_stats_() {}

ModelPerformanceManager::~ModelPerformanceManager() = default;

// ============================================================================
// Запись метрик
// ============================================================================

void ModelPerformanceManager::record_generation(
    const std::string& model_path,
    double tokens_per_second,
    double response_time_ms,
    size_t tokens_generated,
    size_t context_used,
    size_t total_context
) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& stats = models_stats_[model_path];
    
    // Инициализация при первом использовании
    if (stats.total_generations == 0) {
        stats.model_path = model_path;
        stats.model_name = extract_model_name(model_path);
        stats.first_used = std::chrono::system_clock::now();
        stats.total_context = total_context;
    }

    stats.last_used = std::chrono::system_clock::now();
    stats.total_generations++;

    // Обновление скользящих средних
    stats.update_tokens_per_second(tokens_per_second);
    stats.update_response_time(response_time_ms);
    stats.update_tokens_generated(tokens_generated, stats.total_generations);

    // Обновление статистики контекста
    stats.avg_context_used = static_cast<size_t>(
        0.9 * stats.avg_context_used + 0.1 * context_used
    );
}

// ============================================================================
// Чтение метрик
// ============================================================================

ModelPerformanceStats ModelPerformanceManager::get_model_stats(const std::string& model_path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = models_stats_.find(model_path);
    if (it != models_stats_.end()) {
        return it->second;
    }
    return ModelPerformanceStats{};
}

std::map<std::string, ModelPerformanceStats> ModelPerformanceManager::get_all_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_stats_;
}

size_t ModelPerformanceManager::get_model_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_stats_.size();
}

bool ModelPerformanceManager::has_model_stats(const std::string& model_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_stats_.find(model_path) != models_stats_.end();
}

// ============================================================================
// Управление
// ============================================================================

void ModelPerformanceManager::reset_model_stats(const std::string& model_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    models_stats_.erase(model_path);
}

void ModelPerformanceManager::reset_all_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    models_stats_.clear();
}

// ============================================================================
// Сериализация
// ============================================================================

std::string ModelPerformanceManager::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);

    json j = json::object();

    for (const auto& [path, stats] : models_stats_) {
        json model_json;
        model_json["model_path"] = stats.model_path;
        model_json["model_name"] = stats.model_name;
        model_json["total_generations"] = stats.total_generations;
        model_json["avg_tokens_per_second"] = std::round(stats.avg_tokens_per_second * 100.0) / 100.0;
        model_json["avg_response_time_ms"] = std::round(stats.avg_response_time_ms * 100.0) / 100.0;
        model_json["avg_tokens_generated"] = stats.avg_tokens_generated;
        model_json["avg_context_used"] = stats.avg_context_used;
        model_json["total_context"] = stats.total_context;

        // Временные метки
        auto first_used_time = std::chrono::system_clock::to_time_t(stats.first_used);
        auto last_used_time = std::chrono::system_clock::to_time_t(stats.last_used);
        
        if (first_used_time != 0) {
            model_json["first_used"] = std::ctime(&first_used_time);
            // Удалить newline из ctime
            if (!model_json["first_used"].get<std::string>().empty()) {
                auto& s = model_json["first_used"].get_ref<std::string&>();
                if (!s.empty() && s.back() == '\n') {
                    s.pop_back();
                }
            }
        }
        if (last_used_time != 0) {
            model_json["last_used"] = std::ctime(&last_used_time);
            if (!model_json["last_used"].get<std::string>().empty()) {
                auto& s = model_json["last_used"].get_ref<std::string&>();
                if (!s.empty() && s.back() == '\n') {
                    s.pop_back();
                }
            }
        }

        j[path] = model_json;
    }

    return j.dump(2);
}

bool ModelPerformanceManager::from_json(const std::string& json_str) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        json j = json::parse(json_str);

        models_stats_.clear();

        for (const auto& [path, model_json] : j.items()) {
            ModelPerformanceStats stats;
            stats.model_path = model_json.value("model_path", "");
            stats.model_name = model_json.value("model_name", "");
            stats.total_generations = model_json.value("total_generations", 0);
            stats.avg_tokens_per_second = model_json.value("avg_tokens_per_second", 0.0);
            stats.avg_response_time_ms = model_json.value("avg_response_time_ms", 0.0);
            stats.avg_tokens_generated = model_json.value("avg_tokens_generated", 0);
            stats.avg_context_used = model_json.value("avg_context_used", 0);
            stats.total_context = model_json.value("total_context", 0);

            // Временные метки (восстанавливаем как текущие, если есть данные)
            if (stats.total_generations > 0) {
                stats.first_used = std::chrono::system_clock::now();
                stats.last_used = std::chrono::system_clock::now();
            }

            models_stats_[path] = stats;
        }

        return true;
    } catch (const std::exception& e) {
        // Логирование ошибки
        return false;
    }
}

std::string ModelPerformanceManager::generate_report() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::stringstream ss;
    ss << "=== Model Performance Statistics ===\n";
    ss << "\n";

    if (models_stats_.empty()) {
        ss << "No model statistics available yet.\n";
        return ss.str();
    }

    ss << "Total models tracked: " << models_stats_.size() << "\n";
    ss << "\n";

    for (const auto& [path, stats] : models_stats_) {
        ss << "─────────────────────────────────────\n";
        ss << "Model: " << stats.model_name << "\n";
        ss << "Path: " << stats.model_path << "\n";
        ss << "\n";

        ss << "Performance:\n";
        ss << "  Total generations: " << stats.total_generations << "\n";
        ss << "  Avg tokens/sec: " << std::fixed << std::setprecision(1) 
           << stats.avg_tokens_per_second << "\n";
        ss << "  Avg response time: " << std::fixed << std::setprecision(0)
           << stats.avg_response_time_ms << " ms\n";
        ss << "  Avg tokens generated: " << stats.avg_tokens_generated << "\n";
        ss << "\n";

        ss << "Context Usage:\n";
        ss << "  Avg context used: " << stats.avg_context_used << "\n";
        ss << "  Total context: " << stats.total_context << "\n";
        if (stats.total_context > 0) {
            int percent = static_cast<int>(
                static_cast<float>(stats.avg_context_used) / stats.total_context * 100
            );
            ss << "  Usage: " << percent << "%\n";
        }
        ss << "\n";

        // Временные метки
        if (stats.first_used != std::chrono::system_clock::time_point{}) {
            auto first_used_time = std::chrono::system_clock::to_time_t(stats.first_used);
            auto last_used_time = std::chrono::system_clock::to_time_t(stats.last_used);
            
            char buffer[64];
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&first_used_time));
            ss << "First used: " << buffer << "\n";
            
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&last_used_time));
            ss << "Last used: " << buffer << "\n";
        }
        ss << "\n";
    }

    return ss.str();
}

// ============================================================================
// Вспомогательные функции
// ============================================================================

std::string ModelPerformanceManager::extract_model_name(const std::string& model_path) const {
    // Извлечь имя файла из пути
    size_t pos = model_path.find_last_of("/\\");
    std::string filename = (pos != std::string::npos) ? 
                           model_path.substr(pos + 1) : model_path;

    // Удалить расширение
    size_t ext_pos = filename.find_last_of('.');
    if (ext_pos != std::string::npos) {
        filename = filename.substr(0, ext_pos);
    }

    return filename;
}

} // namespace core
} // namespace llama_gui

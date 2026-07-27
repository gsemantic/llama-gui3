#include "../../include/bench/llama_bench_results.h"
#include "../../include/bench/bench_common.h"

#include <fstream>
#include <algorithm>
#include <stdexcept>

namespace llama_gui {
namespace bench {

/**
 * @brief Внутренняя реализация LlamaBenchResults
 */
struct LlamaBenchResults::Impl {
    std::vector<BenchRunResult> results;
    std::vector<BenchComparisonResult> comparisons;
    
    std::string storage_path;
    std::string history_file;
    
    mutable std::mutex mutex;
    
    // Кэш для быстрого поиска
    std::unordered_map<std::string, size_t> profile_to_result_index;
    std::unordered_map<std::string, size_t> model_to_result_index;
    
    bool dirty = false;  // Есть несохранённые изменения
};

// ============================================================================
// Конструктор/деструктор
// ============================================================================

LlamaBenchResults::LlamaBenchResults()
    : pimpl_(std::make_unique<Impl>()) 
{
    storage_path_ = "bench_results";
    pimpl_->history_file = storage_path_ + "/history.json";
    initializeStorage();
}

LlamaBenchResults::LlamaBenchResults(const std::string& storage_path)
    : pimpl_(std::make_unique<Impl>())
    , storage_path_(storage_path)
{
    pimpl_->history_file = storage_path + "/history.json";
    initializeStorage();
}

LlamaBenchResults::~LlamaBenchResults() {
    // Автосохранение при уничтожении если есть изменения
    if (pimpl_->dirty) {
        saveToHistory();
    }
}

// ============================================================================
// Добавление результатов
// ============================================================================

void LlamaBenchResults::addResult(const BenchRunResult& result) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    pimpl_->results.push_back(result);
    pimpl_->dirty = true;
    
    // Обновить кэш
    if (!result.profile_name.empty()) {
        pimpl_->profile_to_result_index[result.profile_name] = pimpl_->results.size() - 1;
    }
    if (!result.model_path.empty()) {
        pimpl_->model_to_result_index[result.model_path] = pimpl_->results.size() - 1;
    }
}

void LlamaBenchResults::addResults(const std::vector<BenchRunResult>& results) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    for (const auto& result : results) {
        pimpl_->results.push_back(result);
        
        // Обновить кэш
        if (!result.profile_name.empty()) {
            pimpl_->profile_to_result_index[result.profile_name] = pimpl_->results.size() - 1;
        }
        if (!result.model_path.empty()) {
            pimpl_->model_to_result_index[result.model_path] = pimpl_->results.size() - 1;
        }
    }
    
    pimpl_->dirty = true;
}

void LlamaBenchResults::addComparison(const BenchComparisonResult& comparison) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    pimpl_->comparisons.push_back(comparison);
    pimpl_->dirty = true;
}

// ============================================================================
// Сохранение и загрузка
// ============================================================================

bool LlamaBenchResults::saveToFile(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    try {
        nlohmann::json json = toJson();
        
        // Создать директорию при необходимости
        BenchCommon::createDirectory(BenchCommon::extractFileName(file_path.substr(0, file_path.find_last_of('/'))));
        
        std::ofstream file(file_path);
        if (!file.is_open()) {
            return false;
        }
        
        file << json.dump(2);  // Красивый вывод с отступами
        file.close();
        
        return file.good();
    } catch (const std::exception&) {
        return false;
    }
}

bool LlamaBenchResults::loadFromFile(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);

    try {
        std::string content = BenchCommon::readFileToString(file_path);
        if (content.empty()) {
            return false;
        }

        nlohmann::json json = nlohmann::json::parse(content);
        
        // Распарсить JSON и загрузить данные напрямую
        if (json.contains("results") && json["results"].is_array()) {
            pimpl_->results.clear();
            for (const auto& r : json["results"]) {
                BenchRunResult result;
                result.test_id = r.value("test_id", "");
                result.profile_name = r.value("profile_name", "");
                result.model_path = r.value("model_path", "");
                result.model_name = r.value("model_name", "");
                result.prompt_tokens_per_sec = r.value("prompt_tokens_per_sec", 0.0);
                result.gen_tokens_per_sec = r.value("gen_tokens_per_sec", 0.0);
                result.prompt_ms_total = r.value("prompt_ms_total", 0.0);
                result.gen_ms_total = r.value("gen_ms_total", 0.0);
                result.ttft_ms = r.value("ttft_ms", 0.0);
                result.repetitions_completed = r.value("repetitions_completed", 0);
                result.duration_seconds = r.value("duration_seconds", 0.0);
                pimpl_->results.push_back(result);
            }
        }

        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

bool LlamaBenchResults::saveToHistory() {
    return saveToFile(getHistoryFilePath());
}

bool LlamaBenchResults::loadFromHistory() {
    return loadFromFile(getHistoryFilePath());
}

// ============================================================================
// Поиск и фильтрация
// ============================================================================

std::vector<BenchRunResult> LlamaBenchResults::findByProfile(const std::string& profile_name) const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    std::vector<BenchRunResult> filtered;
    
    for (const auto& result : pimpl_->results) {
        if (result.profile_name == profile_name || 
            result.model_path.find(profile_name) != std::string::npos) {
            filtered.push_back(result);
        }
    }
    
    return filtered;
}

std::vector<BenchRunResult> LlamaBenchResults::findByDateRange(
    std::chrono::system_clock::time_point from,
    std::chrono::system_clock::time_point to) const 
{
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    std::vector<BenchRunResult> filtered;
    
    for (const auto& result : pimpl_->results) {
        if (result.start_time >= from && result.start_time <= to) {
            filtered.push_back(result);
        }
    }
    
    return filtered;
}

std::vector<BenchRunResult> LlamaBenchResults::findBestByMetric(
    const std::string& metric, 
    int top_n) const 
{
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    // Копировать результаты для сортировки
    std::vector<BenchRunResult> sorted = pimpl_->results;
    
    // Сортировать по метрике
    std::sort(sorted.begin(), sorted.end(), [&metric](const BenchRunResult& a, const BenchRunResult& b) {
        if (metric == "prompt_tokens_per_sec" || metric == "prompt_tps" || metric == "pp") {
            return a.prompt_tokens_per_sec > b.prompt_tokens_per_sec;
        }
        else if (metric == "generation_tokens_per_sec" || metric == "gen_tps" || metric == "tg") {
            return a.gen_tokens_per_sec > b.gen_tokens_per_sec;
        }
        else if (metric == "ttft_ms" || metric == "ttft") {
            return a.ttft_ms < b.ttft_ms;  // Меньше = лучше
        }
        else if (metric == "duration_seconds" || metric == "duration") {
            return a.duration_seconds < b.duration_seconds;  // Меньше = лучше
        }
        else {
            // По умолчанию сортировка по prompt TPS
            return a.prompt_tokens_per_sec > b.prompt_tokens_per_sec;
        }
    });
    
    // Взять топ-N
    if (static_cast<int>(sorted.size()) > top_n) {
        sorted.resize(top_n);
    }
    
    return sorted;
}

std::vector<BenchRunResult> LlamaBenchResults::findByModel(const std::string& model_path) const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    std::vector<BenchRunResult> filtered;
    
    for (const auto& result : pimpl_->results) {
        if (result.model_path.find(model_path) != std::string::npos) {
            filtered.push_back(result);
        }
    }
    
    return filtered;
}

const BenchRunResult* LlamaBenchResults::getLastResult() const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    if (pimpl_->results.empty()) {
        return nullptr;
    }
    
    return &pimpl_->results.back();
}

// ============================================================================
// Сравнение
// ============================================================================

BenchComparisonResult LlamaBenchResults::compareProfiles(
    const std::vector<std::string>& profile_names) const 
{
    BenchComparisonResult comparison;
    comparison.comparison_id = BenchCommon::getTimestampForFileName();
    comparison.created_at = std::chrono::system_clock::now();
    comparison.profile_names = profile_names;
    
    // Найти результаты для каждого профиля
    for (const auto& profile_name : profile_names) {
        auto results = findByProfile(profile_name);
        
        if (!results.empty()) {
            // Взять лучший результат для профиля
            auto best_it = std::max_element(
                results.begin(), 
                results.end(),
                [](const BenchRunResult& a, const BenchRunResult& b) {
                    return a.prompt_tokens_per_sec < b.prompt_tokens_per_sec;
                }
            );
            
            comparison.results.push_back(*best_it);
        }
    }
    
    // Обновить статистику
    updateComparisonStats(comparison);
    
    return comparison;
}

BenchComparisonResult LlamaBenchResults::compareAllProfiles() const {
    auto profiles = getAllProfileNames();
    return compareProfiles(profiles);
}

// ============================================================================
// Экспорт
// ============================================================================

std::string LlamaBenchResults::exportToJson() const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    nlohmann::json json = toJson();
    return json.dump(2);
}

std::string LlamaBenchResults::exportToCsv() const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    std::ostringstream csv;
    
    // Заголовки
    csv << "test_id,profile_name,model_path,prompt_tps,gen_tps,prompt_ms,gen_ms,ttft_ms,"
        << "repetitions,duration_seconds,status\n";
    
    // Данные
    for (const auto& result : pimpl_->results) {
        csv << result.test_id << ","
            << result.profile_name << ","
            << "\"" << result.model_path << "\","
            << result.prompt_tokens_per_sec << ","
            << result.gen_tokens_per_sec << ","
            << result.prompt_ms_total << ","
            << result.gen_ms_total << ","
            << result.ttft_ms << ","
            << result.repetitions_completed << ","
            << result.duration_seconds << ","
            << result.getStatusString() << "\n";
    }
    
    return csv.str();
}

std::string LlamaBenchResults::exportToMarkdown(bool include_comparison) const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    std::ostringstream md;
    
    // Заголовок
    md << "# Llama Bench Results\n\n";
    md << "Generated: " << BenchCommon::getTimestampReadable() << "\n\n";
    
    // Статистика
    md << "## Summary\n\n";
    md << "- Total results: " << pimpl_->results.size() << "\n";
    md << "- Comparisons: " << pimpl_->comparisons.size() << "\n\n";
    
    // Таблица результатов
    md << "## Results\n\n";
    md << "| Profile | Model | Prompt TPS | Gen TPS | TTFT (ms) | Duration | Status |\n";
    md << "|---------|-------|------------|---------|-----------|----------|--------|\n";
    
    for (const auto& result : pimpl_->results) {
        md << "| " << result.profile_name << " "
           << BenchCommon::getStatusIcon(result.status) << " | "
           << result.model_name << " | "
           << BenchCommon::formatSpeed(result.prompt_tokens_per_sec) << " | "
           << BenchCommon::formatSpeed(result.gen_tokens_per_sec) << " | "
           << BenchCommon::formatTimeMs(result.ttft_ms) << " | "
           << BenchCommon::formatDuration(result.duration_seconds) << " | "
           << result.getStatusString() << " |\n";
    }
    
    md << "\n";
    
    // Таблица сравнения если есть
    if (include_comparison && !pimpl_->comparisons.empty()) {
        md << "## Comparisons\n\n";
        
        for (const auto& comp : pimpl_->comparisons) {
            md << "### Comparison: " << comp.comparison_id << "\n\n";
            
            if (comp.best_prompt_tps_profile.empty() == false) {
                md << "- Best Prompt TPS: **" << comp.best_prompt_tps_profile << "**\n";
            }
            if (comp.best_gen_tps_profile.empty() == false) {
                md << "- Best Gen TPS: **" << comp.best_gen_tps_profile << "**\n";
            }
            
            md << "\n";
        }
    }
    
    return md.str();
}

bool LlamaBenchResults::exportToFile(const std::string& file_path, OutputFormat format) {
    std::string content;
    
    switch (format) {
        case OutputFormat::JSON:
            content = exportToJson();
            break;
        case OutputFormat::CSV:
            content = exportToCsv();
            break;
        case OutputFormat::Markdown:
            content = exportToMarkdown();
            break;
        default:
            return false;
    }
    
    return BenchCommon::writeStringToFile(file_path, content);
}

// ============================================================================
// Статистика
// ============================================================================

size_t LlamaBenchResults::getTotalResults() const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    return pimpl_->results.size();
}

size_t LlamaBenchResults::getComparisonsCount() const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    return pimpl_->comparisons.size();
}

std::chrono::system_clock::time_point LlamaBenchResults::getLastRunTime() const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    if (pimpl_->results.empty()) {
        return std::chrono::system_clock::time_point{};
    }
    
    return pimpl_->results.back().start_time;
}

std::vector<std::string> LlamaBenchResults::getAllProfileNames() const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    std::vector<std::string> names;
    std::unordered_set<std::string> unique_names;
    
    for (const auto& result : pimpl_->results) {
        if (!result.profile_name.empty() && 
            unique_names.find(result.profile_name) == unique_names.end()) {
            names.push_back(result.profile_name);
            unique_names.insert(result.profile_name);
        }
    }
    
    return names;
}

std::vector<std::string> LlamaBenchResults::getAllModelPaths() const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    std::vector<std::string> paths;
    std::unordered_set<std::string> unique_paths;
    
    for (const auto& result : pimpl_->results) {
        if (!result.model_path.empty() && 
            unique_paths.find(result.model_path) == unique_paths.end()) {
            paths.push_back(result.model_path);
            unique_paths.insert(result.model_path);
        }
    }
    
    return paths;
}

// ============================================================================
// Очистка
// ============================================================================

void LlamaBenchResults::clear() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    pimpl_->results.clear();
    pimpl_->comparisons.clear();
    pimpl_->profile_to_result_index.clear();
    pimpl_->model_to_result_index.clear();
    pimpl_->dirty = true;
}

size_t LlamaBenchResults::removeOlderThan(std::chrono::system_clock::time_point cutoff) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    size_t removed = 0;
    
    auto it = std::remove_if(pimpl_->results.begin(), pimpl_->results.end(),
        [&cutoff](const BenchRunResult& result) {
            return result.start_time < cutoff;
        });
    
    removed = std::distance(it, pimpl_->results.end());
    pimpl_->results.erase(it, pimpl_->results.end());
    
    if (removed > 0) {
        pimpl_->dirty = true;
    }
    
    return removed;
}

bool LlamaBenchResults::removeById(const std::string& test_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    auto it = std::find_if(pimpl_->results.begin(), pimpl_->results.end(),
        [&test_id](const BenchRunResult& result) {
            return result.test_id == test_id;
        });
    
    if (it != pimpl_->results.end()) {
        pimpl_->results.erase(it);
        pimpl_->dirty = true;
        return true;
    }
    
    return false;
}

// ============================================================================
// Сериализация
// ============================================================================

nlohmann::json LlamaBenchResults::toJson() const {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    nlohmann::json json = nlohmann::json::object();
    
    // Результаты
    json["results"] = nlohmann::json::array();
    for (const auto& result : pimpl_->results) {
        nlohmann::json r = nlohmann::json::object();
        r["test_id"] = result.test_id;
        r["profile_name"] = result.profile_name;
        r["model_path"] = result.model_path;
        r["model_name"] = result.model_name;
        r["status"] = result.getStatusString();
        r["error_message"] = result.error_message;
        
        // Метрики
        r["prompt_tokens_per_sec"] = result.prompt_tokens_per_sec;
        r["gen_tokens_per_sec"] = result.gen_tokens_per_sec;
        r["prompt_ms_total"] = result.prompt_ms_total;
        r["gen_ms_total"] = result.gen_ms_total;
        r["prompt_ms_per_token"] = result.prompt_ms_per_token;
        r["gen_ms_per_token"] = result.gen_ms_per_token;
        r["ttft_ms"] = result.ttft_ms;
        
        // Параметры
        r["repetitions_completed"] = result.repetitions_completed;
        r["context_depth"] = result.context_depth;
        
        // Память
        r["gpu_memory_mb"] = result.gpu_memory_mb;
        r["cpu_memory_mb"] = result.cpu_memory_mb;
        
        // Время
        r["duration_seconds"] = result.duration_seconds;
        
        json["results"].push_back(r);
    }
    
    // Сравнения
    json["comparisons"] = nlohmann::json::array();
    for (const auto& comp : pimpl_->comparisons) {
        nlohmann::json c = nlohmann::json::object();
        c["comparison_id"] = comp.comparison_id;
        c["profile_names"] = comp.profile_names;
        c["best_prompt_tps_profile"] = comp.best_prompt_tps_profile;
        c["best_gen_tps_profile"] = comp.best_gen_tps_profile;
        c["total_tests_run"] = comp.total_tests_run;
        c["total_tests_failed"] = comp.total_tests_failed;
        
        json["comparisons"].push_back(c);
    }
    
    // Метаданные
    json["metadata"] = nlohmann::json::object();
    json["metadata"]["created_at"] = BenchCommon::getTimestampReadable();
    json["metadata"]["total_results"] = pimpl_->results.size();
    json["metadata"]["total_comparisons"] = pimpl_->comparisons.size();
    
    return json;
}

void LlamaBenchResults::loadFromJson(const nlohmann::json& json) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    
    pimpl_->results.clear();

    if (json.contains("results") && json["results"].is_array()) {
        for (const auto& r : json["results"]) {
            BenchRunResult result;
            result.test_id = r.value("test_id", "");
            result.profile_name = r.value("profile_name", "");
            result.model_path = r.value("model_path", "");
            result.model_name = r.value("model_name", "");
            result.status = BenchStatus::Completed;
            result.error_message = r.value("error_message", "");

            result.prompt_tokens_per_sec = r.value("prompt_tokens_per_sec", 0.0);
            result.gen_tokens_per_sec = r.value("gen_tokens_per_sec", 0.0);
            result.prompt_ms_total = r.value("prompt_ms_total", 0.0);
            result.gen_ms_total = r.value("gen_ms_total", 0.0);
            result.ttft_ms = r.value("ttft_ms", 0.0);

            result.repetitions_completed = r.value("repetitions_completed", 0);
            result.context_depth = r.value("context_depth", 0);

            result.gpu_memory_mb = r.value("gpu_memory_mb", 0);
            result.cpu_memory_mb = r.value("cpu_memory_mb", 0);

            result.duration_seconds = r.value("duration_seconds", 0.0);

            pimpl_->results.push_back(result);
        }
    }
}

// ============================================================================
// Доступ к данным
// ============================================================================

const std::vector<BenchRunResult>& LlamaBenchResults::getResults() const {
    return pimpl_->results;
}

const std::vector<BenchComparisonResult>& LlamaBenchResults::getComparisons() const {
    return pimpl_->comparisons;
}

std::string LlamaBenchResults::getStoragePath() const {
    return storage_path_;
}

void LlamaBenchResults::setStoragePath(const std::string& path) {
    storage_path_ = path;
}

// ============================================================================
// Приватные методы
// ============================================================================

void LlamaBenchResults::initializeStorage() {
    // Создать директорию для хранения
    BenchCommon::createDirectory(storage_path_);
}

std::string LlamaBenchResults::getHistoryFilePath() const {
    return storage_path_ + "/history.json";
}

void LlamaBenchResults::updateComparisonStats(BenchComparisonResult& comparison) const {
    comparison.total_tests_run = static_cast<int>(comparison.results.size());
    comparison.total_tests_failed = 0;
    
    for (const auto& result : comparison.results) {
        if (result.status != BenchStatus::Completed) {
            comparison.total_tests_failed++;
        }
    }
    
    // Найти лучшие профили
    double best_prompt_tps = 0.0;
    double best_gen_tps = 0.0;
    
    for (const auto& result : comparison.results) {
        if (result.prompt_tokens_per_sec > best_prompt_tps) {
            best_prompt_tps = result.prompt_tokens_per_sec;
            comparison.best_prompt_tps_profile = result.profile_name;
        }
        if (result.gen_tokens_per_sec > best_gen_tps) {
            best_gen_tps = result.gen_tokens_per_sec;
            comparison.best_gen_tps_profile = result.profile_name;
        }
    }
    
    // Посчитать общую длительность
    comparison.total_duration_seconds = 0.0;
    for (const auto& result : comparison.results) {
        comparison.total_duration_seconds += result.duration_seconds;
    }
}

} // namespace bench
} // namespace llama_gui

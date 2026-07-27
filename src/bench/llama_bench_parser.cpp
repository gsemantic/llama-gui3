#include "../../include/bench/llama_bench_parser.h"
#include "../../include/bench/bench_common.h"

#include <sstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace llama_gui {
namespace bench {

// ============================================================================
// Основные методы парсинга
// ============================================================================

std::vector<BenchRunResult> LlamaBenchParser::parseJson(const std::string& json_input) {
    std::vector<BenchRunResult> results;
    
    if (json_input.empty()) {
        return results;
    }
    
    try {
        nlohmann::json json = nlohmann::json::parse(json_input);
        
        // llama-bench может возвращать один объект или массив
        if (json.is_array()) {
            for (const auto& item : json) {
                if (item.is_object()) {
                    results.push_back(extractFromJson(item));
                }
            }
        } else if (json.is_object()) {
            // Один объект
            results.push_back(extractFromJson(json));
        }
        
    } catch (const nlohmann::json::parse_error& e) {
        // Если парсинг не удался, вернуть пустой результат
        BenchRunResult error_result;
        error_result.status = BenchStatus::Failed;
        error_result.error_message = std::string("JSON parse error: ") + e.what();
        results.push_back(error_result);
    } catch (const std::exception& e) {
        BenchRunResult error_result;
        error_result.status = BenchStatus::Failed;
        error_result.error_message = std::string("Parse error: ") + e.what();
        results.push_back(error_result);
    }
    
    return results;
}

std::vector<BenchRunResult> LlamaBenchParser::parseCsv(const std::string& csv_input) {
    std::vector<BenchRunResult> results;
    
    if (csv_input.empty()) {
        return results;
    }
    
    std::vector<std::string> lines = splitString(csv_input, '\n');
    if (lines.empty()) {
        return results;
    }
    
    // Первая строка - заголовки
    std::vector<std::string> headers = splitCsvLine(lines[0]);
    
    // Остальные строки - данные
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        if (line.empty()) {
            continue;
        }
        
        results.push_back(extractFromCsv(line, headers));
    }
    
    return results;
}

std::vector<BenchRunResult> LlamaBenchParser::parseMarkdown(const std::string& md_input) {
    std::vector<BenchRunResult> results;
    
    if (md_input.empty()) {
        return results;
    }
    
    std::vector<std::string> lines = splitMarkdownTable(md_input);
    if (lines.empty()) {
        return results;
    }
    
    // Найти заголовки
    std::vector<std::string> headers = parseMarkdownHeaders(lines);
    
    // Найти разделитель (строка с |---|)
    int separator_idx = findMarkdownSeparator(lines);
    if (separator_idx < 0) {
        return results;
    }
    
    // Парсить строки данных после разделителя
    for (size_t i = separator_idx + 1; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        if (line.empty() || line[0] != '|') {
            continue;
        }
        
        // Разделить на ячейки
        std::vector<std::string> cells;
        std::stringstream ss(line);
        std::string cell;
        
        while (std::getline(ss, cell, '|')) {
            std::string trimmed = trim(cell);
            if (!trimmed.empty()) {
                cells.push_back(extractMarkdownCell(trimmed));
            }
        }
        
        if (cells.size() >= headers.size()) {
            // Создать результат из ячеек
            BenchRunResult result;
            result.status = BenchStatus::Completed;
            
            for (size_t j = 0; j < headers.size() && j < cells.size(); ++j) {
                const std::string& header = headers[j];
                const std::string& value = cells[j];
                
                // Маппинг заголовков на поля
                if (header == "model" || header == "Model") {
                    result.model_path = value;
                    result.model_name = BenchCommon::extractFileNameWithoutExt(value);
                }
                else if (header == "prompt_tokens_per_sec" || header == "pp t/s") {
                    result.prompt_tokens_per_sec = parseDouble(value);
                }
                else if (header == "generation_tokens_per_sec" || header == "tg t/s") {
                    result.gen_tokens_per_sec = parseDouble(value);
                }
                else if (header == "prompt_ms_total" || header == "prompt ms") {
                    result.prompt_ms_total = parseDouble(value);
                }
                else if (header == "generation_ms_total" || header == "generation ms") {
                    result.gen_ms_total = parseDouble(value);
                }
                else if (header == "prompt_ms_per_token") {
                    result.prompt_ms_per_token = parseDouble(value);
                }
                else if (header == "generation_ms_per_token") {
                    result.gen_ms_per_token = parseDouble(value);
                }
            }
            
            results.push_back(result);
        }
    }
    
    return results;
}

std::vector<BenchRunResult> LlamaBenchParser::parseJsonl(const std::string& jsonl_input) {
    std::vector<BenchRunResult> results;
    
    if (jsonl_input.empty()) {
        return results;
    }
    
    std::vector<std::string> lines = splitString(jsonl_input, '\n');
    
    for (const std::string& line : lines) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        
        try {
            nlohmann::json json = nlohmann::json::parse(trimmed);
            if (json.is_object()) {
                results.push_back(extractFromJson(json));
            }
        } catch (const std::exception&) {
            // Пропустить некорректную строку
            continue;
        }
    }
    
    return results;
}

std::vector<BenchRunResult> LlamaBenchParser::parse(
    const std::string& input, 
    OutputFormat hint) 
{
    if (input.empty()) {
        return {};
    }
    
    // Если формат не указан или hint ненадёжен, определить автоматически
    OutputFormat format = hint;
    if (format == OutputFormat::SQL) {
        // SQL пока не поддерживается
        return {};
    }
    
    switch (format) {
        case OutputFormat::JSON:
            return parseJson(input);
        case OutputFormat::CSV:
            return parseCsv(input);
        case OutputFormat::Markdown:
            return parseMarkdown(input);
        case OutputFormat::JSONL:
            return parseJsonl(input);
        default:
            // Авто-определение
            format = detectFormat(input);
            return parse(input, format);
    }
}

// ============================================================================
// Валидация
// ============================================================================

bool LlamaBenchParser::validateResult(const BenchRunResult& result) {
    // Проверить статус
    if (result.status != BenchStatus::Completed) {
        return false;
    }
    
    // Проверить наличие хотя бы одной метрики
    if (result.prompt_tokens_per_sec <= 0 && result.gen_tokens_per_sec <= 0) {
        return false;
    }
    
    // Проверить путь к модели
    if (result.model_path.empty()) {
        return false;
    }
    
    return true;
}

bool LlamaBenchParser::hasAllMetrics(const BenchRunResult& result) {
    return result.prompt_tokens_per_sec > 0 &&
           result.gen_tokens_per_sec > 0 &&
           result.prompt_ms_total > 0 &&
           result.gen_ms_total > 0;
}

// ============================================================================
// Вспомогательные методы
// ============================================================================

BenchRunResult LlamaBenchParser::extractFromJson(const nlohmann::json& json_obj) {
    BenchRunResult result;
    result.status = BenchStatus::Completed;
    
    // Извлечь основные поля
    if (json_obj.contains("model")) {
        result.model_path = json_obj.value("model", "");
        result.model_name = BenchCommon::extractFileNameWithoutExt(result.model_path);
    }
    
    if (json_obj.contains("model_name")) {
        result.model_name = json_obj.value("model_name", "");
    }
    
    if (json_obj.contains("test_id")) {
        result.test_id = json_obj.value("test_id", "");
    }
    
    // Метрики производительности
    if (json_obj.contains("prompt_tokens_per_sec")) {
        result.prompt_tokens_per_sec = json_obj.value("prompt_tokens_per_sec", 0.0);
    }
    
    if (json_obj.contains("generation_tokens_per_sec")) {
        result.gen_tokens_per_sec = json_obj.value("generation_tokens_per_sec", 0.0);
    }
    
    if (json_obj.contains("prompt_ms_total")) {
        result.prompt_ms_total = json_obj.value("prompt_ms_total", 0.0);
    }
    
    if (json_obj.contains("generation_ms_total")) {
        result.gen_ms_total = json_obj.value("generation_ms_total", 0.0);
    }
    
    if (json_obj.contains("prompt_ms_per_token")) {
        result.prompt_ms_per_token = json_obj.value("prompt_ms_per_token", 0.0);
    }
    
    if (json_obj.contains("generation_ms_per_token")) {
        result.gen_ms_per_token = json_obj.value("generation_ms_per_token", 0.0);
    }
    
    if (json_obj.contains("ttft_ms")) {
        result.ttft_ms = json_obj.value("ttft_ms", 0.0);
    }
    
    // Параметры теста
    if (json_obj.contains("n_prompt")) {
        result.params.n_prompt = json_obj.value("n_prompt", 512);
    }
    
    if (json_obj.contains("n_gen")) {
        result.params.n_gen = json_obj.value("n_gen", 128);
    }
    
    if (json_obj.contains("batch_size")) {
        result.params.batch_size = json_obj.value("batch_size", 2048);
    }
    
    if (json_obj.contains("threads")) {
        result.params.threads = json_obj.value("threads", 2);
    }
    
    if (json_obj.contains("n_gpu_layers")) {
        result.params.n_gpu_layers = json_obj.value("n_gpu_layers", 99);
    }
    
    // Статистика повторений
    if (json_obj.contains("repetitions_completed")) {
        result.repetitions_completed = json_obj.value("repetitions_completed", 0);
    }
    
    if (json_obj.contains("prompt_tpss")) {
        result.prompt_tpss = json_obj.value("prompt_tpss", std::vector<double>{});
    }
    
    if (json_obj.contains("gen_tpss")) {
        result.gen_tpss = json_obj.value("gen_tpss", std::vector<double>{});
    }
    
    // Использование памяти
    if (json_obj.contains("gpu_memory_mb")) {
        result.gpu_memory_mb = json_obj.value("gpu_memory_mb", 0);
    }
    
    if (json_obj.contains("cpu_memory_mb")) {
        result.cpu_memory_mb = json_obj.value("cpu_memory_mb", 0);
    }
    
    // Длительность
    if (json_obj.contains("duration_seconds")) {
        result.duration_seconds = json_obj.value("duration_seconds", 0.0);
    }
    
    return result;
}

BenchRunResult LlamaBenchParser::extractFromCsv(
    const std::string& csv_line, 
    const std::vector<std::string>& headers) 
{
    BenchRunResult result;
    result.status = BenchStatus::Completed;
    
    std::vector<std::string> values = splitCsvLine(csv_line);
    
    for (size_t i = 0; i < headers.size() && i < values.size(); ++i) {
        const std::string& header = headers[i];
        const std::string& value = values[i];
        
        if (header == "model" || header == "model_path") {
            result.model_path = value;
            result.model_name = BenchCommon::extractFileNameWithoutExt(value);
        }
        else if (header == "prompt_tokens_per_sec") {
            result.prompt_tokens_per_sec = parseDouble(value);
        }
        else if (header == "generation_tokens_per_sec") {
            result.gen_tokens_per_sec = parseDouble(value);
        }
        else if (header == "prompt_ms_total") {
            result.prompt_ms_total = parseDouble(value);
        }
        else if (header == "generation_ms_total") {
            result.gen_ms_total = parseDouble(value);
        }
        else if (header == "repetitions_completed") {
            result.repetitions_completed = parseInt(value);
        }
        else if (header == "duration_seconds") {
            result.duration_seconds = parseDouble(value);
        }
    }
    
    return result;
}

OutputFormat LlamaBenchParser::detectFormat(const std::string& input) {
    std::string trimmed = trim(input);
    
    // Проверить JSON (начинается с { или [)
    if (!trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '[')) {
        // Проверить JSONL (множественные JSON объекты)
        if (trimmed.find("}\n{") != std::string::npos || 
            trimmed.find("}\r\n{") != std::string::npos) {
            return OutputFormat::JSONL;
        }
        return OutputFormat::JSON;
    }
    
    // Проверить Markdown (содержит | и ---)
    if (trimmed.find('|') != std::string::npos && 
        trimmed.find("---") != std::string::npos) {
        return OutputFormat::Markdown;
    }
    
    // Проверить CSV (содержит запятые и заголовки)
    if (trimmed.find(',') != std::string::npos) {
        return OutputFormat::CSV;
    }
    
    // По умолчанию JSON
    return OutputFormat::JSON;
}

// ============================================================================
// Приватные вспомогательные методы
// ============================================================================

std::vector<std::string> LlamaBenchParser::splitCsvLine(const std::string& line) {
    std::vector<std::string> result;
    std::string cell;
    bool in_quotes = false;
    
    for (size_t i = 0; i < line.length(); ++i) {
        char c = line[i];
        
        if (c == '"') {
            in_quotes = !in_quotes;
        }
        else if (c == ',' && !in_quotes) {
            result.push_back(trim(cell));
            cell.clear();
        }
        else {
            cell += c;
        }
    }
    
    // Добавить последнюю ячейку
    if (!cell.empty()) {
        result.push_back(trim(cell));
    }
    
    return result;
}

std::vector<std::string> LlamaBenchParser::splitMarkdownTable(const std::string& md) {
    std::vector<std::string> lines;
    std::istringstream iss(md);
    std::string line;
    
    while (std::getline(iss, line)) {
        std::string trimmed = trim(line);
        if (!trimmed.empty()) {
            lines.push_back(trimmed);
        }
    }
    
    return lines;
}

std::vector<std::string> LlamaBenchParser::splitString(
    const std::string& str, 
    char delimiter) 
{
    std::vector<std::string> result;
    std::istringstream iss(str);
    std::string token;
    
    while (std::getline(iss, token, delimiter)) {
        result.push_back(token);
    }
    
    return result;
}

std::string LlamaBenchParser::trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

double LlamaBenchParser::parseDouble(const std::string& str, double default_val) {
    std::string trimmed = trim(str);
    if (trimmed.empty()) {
        return default_val;
    }
    
    try {
        return std::stod(trimmed);
    } catch (...) {
        return default_val;
    }
}

int LlamaBenchParser::parseInt(const std::string& str, int default_val) {
    std::string trimmed = trim(str);
    if (trimmed.empty()) {
        return default_val;
    }
    
    try {
        return std::stoi(trimmed);
    } catch (...) {
        return default_val;
    }
}

BenchStatus LlamaBenchParser::parseStatus(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "completed" || lower == "success") {
        return BenchStatus::Completed;
    }
    if (lower == "failed" || lower == "error") {
        return BenchStatus::Failed;
    }
    if (lower == "cancelled" || lower == "aborted") {
        return BenchStatus::Cancelled;
    }
    if (lower == "running" || lower == "in_progress") {
        return BenchStatus::Running;
    }
    
    return BenchStatus::Pending;
}

std::string LlamaBenchParser::extractMarkdownCell(const std::string& cell) {
    // Удалить лишние пробелы и возможные форматирования
    std::string result = trim(cell);
    
    // Удалить звёздочки (жирный текст)
    while (result.length() >= 2 && result.front() == '*' && result.back() == '*') {
        result = result.substr(1, result.length() - 2);
    }
    
    // Удалить подчёркивания (курсив)
    while (result.length() >= 1 && result.front() == '_') {
        result = result.substr(1);
    }
    while (result.length() >= 1 && result.back() == '_') {
        result = result.substr(0, result.length() - 1);
    }
    
    return trim(result);
}

std::vector<std::string> LlamaBenchParser::parseMarkdownHeaders(
    const std::vector<std::string>& lines) 
{
    if (lines.empty()) {
        return {};
    }
    
    // Первая строка - заголовки
    std::string header_line = lines[0];
    
    // Разделить на ячейки
    std::vector<std::string> headers;
    std::stringstream ss(header_line);
    std::string cell;
    
    while (std::getline(ss, cell, '|')) {
        std::string trimmed = trim(cell);
        if (!trimmed.empty()) {
            headers.push_back(extractMarkdownCell(trimmed));
        }
    }
    
    return headers;
}

int LlamaBenchParser::findMarkdownSeparator(const std::vector<std::string>& lines) {
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        
        // Искать строку с ---
        if (line.find("---") != std::string::npos && line.find('|') != std::string::npos) {
            return static_cast<int>(i);
        }
    }
    
    return -1;  // Не найдено
}

} // namespace bench
} // namespace llama_gui

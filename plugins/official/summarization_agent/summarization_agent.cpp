/**
 * @file summarization_agent.cpp
 * @brief Реализация агента для суммаризации текста
 */

#include "summarization_agent.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

namespace agents {

// ============================================================================
// SummarizationAgent implementation
// ============================================================================

SummarizationAgent::SummarizationAgent() = default;

SummarizationAgent::~SummarizationAgent() {
    shutdown();
}

const char* SummarizationAgent::name() const {
    return "summarization_agent";
}

const char* SummarizationAgent::description() const {
    return "Text summarization agent. Provides extractive summarization using "
           "sentence scoring and keyword extraction. Supports multiple styles "
           "including abstract, key points, and TL;DR.";
}

const char* SummarizationAgent::version() const {
    return "1.0.0";
}

bool SummarizationAgent::initialize(AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Загрузка настроек
    auto config = context_->get_agent_config(name());
    if (config.contains("max_summary_ratio")) {
        max_summary_ratio_ = config["max_summary_ratio"].get<float>();
    }
    if (config.contains("min_summary_length")) {
        min_summary_length_ = config["min_summary_length"].get<int>();
    }
    if (config.contains("max_summary_length")) {
        max_summary_length_ = config["max_summary_length"].get<int>();
    }
    
    context_->info(name(), "Initialized with ratio=" + 
                   std::to_string(max_summary_ratio_));
    
    return true;
}

AgentResult SummarizationAgent::execute(const AgentRequest& request) {
    if (!initialized_) {
        return AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "summarize") {
        return handle_summarize(request);
    } else if (action == "summarize_file") {
        return handle_summarize_file(request);
    } else if (action == "extract_key_points") {
        return handle_extract_key_points(request);
    } else if (action == "generate_abstract") {
        return handle_generate_abstract(request);
    } else if (action == "tldr") {
        return handle_tldr(request);
    }
    
    return AgentResult::error("Unknown action: " + action);
}

void SummarizationAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down. Summarized: " + 
                       std::to_string(summarization_count_));
    }
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability SummarizationAgent::capabilities() const {
    return AgentCapability::TEXT_SUMMARY | 
           AgentCapability::TEXT_EXTRACTION;
}

bool SummarizationAgent::is_ready() const {
    return initialized_;
}

// ============================================================================
// Обработчики действий
// ============================================================================

AgentResult SummarizationAgent::handle_summarize(const AgentRequest& request) {
    std::string text = request.get_param<std::string>("text", "");
    int max_sentences = request.get_param<int>("max_sentences", 5);
    std::string style = request.get_param<std::string>("style", default_style_);
    
    if (text.empty()) {
        return AgentResult::error("Text is empty");
    }
    
    if (text.length() > 100000) {
        return AgentResult::error("Text too long (max 100000 chars)");
    }
    
    // Экстрактивная суммаризация
    std::string summary = extractive_summarize(text, max_sentences);
    
    // Ограничение длины
    if (static_cast<int>(summary.length()) > max_summary_length_) {
        summary = summary.substr(0, max_summary_length_) + "...";
    }
    
    summarization_count_++;
    
    nlohmann::json result;
    result["summary"] = summary;
    result["original_length"] = static_cast<int>(text.length());
    result["summary_length"] = static_cast<int>(summary.length());
    result["ratio"] = static_cast<float>(summary.length()) / text.length();
    result["style"] = style;
    
    // Ключевые слова
    auto keywords = extract_keywords(text);
    if (keywords.size() > 10) {
        keywords.resize(10);
    }
    result["keywords"] = keywords;
    
    return AgentResult::success(result);
}

AgentResult SummarizationAgent::handle_summarize_file(const AgentRequest& request) {
    std::string file_path = request.file_path();
    
    if (file_path.empty()) {
        return AgentResult::error("File path is empty");
    }
    
    // Чтение файла
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return AgentResult::error("Cannot open file: " + file_path);
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    if (content.empty()) {
        return AgentResult::error("File is empty");
    }
    
    int max_sentences = request.get_param<int>("max_sentences", 5);
    
    std::string summary = extractive_summarize(content, max_sentences);
    
    summarization_count_++;
    
    return AgentResult::success({
        {"summary", summary},
        {"file_path", file_path},
        {"original_length", static_cast<int>(content.length())},
        {"summary_length", static_cast<int>(summary.length())}
    });
}

AgentResult SummarizationAgent::handle_extract_key_points(const AgentRequest& request) {
    std::string text = request.get_param<std::string>("text", "");
    int max_points = request.get_param<int>("max_points", 5);
    
    if (text.empty()) {
        return AgentResult::error("Text is empty");
    }
    
    // Разделение на предложения и сортировка по важности
    auto sentences = split_into_sentences(text);
    
    std::vector<std::pair<float, std::string>> scored;
    for (const auto& sentence : sentences) {
        float score = score_sentence(sentence, text);
        scored.push_back({score, sentence});
    }
    
    // Сортировка по убыванию важности
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // Берём топ-N
    nlohmann::json key_points = nlohmann::json::array();
    for (size_t i = 0; i < std::min(static_cast<size_t>(max_points), scored.size()); i++) {
        nlohmann::json point;
        point["text"] = scored[i].second;
        point["score"] = scored[i].first;
        key_points.push_back(point);
    }
    
    return AgentResult::success({
        {"key_points", key_points},
        {"count", static_cast<int>(key_points.size())}
    });
}

AgentResult SummarizationAgent::handle_generate_abstract(const AgentRequest& request) {
    std::string text = request.get_param<std::string>("text", "");
    
    if (text.empty()) {
        return AgentResult::error("Text is empty");
    }
    
    // Абстракт - это короткая суммаризация (2-3 предложения)
    std::string abstract = extractive_summarize(text, 3);
    
    summarization_count_++;
    
    return AgentResult::success({
        {"abstract", abstract},
        {"length", static_cast<int>(abstract.length())}
    });
}

AgentResult SummarizationAgent::handle_tldr(const AgentRequest& request) {
    std::string text = request.get_param<std::string>("text", "");
    
    if (text.empty()) {
        return AgentResult::error("Text is empty");
    }
    
    // TL;DR - одно самое важное предложение
    std::string tldr = extractive_summarize(text, 1);
    
    summarization_count_++;
    
    return AgentResult::success({
        {"tldr", tldr},
        {"length", static_cast<int>(tldr.length())}
    });
}

// ============================================================================
// Вспомогательные функции
// ============================================================================

std::vector<std::string> SummarizationAgent::split_into_sentences(
        const std::string& text) const {
    std::vector<std::string> sentences;
    std::string current;
    
    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        current += c;
        
        // Конец предложения
        if (c == '.' || c == '!' || c == '?') {
            // Проверка на сокращения
            if (i + 1 < text.length() && 
                (text[i + 1] == ' ' || text[i + 1] == '\n')) {
                // Удаляем ведущие пробелы
                size_t start = current.find_first_not_of(" \t\n");
                if (start != std::string::npos) {
                    sentences.push_back(current.substr(start));
                }
                current.clear();
            }
        }
    }
    
    // Остаток
    if (!current.empty()) {
        size_t start = current.find_first_not_of(" \t\n");
        if (start != std::string::npos) {
            sentences.push_back(current.substr(start));
        }
    }
    
    return sentences;
}

float SummarizationAgent::score_sentence(const std::string& sentence,
                                          const std::string& text) const {
    float score = 0.0f;
    
    // Длина предложения (более длинные обычно информативнее)
    score += std::min(1.0f, static_cast<float>(sentence.length()) / 200.0f);
    
    // Ключевые слова
    auto keywords = extract_keywords(text);
    std::string sentence_lower = sentence;
    std::transform(sentence_lower.begin(), sentence_lower.end(), 
                   sentence_lower.begin(), ::tolower);
    
    for (const auto& keyword : keywords) {
        if (sentence_lower.find(keyword) != std::string::npos) {
            score += 0.5f;
        }
    }
    
    // Позиция в тексте (первые предложения часто важнее)
    size_t pos = text.find(sentence);
    if (pos != std::string::npos) {
        float position_score = 1.0f - static_cast<float>(pos) / text.length();
        score += position_score * 0.5f;
    }
    
    return score;
}

std::vector<std::string> SummarizationAgent::extract_keywords(
        const std::string& text) const {
    std::unordered_map<std::string, int> word_freq;
    
    // Токенизация
    std::string current_word;
    for (char c : text) {
        if (std::isalpha(c)) {
            current_word += std::tolower(c);
        } else {
            if (current_word.length() > 3) {  // Игнорируем короткие слова
                word_freq[current_word]++;
            }
            current_word.clear();
        }
    }
    
    // Стоп-слова (упрощённый список)
    static const std::unordered_set<std::string> stop_words = {
        "this", "that", "with", "have", "from", "they", "will", "would",
        "there", "their", "what", "about", "which", "when", "make", "like",
        "just", "over", "such", "into", "than", "them", "these", "some",
        "also", "only", "other", "come", "could", "give", "most"
    };
    
    // Фильтрация и сортировка
    std::vector<std::pair<int, std::string>> freq_vec;
    for (const auto& [word, freq] : word_freq) {
        if (stop_words.count(word) == 0) {
            freq_vec.push_back({freq, word});
        }
    }
    
    std::sort(freq_vec.begin(), freq_vec.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    std::vector<std::string> keywords;
    for (size_t i = 0; i < std::min(freq_vec.size(), static_cast<size_t>(20)); i++) {
        keywords.push_back(freq_vec[i].second);
    }
    
    return keywords;
}

std::string SummarizationAgent::extractive_summarize(
        const std::string& text, int max_sentences) const {
    auto sentences = split_into_sentences(text);
    
    if (sentences.empty()) {
        return "";
    }
    
    // Подсчёт очков для каждого предложения
    std::vector<std::pair<float, size_t>> scored;
    for (size_t i = 0; i < sentences.size(); i++) {
        float score = score_sentence(sentences[i], text);
        scored.push_back({score, i});
    }
    
    // Сортировка по убыванию
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // Берём топ-N предложений
    int actual_max = std::min(max_sentences, static_cast<int>(sentences.size()));
    
    std::vector<std::pair<float, size_t>> top(scored.begin(), 
                                               scored.begin() + actual_max);
    
    // Сортировка по оригинальной позиции (для сохранения порядка)
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    
    // Сборка суммаризации
    std::ostringstream summary;
    for (size_t i = 0; i < top.size(); i++) {
        summary << sentences[top[i].second];
        if (i < top.size() - 1) {
            summary << " ";
        }
    }
    
    return summary.str();
}

AgentResult SummarizationAgent::llm_summarize(const std::string& text,
                                               const std::string& style) const {
    // Заглушка для будущей LLM интеграции
    (void)text;
    (void)style;
    
    return AgentResult::error("LLM summarization not implemented. "
                              "Use extractive summarization instead.");
}

} // namespace agents

// ============================================================================
// C-API экспорт
// ============================================================================

extern "C" {

AGENT_PLUGIN_EXPORT const char* plugin_get_name() {
    return "summarization_agent";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_version() {
    return "1.0.0";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_api_version() {
    return AGENT_PLUGIN_API_VERSION;
}

AGENT_PLUGIN_EXPORT agents::IAgent* plugin_create_agent() {
    return new agents::SummarizationAgent();
}

AGENT_PLUGIN_EXPORT void plugin_destroy_agent(agents::IAgent* agent) {
    delete agent;
}

AGENT_PLUGIN_EXPORT PluginExports* plugin_get_exports() {
    static PluginExports exports = {
        AGENT_PLUGIN_API_VERSION,
        plugin_get_name,
        plugin_get_version,
        plugin_get_api_version,
        reinterpret_cast<plugin_create_agent_fn>(plugin_create_agent),
        reinterpret_cast<plugin_destroy_agent_fn>(plugin_destroy_agent),
        nullptr,
        nullptr
    };
    return &exports;
}

} // extern "C"

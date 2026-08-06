#pragma once

/**
 * @file summarization_agent.h
 * @brief Агент для суммаризации текста и документов
 */

#include <agents/agents.h>
#include <string>
#include <vector>
#include <memory>

namespace agents {

/**
 * @brief Агент для суммаризации текста
 * 
 * Поддерживаемые действия:
 * - summarize - суммаризация текста
 * - summarize_file - суммаризация файла
 * - extract_key_points - извлечение ключевых точек
 * - generate_abstract - генерация абстракта
 * - tl;dr - краткое содержание (очень коротко)
 */
class SummarizationAgent : public IAgent {
public:
    SummarizationAgent();
    ~SummarizationAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(AgentContext* context) override;
    AgentResult execute(const AgentRequest& request) override;
    void shutdown() override;
    AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    // Обработчики действий
    AgentResult handle_summarize(const AgentRequest& request);
    AgentResult handle_summarize_file(const AgentRequest& request);
    AgentResult handle_extract_key_points(const AgentRequest& request);
    AgentResult handle_generate_abstract(const AgentRequest& request);
    AgentResult handle_tldr(const AgentRequest& request);

    /**
     * @brief Разделение текста на предложения
     */
    std::vector<std::string> split_into_sentences(const std::string& text) const;

    /**
     * @brief Подсчёт важности предложения
     */
    float score_sentence(const std::string& sentence,
                         const std::string& text) const;

    /**
     * @brief Извлечение ключевых слов
     */
    std::vector<std::string> extract_keywords(const std::string& text) const;

    /**
     * @brief Суммаризация через выделение важных предложений
     */
    std::string extractive_summarize(const std::string& text,
                                      int max_sentences) const;

    /**
     * @brief Суммаризация через LLM (заглушка)
     */
    AgentResult llm_summarize(const std::string& text,
                               const std::string& style) const;

    AgentContext* context_ = nullptr;
    bool initialized_ = false;

    // Настройки
    float max_summary_ratio_ = 0.3f;  // Максимум 30% от оригинала
    int min_summary_length_ = 100;    // Минимум 100 символов
    int max_summary_length_ = 2000;   // Максимум 2000 символов
    std::string default_style_ = "neutral";
    
    // Статистика
    int summarization_count_ = 0;
};

} // namespace agents

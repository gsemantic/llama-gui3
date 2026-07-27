#pragma once

/**
 * @file code_agent.h
 * @brief Агент для генерации и анализа кода
 */

#include <agents/agents.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace agents {

/**
 * @brief Языки программирования для Code Agent
 */
enum class CodeLanguage {
    UNKNOWN,
    CPP,
    C,
    PYTHON,
    JAVASCRIPT,
    TYPESCRIPT,
    JAVA,
    CSHARP,
    GO,
    RUST,
    RUBY,
    PHP,
    SWIFT,
    KOTLIN
};

/**
 * @brief Агент для работы с кодом
 * 
 * Поддерживаемые действия:
 * - generate - генерация кода
 * - analyze - анализ кода
 * - format - форматирование
 * - explain - объяснение кода
 * - refactor - рефакторинг
 * - translate - перевод между языками
 * - lint - проверка стиля
 */
class CodeAgent : public IAgent {
public:
    CodeAgent();
    ~CodeAgent() override;

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
    AgentResult handle_generate(const AgentRequest& request);
    AgentResult handle_analyze(const AgentRequest& request);
    AgentResult handle_format(const AgentRequest& request);
    AgentResult handle_explain(const AgentRequest& request);
    AgentResult handle_refactor(const AgentRequest& request);
    AgentResult handle_translate(const AgentRequest& request);
    AgentResult handle_lint(const AgentRequest& request);

    /**
     * @brief Определение языка по расширению
     */
    CodeLanguage detect_language(const std::string& filename) const;

    /**
     * @brief Определение языка по содержимому
     */
    CodeLanguage detect_language_by_content(const std::string& content) const;

    /**
     * @brief Получение расширения для языка
     */
    std::string get_extension_for_language(CodeLanguage lang) const;

    /**
     * @brief Генерация кода через LLM
     */
    AgentResult generate_via_llm(const std::string& prompt,
                                  CodeLanguage language);

    AgentContext* context_ = nullptr;
    bool initialized_ = false;

    // Настройки
    CodeLanguage default_language_ = CodeLanguage::CPP;
    size_t max_code_length_ = 10000;
    std::string default_model_ = "default";
    
    // Статистика
    int generation_count_ = 0;
    int analysis_count_ = 0;
};

} // namespace agents

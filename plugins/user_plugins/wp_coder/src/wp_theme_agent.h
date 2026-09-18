#pragma once

#include <agents/agents.h>
#include <string>
#include <vector>
#include <memory>
#include <core/openrouter_client.h>

namespace wp_coder {

/**
 * @brief Агент для генерации тем WordPress
 * 
 * Поддерживаемые действия:
 * - generate — генерация темы по описанию
 * - scaffold — создание структуры файлов темы
 * - add_hooks — добавление хуков в тему
 * - localize — подготовка к локализации
 */
class WPThemeAgent : public agents::IAgent {
public:
    WPThemeAgent();
    ~WPThemeAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(agents::AgentContext* context) override;
    agents::AgentResult execute(const agents::AgentRequest& request) override;
    void shutdown() override;
    agents::AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    agents::AgentResult handle_generate(const agents::AgentRequest& request);
    agents::AgentResult handle_scaffold(const agents::AgentRequest& request);
    agents::AgentResult handle_add_hooks(const agents::AgentRequest& request);
    agents::AgentResult handle_localize(const agents::AgentRequest& request);

    agents::AgentContext* context_ = nullptr;
    bool initialized_ = false;
    std::unique_ptr<llama_gui::core::OpenRouterClient> llm_client_;

    nlohmann::json parse_generated_files(const std::string& generated_code) const;
};

} // namespace wp_coder
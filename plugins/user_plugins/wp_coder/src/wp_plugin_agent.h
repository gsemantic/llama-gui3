#pragma once

#include <agents/agents.h>
#include <string>
#include <vector>
#include <memory>

namespace wp_coder {

/**
 * @brief Агент для генерации плагинов WordPress
 * 
 * Поддерживаемые действия:
 * - generate — генерация плагина по описанию
 * - scaffold — создание структуры файлов плагина
 * - add_shortcode — добавление шорткода
 * - add_rest — добавление REST endpoint
 * - add_gutenberg — подготовка Gutenberg-блока
 */
class WPPluginAgent : public agents::IAgent {
public:
    WPPluginAgent();
    ~WPPluginAgent() override;

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
    agents::AgentResult handle_add_shortcode(const agents::AgentRequest& request);
    agents::AgentResult handle_add_rest(const agents::AgentRequest& request);
    agents::AgentResult handle_add_gutenberg(const agents::AgentRequest& request);

    agents::AgentContext* context_ = nullptr;
    bool initialized_ = false;
};

} // namespace wp_coder
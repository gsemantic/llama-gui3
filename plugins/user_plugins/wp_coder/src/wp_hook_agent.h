#pragma once

#include <agents/agents.h>
#include <string>
#include <vector>
#include <memory>

namespace wp_coder {

/**
 * @brief Агент для работы с хуками WordPress
 * 
 * Поддерживаемые действия:
 * - find — поиск хуков в кодовой базе
 * - generate — генерация кода хука
 * - map — построение карты хуков темы/плагина
 * - validate — валидация сигнатур хуков
 */
class WPHookAgent : public agents::IAgent {
public:
    WPHookAgent();
    ~WPHookAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(agents::AgentContext* context) override;
    agents::AgentResult execute(const agents::AgentRequest& request) override;
    void shutdown() override;
    agents::AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    agents::AgentResult handle_find(const agents::AgentRequest& request);
    agents::AgentResult handle_generate(const agents::AgentRequest& request);
    agents::AgentResult handle_map(const agents::AgentRequest& request);
    agents::AgentResult handle_validate(const agents::AgentRequest& request);

    agents::AgentContext* context_ = nullptr;
    bool initialized_ = false;
};

} // namespace wp_coder
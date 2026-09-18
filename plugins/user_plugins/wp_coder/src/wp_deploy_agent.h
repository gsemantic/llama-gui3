#pragma once

#include <agents/agents.h>
#include <string>
#include <vector>
#include <memory>
#include <core/openrouter_client.h>

namespace wp_coder {

/**
 * @brief Агент для деплоя WordPress
 * 
 * Поддерживаемые действия:
 * - deploy — деплой на хостинг (rsync, WP-CLI)
 * - migrate_db — миграция БД
 * - rollback — откат деплоя
 * - backup — создание бэкапа
 * - verify — проверка деплоя
 */
class WPDeployAgent : public agents::IAgent {
public:
    WPDeployAgent();
    ~WPDeployAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(agents::AgentContext* context) override;
    agents::AgentResult execute(const agents::AgentRequest& request) override;
    void shutdown() override;
    agents::AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    agents::AgentResult handle_deploy(const agents::AgentRequest& request);
    agents::AgentResult handle_migrate_db(const agents::AgentRequest& request);
    agents::AgentResult handle_rollback(const agents::AgentRequest& request);
    agents::AgentResult handle_backup(const agents::AgentRequest& request);
    agents::AgentResult handle_verify(const agents::AgentRequest& request);

    agents::AgentContext* context_ = nullptr;
    bool initialized_ = false;
    std::unique_ptr<llama_gui::core::OpenRouterClient> llm_client_;
};

} // namespace wp_coder
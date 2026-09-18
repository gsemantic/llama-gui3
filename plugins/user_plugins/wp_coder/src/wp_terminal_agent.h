#pragma once

#include <agents/agents.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <memory>

namespace wp_coder {

/**
 * @brief Агент для выполнения WP-CLI и shell-команд
 * 
 * Поддерживаемые действия:
 * - exec — выполнение команды
 * - exec_safe — выполнение из белого списка
 * - wp_cli — выполнение WP-CLI команды
 * - list_commands — список разрешённых команд
 * - add_command — добавление в белый список
 */
class WPTerminalAgent : public agents::IAgent {
public:
    WPTerminalAgent();
    ~WPTerminalAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(agents::AgentContext* context) override;
    agents::AgentResult execute(const agents::AgentRequest& request) override;
    void shutdown() override;
    agents::AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    agents::AgentResult handle_exec(const agents::AgentRequest& request);
    agents::AgentResult handle_exec_safe(const agents::AgentRequest& request);
    agents::AgentResult handle_wp_cli(const agents::AgentRequest& request);
    agents::AgentResult handle_list_commands(const agents::AgentRequest& request);
    agents::AgentResult handle_add_command(const agents::AgentRequest& request);

    bool is_command_allowed(const std::string& command) const;
    std::string extract_base_command(const std::string& command) const;
    bool is_dangerous_command(const std::string& command) const;

    agents::AgentContext* context_ = nullptr;
    bool initialized_ = false;

    std::unordered_set<std::string> allowed_commands_;
    std::string default_shell_ = "/bin/bash";
    bool strict_mode_ = true;
};

} // namespace wp_coder
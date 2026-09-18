#pragma once

#include <agents/agents.h>
#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

namespace wp_coder {

/**
 * @brief Оркестратор для wp-coder, делегирующий задачи субагентам
 * 
 * Поддерживаемые действия:
 * - generate_theme — генерация темы WordPress
 * - generate_plugin — генерация плагина WordPress
 * - find_hooks — поиск хуков WordPress
 * - deploy — деплой на хостинг
 * - search_code — поиск по кодовой базе (RAG)
 * - exec_cli — выполнение WP-CLI команд
 * - file_ops — операции с файлами
 */
class WPCoderOrchestrator : public agents::IAgent {
public:
    WPCoderOrchestrator();
    ~WPCoderOrchestrator() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(agents::AgentContext* context) override;
    agents::AgentResult execute(const agents::AgentRequest& request) override;
    void shutdown() override;
    agents::AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    // Обработчики действий
    agents::AgentResult handle_generate_theme(const agents::AgentRequest& request);
    agents::AgentResult handle_generate_plugin(const agents::AgentRequest& request);
    agents::AgentResult handle_find_hooks(const agents::AgentRequest& request);
    agents::AgentResult handle_deploy(const agents::AgentRequest& request);
    agents::AgentResult handle_search_code(const agents::AgentRequest& request);
    agents::AgentResult handle_exec_cli(const agents::AgentRequest& request);
    agents::AgentResult handle_file_ops(const agents::AgentRequest& request);

    // Управление субагентами
    void load_harness_profile(const std::string& profile_name);
    std::string get_current_profile() const;

    agents::AgentContext* context_ = nullptr;
    bool initialized_ = false;

    // Настройки
    std::string current_profile_ = "fast_local";
    nlohmann::json profile_config_;
};

} // namespace wp_coder
#pragma once

#include <agents/agents.h>
#include <string>
#include <vector>
#include <memory>

namespace wp_coder {

/**
 * @brief Агент RAG для WordPress-кодовой базы
 * 
 * Поддерживаемые действия:
 * - search — семантический поиск по коду WP
 * - add_corpus — добавление корпуса (ядро WP, темы, плагины)
 * - list_corpus — список документов в корпусе
 * - clear — очистка корпуса
 * - stats — статистика
 */
class WPRagAgent : public agents::IAgent {
public:
    WPRagAgent();
    ~WPRagAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(agents::AgentContext* context) override;
    agents::AgentResult execute(const agents::AgentRequest& request) override;
    void shutdown() override;
    agents::AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    agents::AgentResult handle_search(const agents::AgentRequest& request);
    agents::AgentResult handle_add_corpus(const agents::AgentRequest& request);
    agents::AgentResult handle_list_corpus(const agents::AgentRequest& request);
    agents::AgentResult handle_clear(const agents::AgentRequest& request);
    agents::AgentResult handle_stats(const agents::AgentRequest& request);

    agents::AgentContext* context_ = nullptr;
    bool initialized_ = false;
};

} // namespace wp_coder
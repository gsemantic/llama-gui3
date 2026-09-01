#pragma once

/**
 * @file web_search_agent.h
 * @brief Агент для HTTP запросов и поиска в интернете
 */

#include <agents/agents.h>
#include <string>
#include <vector>
#include <chrono>

// CURL будет объявлен в curl.h который включается в .cpp файле

namespace agents {

/**
 * @brief Агент для работы с HTTP и поиска в интернете
 * 
 * Поддерживаемые действия:
 * - get - HTTP GET запрос
 * - post - HTTP POST запрос
 * - put - HTTP PUT запрос
 * - delete - HTTP DELETE запрос
 * - head - HTTP HEAD запрос
 * - search - поиск через поисковую систему
 */
class WebSearchAgent : public IAgent {
public:
    WebSearchAgent();
    ~WebSearchAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(AgentContext* context) override;
    AgentResult execute(const AgentRequest& request) override;
    void shutdown() override;
    AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    // Обработчики HTTP методов
    AgentResult handle_get(const AgentRequest& request);
    AgentResult handle_post(const AgentRequest& request);
    AgentResult handle_put(const AgentRequest& request);
    AgentResult handle_delete(const AgentRequest& request);
    AgentResult handle_head(const AgentRequest& request);
    AgentResult handle_search(const AgentRequest& request);

    /**
     * @brief Выполнение HTTP запроса
     */
    AgentResult perform_request(const std::string& method,
                                 const std::string& url,
                                 const std::string& body,
                                 const std::vector<std::string>& headers,
                                 int timeout_ms,
                                 const std::string& user_agent);

    AgentContext* context_ = nullptr;
    bool initialized_ = false;

    // Настройки по умолчанию
    int timeout_ms_ = 30000;
    int max_redirects_ = 5;
    std::string user_agent_ = "llama-gui-agent/1.0";
};

} // namespace agents

#pragma once

/**
 * @file web_render_agent.h
 * @brief Агент для рендеринга веб-страниц через headless-браузер (Chromium).
 *
 * ВАЖНО: это «headless-браузер» (рендеринг страниц), а НЕ «серверный режим
 * (без GUI)» самого приложения llama-gui (флаг --headless в src/main.cpp).
 */

#include <agents/agents.h>
#include <headless_browser/headless_browser.h>
#include <string>

namespace agents {

/**
 * @brief Агент для рендеринга веб-страниц через headless-браузер
 *
 * Поддерживаемые действия:
 * - render      - сериализовать отрендеренный DOM страницы (опция --dump-dom)
 * - screenshot  - сделать скриншот страницы в PNG-файл
 * - available   - проверить, доступен ли headless-браузер (Chromium)
 * - thin        - оценить, является ли HTML «пустой» JS-оболочкой (SPA)
 */
class WebRenderAgent : public IAgent {
public:
    WebRenderAgent();
    ~WebRenderAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(AgentContext* context) override;
    AgentResult execute(const AgentRequest& request) override;
    void shutdown() override;
    AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    AgentResult handle_render(const AgentRequest& request);
    AgentResult handle_screenshot(const AgentRequest& request);
    AgentResult handle_available(const AgentRequest& request);
    AgentResult handle_thin(const AgentRequest& request);

    // Собирает RenderOptions из параметров запроса (со значениями по умолчанию).
    headless_browser::RenderOptions build_options(const AgentRequest& request) const;

    AgentContext* context_ = nullptr;
    bool initialized_ = false;

    // Настройки по умолчанию (переопределяются через параметры запроса).
    std::string browser_path_ = "chromium";
    std::string user_agent_;
    int timeout_ms_ = 30000;
    int virtual_time_budget_ms_ = 15000;
};

} // namespace agents

/**
 * @file web_render_agent.cpp
 * @brief Агент для рендеринга веб-страниц через headless-браузер (Chromium).
 *
 * Обёртка над общей библиотекой libs/headless_browser. Использует Chromium
 * в headless-режиме (НЕ путать с «серверным режимом (без GUI)» приложения).
 */

#include "agents/web_render_agent.h"

#include <headless_browser/headless_browser.h>

namespace agents {

WebRenderAgent::WebRenderAgent() = default;
WebRenderAgent::~WebRenderAgent() = default;

const char* WebRenderAgent::name() const {
    return "web_render_agent";
}

const char* WebRenderAgent::description() const {
    return "Headless browser (Chromium) rendering agent. Renders web pages by "
           "executing JavaScript and serializing the resulting DOM (--dump-dom), "
           "or captures a PNG screenshot. Useful for SPA sites (e.g. VK.ru) that "
           "deliver content via JS rather than static HTML.";
}

const char* WebRenderAgent::version() const {
    return "1.0.0";
}

bool WebRenderAgent::initialize(AgentContext* context) {
    if (!context) return false;
    context_ = context;
    initialized_ = true;

    auto config = context_->get_agent_config(name());
    if (config.contains("browser_path")) {
        browser_path_ = config["browser_path"].get<std::string>();
    }
    if (config.contains("user_agent")) {
        user_agent_ = config["user_agent"].get<std::string>();
    }
    if (config.contains("timeout_ms")) {
        timeout_ms_ = config["timeout_ms"].get<int>();
    }
    if (config.contains("virtual_time_budget_ms")) {
        virtual_time_budget_ms_ = config["virtual_time_budget_ms"].get<int>();
    }

    context_->info(name(), "Initialized (browser_path=" + browser_path_ + ")");
    return true;
}

AgentResult WebRenderAgent::execute(const AgentRequest& request) {
    if (!initialized_) {
        return AgentResult::error("Agent not initialized");
    }
    std::string action = request.action();
    if (action == "render")        return handle_render(request);
    if (action == "screenshot")    return handle_screenshot(request);
    if (action == "available")     return handle_available(request);
    if (action == "thin")          return handle_thin(request);
    return AgentResult::error("Unknown action: " + action);
}

void WebRenderAgent::shutdown() {
    if (context_) context_->info(name(), "Shutting down");
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability WebRenderAgent::capabilities() const {
    return AgentCapability::WEB_RENDER |
           AgentCapability::HTTP_GET |
           AgentCapability::LONG_RUNNING |
           AgentCapability::LARGE_OUTPUT;
}

bool WebRenderAgent::is_ready() const {
    return initialized_;
}

// Собирает RenderOptions из параметров запроса (со значениями по умолчанию).
headless_browser::RenderOptions WebRenderAgent::build_options(
    const AgentRequest& request) const {
    headless_browser::RenderOptions opts;
    opts.browser_path = request.get_param<std::string>("browser_path", browser_path_);
    opts.user_agent   = request.get_param<std::string>("user_agent", user_agent_);
    opts.timeout_ms   = request.get_param<int>("timeout_ms", timeout_ms_);
    opts.virtual_time_budget_ms =
        request.get_param<int>("virtual_time_budget_ms", virtual_time_budget_ms_);
    return opts;
}

AgentResult WebRenderAgent::handle_render(const AgentRequest& request) {
    std::string url = request.get_param<std::string>("url", "");
    if (url.empty()) return AgentResult::error("URL is empty");

    auto opts = build_options(request);
    std::string error;
    std::string dom = headless_browser::render_dom(url, opts, &error);
    if (dom.empty()) {
        return AgentResult::error("Render failed: " + error);
    }

    nlohmann::json result;
    result["url"] = url;
    result["dom"] = dom;
    result["dom_length"] = static_cast<int>(dom.size());
    result["success"] = true;
    return AgentResult::success(result);
}

AgentResult WebRenderAgent::handle_screenshot(const AgentRequest& request) {
    std::string url = request.get_param<std::string>("url", "");
    std::string out_path = request.get_param<std::string>("out_path", "");
    if (url.empty())    return AgentResult::error("URL is empty");
    if (out_path.empty()) return AgentResult::error("out_path is empty");

    auto opts = build_options(request);
    if (request.has_param("window_width")) {
        opts.window_width = request.get_param<int>("window_width", opts.window_width);
    }
    if (request.has_param("window_height")) {
        opts.window_height = request.get_param<int>("window_height", opts.window_height);
    }

    std::string error;
    bool ok = headless_browser::screenshot(url, out_path, opts, &error);
    if (!ok) return AgentResult::error("Screenshot failed: " + error);

    nlohmann::json result;
    result["url"] = url;
    result["path"] = out_path;
    result["success"] = true;
    return AgentResult::success(result);
}

AgentResult WebRenderAgent::handle_available(const AgentRequest& request) {
    headless_browser::RenderOptions opts;
    opts.browser_path = request.get_param<std::string>("browser_path", browser_path_);
    const bool avail = headless_browser::available(opts);

    nlohmann::json result;
    result["browser_path"] = opts.browser_path;
    result["available"] = avail;
    return AgentResult::success(result);
}

AgentResult WebRenderAgent::handle_thin(const AgentRequest& request) {
    std::string html = request.get_param<std::string>("html", "");
    const std::size_t letters = headless_browser::visible_letter_count(html);
    const bool thin = headless_browser::is_thin_content(html);

    nlohmann::json result;
    result["visible_letters"] = static_cast<int>(letters);
    result["thin"] = thin;
    return AgentResult::success(result);
}

} // namespace agents



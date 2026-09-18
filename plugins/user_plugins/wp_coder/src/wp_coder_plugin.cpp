#include "wp_coder_orchestrator.h"
#include "wp_theme_agent.h"
#include "wp_plugin_agent.h"
#include "wp_hook_agent.h"
#include "wp_deploy_agent.h"
#include "wp_rag_agent.h"
#include "wp_terminal_agent.h"
#include "wp_file_agent.h"
#include <agents/agents.h>
#include <memory>

// C-API экспорт для плагина
extern "C" {

AGENT_PLUGIN_EXPORT const char* plugin_get_name() {
    return "wp_coder";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_version() {
    return "0.4.0";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_api_version() {
    return AGENT_PLUGIN_API_VERSION;
}

// Фабрика для создания оркестраторов и субагентов
AGENT_PLUGIN_EXPORT agents::IAgent* plugin_create_agent(const char* agent_name) {
    if (!agent_name) return nullptr;
    
    std::string name(agent_name);
    
    if (name == "wp_coder_orchestrator") {
        return new wp_coder::WPCoderOrchestrator();
    } else if (name == "wp_theme_agent") {
        return new wp_coder::WPThemeAgent();
    } else if (name == "wp_plugin_agent") {
        return new wp_coder::WPPluginAgent();
    } else if (name == "wp_hook_agent") {
        return new wp_coder::WPHookAgent();
    } else if (name == "wp_deploy_agent") {
        return new wp_coder::WPDeployAgent();
    } else if (name == "wp_rag_agent") {
        return new wp_coder::WPRagAgent();
    } else if (name == "wp_terminal_agent") {
        return new wp_coder::WPTerminalAgent();
    } else if (name == "wp_file_agent") {
        return new wp_coder::WPFileAgent();
    }
    
    return nullptr;
}

AGENT_PLUGIN_EXPORT void plugin_destroy_agent(agents::IAgent* agent) {
    delete agent;
}

AGENT_PLUGIN_EXPORT PluginExports* plugin_get_exports() {
    static PluginExports exports = {
        AGENT_PLUGIN_API_VERSION,
        plugin_get_name,
        plugin_get_version,
        plugin_get_api_version,
        reinterpret_cast<plugin_create_agent_fn>(plugin_create_agent),
        reinterpret_cast<plugin_destroy_agent_fn>(plugin_destroy_agent),
        nullptr,
        nullptr
    };
    return &exports;
}

} // extern "C"
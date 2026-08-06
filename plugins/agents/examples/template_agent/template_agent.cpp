/**
 * @file template_agent.cpp
 * @brief Реализация шаблона агента
 * 
 * Инструкция:
 * 1. Реализуйте свои действия в обработчиках
 * 2. Добавьте новые обработчики по необходимости
 * 3. Обновите C-API экспорт с вашим именем
 */

#include "template_agent.h"
#include <iostream>

namespace agents {

// ============================================================================
// TemplateAgent implementation
// ============================================================================

TemplateAgent::TemplateAgent() = default;

TemplateAgent::~TemplateAgent() {
    shutdown();
}

const char* TemplateAgent::name() const {
    return "template_agent";  // ИЗМЕНИТЬ: ваше имя агента
}

const char* TemplateAgent::description() const {
    return "Шаблон для создания нового агента. "
           "Скопируйте и модифицируйте этот файл для своего агента.";
}

const char* TemplateAgent::version() const {
    return "1.0.0";
}

bool TemplateAgent::initialize(AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Загрузка настроек из конфигурации
    auto config = context_->get_agent_config(name());
    if (config.contains("timeout_ms")) {
        timeout_ms_ = config["timeout_ms"].get<int>();
    }
    
    if (context_) {
        context_->info(name(), "Agent initialized");
    }
    
    return true;
}

AgentResult TemplateAgent::execute(const AgentRequest& request) {
    if (!initialized_) {
        return AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    // ========================================================================
    // ДОБАВЬТЕ СВОИ ДЕЙСТВИЯ ЗДЕСЬ
    // ========================================================================
    
    if (action == "default") {
        return handle_default(request);
    }
    
    // Примеры:
    // else if (action == "my_action") {
    //     return handle_my_action(request);
    // }
    // else if (action == "another_action") {
    //     return handle_another_action(request);
    // }
    
    return AgentResult::error("Unknown action: " + action);
}

void TemplateAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability TemplateAgent::capabilities() const {
    // ========================================================================
    // ВЕРНИТЕ НУЖНЫЕ ВОЗМОЖНОСТИ
    // ========================================================================
    return AgentCapability::NONE;
    
    // Примеры:
    // return AgentCapability::FILE_READ;
    // return AgentCapability::HTTP_GET | AgentCapability::HTTP_POST;
    // return AgentCapability::CODE_GENERATION | AgentCapability::CODE_ANALYSIS;
}

bool TemplateAgent::is_ready() const {
    return initialized_;
}

// ============================================================================
// Обработчики действий
// ============================================================================

AgentResult TemplateAgent::handle_default(const AgentRequest& request) {
    /**
     * @brief Обработчик действия по умолчанию
     * 
     * Замените эту функцию на свою логику
     */
    (void)request;  // unused
    
    return AgentResult::success({
        {"message", "Default action executed"},
        {"agent", name()},
        {"version", version()}
    });
}

// ============================================================================
// ПРИМЕРЫ ОБРАБОТЧИКОВ
// ============================================================================

/*
AgentResult TemplateAgent::handle_my_action(const AgentRequest& request) {
    // Пример: получение параметров
    std::string param1 = request.get_param<std::string>("param1", "default");
    int param2 = request.get_param<int>("param2", 0);
    
    // Ваша логика здесь
    // ...
    
    // Возврат результата
    return AgentResult::success({
        {"result", "success"},
        {"param1", param1},
        {"param2", param2}
    });
}

AgentResult TemplateAgent::handle_file_operation(const AgentRequest& request) {
    std::string file_path = request.file_path();
    
    // Проверка доступа через SecurityManager
    if (context_) {
        // auto security = context_->security();
        // auto result = security->check_file_access(name(), file_path, false);
        // if (!result.allowed) {
        //     return AgentResult::error(result.reason);
        // }
    }
    
    // Ваша логика работы с файлом
    // ...
    
    return AgentResult::success({{"file_path", file_path}});
}
*/

} // namespace agents

// ============================================================================
// C-API экспорт (ОБЯЗАТЕЛЬНО ДЛЯ ВСЕХ ПЛАГИНОВ)
// ============================================================================

extern "C" {

AGENT_PLUGIN_EXPORT const char* plugin_get_name() {
    return "template_agent";  // ИЗМЕНИТЬ: имя вашего плагина
}

AGENT_PLUGIN_EXPORT const char* plugin_get_version() {
    return "1.0.0";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_api_version() {
    return AGENT_PLUGIN_API_VERSION;
}

AGENT_PLUGIN_EXPORT agents::IAgent* plugin_create_agent() {
    return new agents::TemplateAgent();
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
        nullptr,  // initialize (вызывается через IAgent)
        nullptr   // shutdown (вызывается через IAgent)
    };
    return &exports;
}

} // extern "C"

/**
 * @file hello_agent.cpp
 * @brief Пример простейшего плагина агента
 * 
 * Этот плагин демонстрирует минимальную реализацию агента.
 * Он принимает запросы с действием "greet" и возвращает приветствие.
 */

#include <agents/agents.h>
#include <agents/plugin_c_api.h>
#include <string>
#include <sstream>

namespace {

/**
 * @brief Простейший агент для демонстрации API плагинов
 */
class HelloAgent : public agents::IAgent {
public:
    /**
     * @brief Уникальное имя агента
     */
    const char* name() const override {
        return "hello_agent";
    }

    /**
     * @brief Описание агента
     */
    const char* description() const override {
        return "Простой демонстрационный агент для тестирования системы плагинов";
    }

    /**
     * @brief Версия агента
     */
    const char* version() const override {
        return "1.0.0";
    }

    /**
     * @brief Инициализация агента
     * @param context Контекст выполнения
     * @return true если успешно
     */
    bool initialize(agents::AgentContext* context) override {
        context_ = context;
        
        if (context_) {
            context_->info(name(), "HelloAgent initialized");
        }
        
        return true;
    }

    /**
     * @brief Выполнение запроса
     * 
     * Поддерживаемые действия:
     * - "greet" - приветствие
     * - "echo" - эхо запроса
     * - "info" - информация об агенте
     * 
     * @param request Запрос
     * @return Результат выполнения
     */
    agents::AgentResult execute(const agents::AgentRequest& request) override {
        if (!context_) {
            return agents::AgentResult::error("Agent not initialized");
        }

        std::string action = request.action();

        if (context_) {
            context_->debug(name(), "Executing action: " + action);
        }

        if (action == "greet") {
            return handle_greet(request);
        } else if (action == "echo") {
            return handle_echo(request);
        } else if (action == "info") {
            return handle_info(request);
        } else if (action == "add") {
            return handle_add(request);
        }

        return agents::AgentResult::error("Unknown action: " + action);
    }

    /**
     * @brief Остановка агента
     */
    void shutdown() override {
        if (context_) {
            context_->info(name(), "HelloAgent shutdown");
        }
        context_ = nullptr;
    }

    /**
     * @brief Возможности агента
     * 
     * Этот агент не требует специальных возможностей,
     * поэтому возвращает NONE.
     */
    agents::AgentCapability capabilities() const override {
        return agents::AgentCapability::NONE;
    }

    /**
     * @brief Проверка готовности
     */
    bool is_ready() const override {
        return context_ != nullptr;
    }

private:
    /**
     * @brief Обработка действия "greet"
     */
    agents::AgentResult handle_greet(const agents::AgentRequest& request) {
        std::string name = request.get_param<std::string>("name", "World");
        int count = request.get_param<int>("count", 1);

        std::ostringstream greeting;
        greeting << "Hello, " << name << "!";

        if (count > 1) {
            greeting << " (repeated " << count << " times)";
        }

        agents::AgentResult result = agents::AgentResult::success({
            {"greeting", greeting.str()},
            {"name", name},
            {"count", count}
        });

        return result;
    }

    /**
     * @brief Обработка действия "echo"
     */
    agents::AgentResult handle_echo(const agents::AgentRequest& request) {
        return agents::AgentResult::success(request.params());
    }

    /**
     * @brief Обработка действия "info"
     */
    agents::AgentResult handle_info(const agents::AgentRequest& request) {
        (void)request;  // unused

        return agents::AgentResult::success({
            {"name", name()},
            {"description", description()},
            {"version", version()},
            {"api_version", agents::AGENT_API_VERSION}
        });
    }

    /**
     * @brief Обработка действия "add"
     * 
     * Демонстрация работы с числами
     */
    agents::AgentResult handle_add(const agents::AgentRequest& request) {
        int a = request.get_param<int>("a", 0);
        int b = request.get_param<int>("b", 0);

        return agents::AgentResult::success({
            {"result", a + b},
            {"expression", std::to_string(a) + " + " + std::to_string(b)}
        });
    }

    agents::AgentContext* context_ = nullptr;
};

} // anonymous namespace

// ============================================================================
// C-API экспорт для динамической загрузки
// ============================================================================

extern "C" {

/**
 * @brief Получить имя плагина
 */
AGENT_PLUGIN_EXPORT const char* plugin_get_name() {
    return "hello_agent";
}

/**
 * @brief Получить версию плагина
 */
AGENT_PLUGIN_EXPORT const char* plugin_get_version() {
    return "1.0.0";
}

/**
 * @brief Получить совместимую версию API
 */
AGENT_PLUGIN_EXPORT const char* plugin_get_api_version() {
    return AGENT_PLUGIN_API_VERSION;
}

/**
 * @brief Создать экземпляр агента
 * @return Указатель на агент
 */
AGENT_PLUGIN_EXPORT agents::IAgent* plugin_create_agent() {
    return new HelloAgent();
}

/**
 * @brief Уничтожить экземпляр агента
 * @param agent Указатель на агент
 */
AGENT_PLUGIN_EXPORT void plugin_destroy_agent(agents::IAgent* agent) {
    delete agent;
}

/**
 * @brief Получить экспортируемые функции плагина
 * @return Структура с функциями
 */
AGENT_PLUGIN_EXPORT PluginExports* plugin_get_exports() {
    static PluginExports exports = {
        AGENT_PLUGIN_API_VERSION,
        plugin_get_name,
        plugin_get_version,
        plugin_get_api_version,
        plugin_create_agent,
        plugin_destroy_agent,
        nullptr,  // initialize (вызывается через IAgent)
        nullptr   // shutdown (вызывается через IAgent)
    };
    return &exports;
}

} // extern "C"

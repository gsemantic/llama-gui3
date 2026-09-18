#include "wp_plugin_agent.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <core/openrouter_client.h>
#include <nlohmann/json.hpp>

namespace wp_coder {

// ===========================================================================
// WPPluginAgent implementation
// ===========================================================================

WPPluginAgent::WPPluginAgent() = default;

WPPluginAgent::~WPPluginAgent() {
    shutdown();
}

const char* WPPluginAgent::name() const {
    return "wp_plugin_agent";
}

const char* WPPluginAgent::description() const {
    return "WordPress Plugin agent. Generates plugins, scaffolds structure, adds shortcodes, REST endpoints, Gutenberg blocks.";
}

const char* WPPluginAgent::version() const {
    return "0.1.0";
}

bool WPPluginAgent::initialize(agents::AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Инициализация LLM клиента
    llm_client_ = std::make_unique<llama_gui::core::OpenRouterClient>();
    
    // Загрузка API ключа из конфигурации
    if (context_) {
        auto config = context_->get_agent_config(name());
        if (config.contains("api_key")) {
            llm_client_->set_api_key(config["api_key"].get<std::string>());
        }
        
        context_->info(name(), "Initialized with LLM client");
    }
    
    return true;
}

agents::AgentResult WPPluginAgent::execute(const agents::AgentRequest& request) {
    if (!initialized_) {
        return agents::AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "generate") {
        return handle_generate(request);
    } else if (action == "scaffold") {
        return handle_scaffold(request);
    } else if (action == "add_shortcode") {
        return handle_add_shortcode(request);
    } else if (action == "add_rest") {
        return handle_add_rest(request);
    } else if (action == "add_gutenberg") {
        return handle_add_gutenberg(request);
    }
    
    return agents::AgentResult::error("Unknown action: " + action);
}

void WPPluginAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    llm_client_.reset();
    context_ = nullptr;
}

agents::AgentCapability WPPluginAgent::capabilities() const {
    return agents::AgentCapability::CODE_GENERATION | 
           agents::AgentCapability::FILE_WRITE;
}

bool WPPluginAgent::is_ready() const {
    return initialized_ && llm_client_ != nullptr;
}

// ===========================================================================
// Обработчики действий
// ===========================================================================

agents::AgentResult WPPluginAgent::handle_generate(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Generating plugin via LLM");
    }
    
    if (!llm_client_) {
        return agents::AgentResult::error("LLM client not initialized");
    }
    
    std::string description = request.get_param<std::string>("description", "");
    if (description.empty()) {
        return agents::AgentResult::error("Description is empty");
    }
    
    // Формируем промпт для генерации плагина WordPress
    std::string system_prompt = R"(
Ты — эксперт по разработке плагинов WordPress. Сгенерируй полный код плагина на основе описания.
Включи:
1. Основной PHP-файл плагина с заголовком
2. Структуру папок (includes/, admin/, public/)
3. Класс основного плагина с хуками
4. Скрипты админки и фронтенда
5. README.txt
Используй современные практики WP, esc_html__ для перевода, wp_enqueue_script для скриптов.
)";
    
    std::string user_prompt = "Описание плагина: " + description + "\n\nСгенерируй полный код плагина WordPress. Раздели файлы комментариями вида: // FILE: filename.php";
    
    llama_gui::core::OpenRouterRequestParams params;
    params.model = "openai/gpt-4o";
    params.messages = {
        {{"role", "system"}, {"content", system_prompt}},
        {{"role", "user"}, {"content", user_prompt}}
    };
    params.temperature = 0.2;
    params.max_tokens = 4000;
    
    try {
        auto response = llm_client_->complete(params);
        
        if (!response.choices.empty()) {
            std::string generated_code = response.choices[0].message.content;
            
            // Парсим сгенерированный код по файлам
            nlohmann::json files = parse_generated_files(generated_code);
            
            return agents::AgentResult::success({
                {"plugin_code", generated_code},
                {"files", files},
                {"status", "success"},
                {"message", "Plugin generated successfully"}
            });
        } else {
            return agents::AgentResult::error("No response from LLM");
        }
    } catch (const std::exception& e) {
        if (context_) {
            context_->error(name(), "LLM error: " + std::string(e.what()));
        }
        return agents::AgentResult::error("LLM generation failed: " + std::string(e.what()));
    }
}

agents::AgentResult WPPluginAgent::handle_scaffold(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Scaffolding plugin structure");
    }
    
    std::string plugin_name = request.get_param<std::string>("plugin_name", "my-plugin");
    if (plugin_name.empty()) {
        return agents::AgentResult::error("Plugin name is empty");
    }
    
    // TODO: Реальное создание структуры файлов через wp_file_agent
    return agents::AgentResult::success({
        {"message", "Plugin scaffold not implemented yet"},
        {"status", "stub"}
    });
}

agents::AgentResult WPPluginAgent::handle_add_shortcode(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Adding shortcode to plugin");
    }
    // TODO: Добавление шорткода
    return agents::AgentResult::success({
        {"message", "Shortcode addition not implemented yet"},
        {"status", "stub"}
    });
}

agents::AgentResult WPPluginAgent::handle_add_rest(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Adding REST endpoint to plugin");
    }
    // TODO: Добавление REST endpoint
    return agents::AgentResult::success({
        {"message", "REST endpoint addition not implemented yet"},
        {"status", "stub"}
    });
}

agents::AgentResult WPPluginAgent::handle_add_gutenberg(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Preparing Gutenberg block");
    }
    // TODO: Подготовка Gutenberg-блока
    return agents::AgentResult::success({
        {"message", "Gutenberg block not implemented yet"},
        {"status", "stub"}
    });
}

// ===========================================================================
// Вспомогательные функции
// ===========================================================================

nlohmann::json WPPluginAgent::parse_generated_files(const std::string& generated_code) const {
    nlohmann::json files = nlohmann::json::object();
    
    // Простой парсинг по маркерам // FILE: filename
    std::istringstream iss(generated_code);
    std::string line;
    std::string current_file;
    std::string current_content;
    
    while (std::getline(iss, line)) {
        if (line.rfind("// FILE: ", 0) == 0) {
            // Сохраняем предыдущий файл
            if (!current_file.empty()) {
                files[current_file] = current_content;
            }
            
            // Начинаем новый файл
            current_file = line.substr(9); // длина "// FILE: "
            current_content.clear();
        } else {
            current_content += line + "\n";
        }
    }
    
    // Сохраняем последний файл
    if (!current_file.empty()) {
        files[current_file] = current_content;
    }
    
    return files;
}

} // namespace wp_coder
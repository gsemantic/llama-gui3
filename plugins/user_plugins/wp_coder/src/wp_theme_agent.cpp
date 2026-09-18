#include "wp_theme_agent.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <core/openrouter_client.h>
#include <core/embedding_server.h>
#include <nlohmann/json.hpp>

namespace wp_coder {

// ===========================================================================
// WPThemeAgent implementation
// ===========================================================================

WPThemeAgent::WPThemeAgent() = default;

WPThemeAgent::~WPThemeAgent() {
    shutdown();
}

const char* WPThemeAgent::name() const {
    return "wp_theme_agent";
}

const char* WPThemeAgent::description() const {
    return "WordPress Theme agent. Generates themes, scaffolds structure, adds hooks, prepares localization.";
}

const char* WPThemeAgent::version() const {
    return "0.1.0";
}

bool WPThemeAgent::initialize(agents::AgentContext* context) {
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

agents::AgentResult WPThemeAgent::execute(const agents::AgentRequest& request) {
    if (!initialized_) {
        return agents::AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "generate") {
        return handle_generate(request);
    } else if (action == "scaffold") {
        return handle_scaffold(request);
    } else if (action == "add_hooks") {
        return handle_add_hooks(request);
    } else if (action == "localize") {
        return handle_localize(request);
    }
    
    return agents::AgentResult::error("Unknown action: " + action);
}

void WPThemeAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    llm_client_.reset();
    context_ = nullptr;
}

agents::AgentCapability WPThemeAgent::capabilities() const {
    return agents::AgentCapability::CODE_GENERATION | 
           agents::AgentCapability::FILE_WRITE;
}

bool WPThemeAgent::is_ready() const {
    return initialized_ && llm_client_ != nullptr;
}

// ===========================================================================
// Обработчики действий
// ===========================================================================

agents::AgentResult WPThemeAgent::handle_generate(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Generating theme via LLM");
    }
    
    if (!llm_client_) {
        return agents::AgentResult::error("LLM client not initialized");
    }
    
    std::string description = request.get_param<std::string>("description", "");
    if (description.empty()) {
        return agents::AgentResult::error("Description is empty");
    }
    
    // Формируем промпт для генерации темы WordPress
    std::string system_prompt = R"(
Ты — эксперт по разработке тем WordPress. Сгенерируй полный код темы WordPress на основе описания.
Включи:
1. style.css с корректным заголовком темы
2. index.php с основным циклом
3. functions.php с базовыми хуками
4. screenshot.png (заглушка)
5. header.php и footer.php
Используй современные практики WP, esc_html__ для перевода, wp_enqueue_style для стилей.
)";
    
    std::string user_prompt = "Описание темы: " + description + "\n\nСгенерируй полный код темы WordPress. Раздели файлы комментариями вида: // FILE: filename.php";
    
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
                {"theme_code", generated_code},
                {"files", files},
                {"status", "success"},
                {"message", "Theme generated successfully"}
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

agents::AgentResult WPThemeAgent::handle_scaffold(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Scaffolding theme structure");
    }
    
    std::string theme_name = request.get_param<std::string>("theme_name", "my-theme");
    if (theme_name.empty()) {
        return agents::AgentResult::error("Theme name is empty");
    }
    
    // TODO: Реальное создание структуры файлов через wp_file_agent
    return agents::AgentResult::success({
        {"message", "Theme scaffold not implemented yet"},
        {"status", "stub"}
    });
}

agents::AgentResult WPThemeAgent::handle_add_hooks(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Adding hooks to theme");
    }
    // TODO: Добавление хуков
    return agents::AgentResult::success({
        {"message", "Hook addition not implemented yet"},
        {"status", "stub"}
    });
}

agents::AgentResult WPThemeAgent::handle_localize(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Preparing theme for localization");
    }
    // TODO: Локализация
    return agents::AgentResult::success({
        {"message", "Localization not implemented yet"},
        {"status", "stub"}
    });
}

// ===========================================================================
// Вспомогательные функции
// ===========================================================================

nlohmann::json WPThemeAgent::parse_generated_files(const std::string& generated_code) const {
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
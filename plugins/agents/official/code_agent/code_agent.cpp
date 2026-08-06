/**
 * @file code_agent.cpp
 * @brief Реализация агента для генерации и анализа кода
 */

#include "code_agent.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace agents {

// ============================================================================
// CodeAgent implementation
// ============================================================================

CodeAgent::CodeAgent() = default;

CodeAgent::~CodeAgent() {
    shutdown();
}

const char* CodeAgent::name() const {
    return "code_agent";
}

const char* CodeAgent::description() const {
    return "Code generation and analysis agent. Supports multiple programming "
           "languages including C++, Python, JavaScript, and more. Can generate, "
           "analyze, format, explain, and refactor code.";
}

const char* CodeAgent::version() const {
    return "1.0.0";
}

bool CodeAgent::initialize(AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Загрузка настроек
    auto config = context_->get_agent_config(name());
    if (config.contains("default_language")) {
        std::string lang = config["default_language"].get<std::string>();
        if (lang == "python") default_language_ = CodeLanguage::PYTHON;
        else if (lang == "javascript") default_language_ = CodeLanguage::JAVASCRIPT;
        else if (lang == "cpp") default_language_ = CodeLanguage::CPP;
    }
    if (config.contains("max_code_length")) {
        max_code_length_ = config["max_code_length"].get<size_t>();
    }
    
    context_->info(name(), "Initialized with default language: " + 
                   std::to_string(static_cast<int>(default_language_)));
    
    return true;
}

AgentResult CodeAgent::execute(const AgentRequest& request) {
    if (!initialized_) {
        return AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "generate") {
        return handle_generate(request);
    } else if (action == "analyze") {
        return handle_analyze(request);
    } else if (action == "format") {
        return handle_format(request);
    } else if (action == "explain") {
        return handle_explain(request);
    } else if (action == "refactor") {
        return handle_refactor(request);
    } else if (action == "translate") {
        return handle_translate(request);
    } else if (action == "lint") {
        return handle_lint(request);
    }
    
    return AgentResult::error("Unknown action: " + action);
}

void CodeAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down. Generated: " + 
                       std::to_string(generation_count_) + 
                       ", Analyzed: " + std::to_string(analysis_count_));
    }
    initialized_ = false;
    context_ = nullptr;
}

AgentCapability CodeAgent::capabilities() const {
    return AgentCapability::CODE_GENERATION | 
           AgentCapability::CODE_ANALYSIS;
}

bool CodeAgent::is_ready() const {
    return initialized_;
}

// ============================================================================
// Обработчики действий
// ============================================================================

AgentResult CodeAgent::handle_generate(const AgentRequest& request) {
    std::string prompt = request.get_param<std::string>("prompt", "");
    std::string language_str = request.get_param<std::string>("language", "cpp");
    
    if (prompt.empty()) {
        return AgentResult::error("Prompt is empty");
    }
    
    // Определение языка
    CodeLanguage lang = default_language_;
    if (language_str == "python") lang = CodeLanguage::PYTHON;
    else if (language_str == "javascript") lang = CodeLanguage::JAVASCRIPT;
    else if (language_str == "typescript") lang = CodeLanguage::TYPESCRIPT;
    else if (language_str == "java") lang = CodeLanguage::JAVA;
    else if (language_str == "rust") lang = CodeLanguage::RUST;
    else if (language_str == "go") lang = CodeLanguage::GO;
    
    if (context_) {
        context_->info(name(), "Generating " + language_str + " code");
    }
    
    // Генерация через LLM (заглушка - требует интеграции)
    // В реальной реализации здесь будет вызов LLM
    std::ostringstream generated_code;
    generated_code << "// Generated " << language_str << " code\n";
    generated_code << "// Prompt: " << prompt << "\n\n";
    
    if (lang == CodeLanguage::PYTHON) {
        generated_code << "def solution():\n";
        generated_code << "    # TODO: Implement your logic here\n";
        generated_code << "    pass\n";
    } else if (lang == CodeLanguage::CPP) {
        generated_code << "#include <iostream>\n\n";
        generated_code << "int main() {\n";
        generated_code << "    // TODO: Implement your logic here\n";
        generated_code << "    return 0;\n";
        generated_code << "}\n";
    } else {
        generated_code << "// TODO: Implement your solution\n";
    }
    
    generation_count_++;
    
    return AgentResult::success({
        {"code", generated_code.str()},
        {"language", language_str},
        {"lines", static_cast<int>(std::count(generated_code.str().begin(), 
                                               generated_code.str().end(), '\n'))},
        {"generated", true}
    });
}

AgentResult CodeAgent::handle_analyze(const AgentRequest& request) {
    std::string code = request.get_param<std::string>("code", "");
    std::string language_str = request.get_param<std::string>("language", "");
    
    if (code.empty()) {
        return AgentResult::error("Code is empty");
    }
    
    if (code.length() > max_code_length_) {
        return AgentResult::error("Code too long (max " + 
                                  std::to_string(max_code_length_) + " chars)");
    }
    
    // Анализ кода
    int lines = std::count(code.begin(), code.end(), '\n') + 1;
    int non_empty_lines = 0;
    int comment_lines = 0;
    
    std::istringstream iss(code);
    std::string line;
    while (std::getline(iss, line)) {
        // Пропуск пустых строк
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }
        non_empty_lines++;
        
        // Подсчёт комментариев
        size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) {
            if (line.substr(first, 2) == "//" || 
                line.substr(first, 2) == "/*" ||
                line.substr(first, 1) == "#") {
                comment_lines++;
            }
        }
    }
    
    analysis_count_++;
    
    nlohmann::json analysis;
    analysis["total_lines"] = lines;
    analysis["non_empty_lines"] = non_empty_lines;
    analysis["comment_lines"] = comment_lines;
    analysis["code_lines"] = non_empty_lines - comment_lines;
    analysis["character_count"] = static_cast<int>(code.length());
    
    // Простая оценка сложности
    int complexity = 1;
    complexity += std::count(code.begin(), code.end(), 'i') +  // if
                  std::count(code.begin(), code.end(), 'f') +  // for
                  std::count(code.begin(), code.end(), 'w') +  // while
                  std::count(code.begin(), code.end(), '?');   // ternary
    
    analysis["estimated_complexity"] = complexity;
    
    return AgentResult::success(analysis);
}

AgentResult CodeAgent::handle_format(const AgentRequest& request) {
    std::string code = request.get_param<std::string>("code", "");
    
    if (code.empty()) {
        return AgentResult::error("Code is empty");
    }
    
    // Простое форматирование (в реальности нужен clang-format или аналог)
    std::string formatted = code;
    
    // Удаление trailing whitespace
    std::istringstream iss(code);
    std::ostringstream oss;
    std::string line;
    while (std::getline(iss, line)) {
        // Удаляем пробелы в конце строки
        size_t end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) {
            oss << line.substr(0, end + 1) << "\n";
        } else {
            oss << "\n";
        }
    }
    formatted = oss.str();
    
    return AgentResult::success({
        {"code", formatted},
        {"formatted", true}
    });
}

AgentResult CodeAgent::handle_explain(const AgentRequest& request) {
    std::string code = request.get_param<std::string>("code", "");
    std::string language = request.get_param<std::string>("language", "");
    
    if (code.empty()) {
        return AgentResult::error("Code is empty");
    }
    
    // Объяснение кода (заглушка - требует LLM)
    std::ostringstream explanation;
    explanation << "Code Explanation:\n\n";
    explanation << "Language: " << (language.empty() ? "Unknown" : language) << "\n";
    explanation << "Lines: " << (std::count(code.begin(), code.end(), '\n') + 1) << "\n\n";
    explanation << "This code appears to be a " << language << " program.\n";
    explanation << "To get a detailed explanation, please use an LLM-powered agent.\n";
    
    return AgentResult::success({
        {"explanation", explanation.str()},
        {"language", language}
    });
}

AgentResult CodeAgent::handle_refactor(const AgentRequest& request) {
    std::string code = request.get_param<std::string>("code", "");
    std::string style = request.get_param<std::string>("style", "default");
    
    if (code.empty()) {
        return AgentResult::error("Code is empty");
    }
    
    // Рефакторинг (заглушка)
    std::string refactored = code;
    
    return AgentResult::success({
        {"code", refactored},
        {"refactored", true},
        {"style", style},
        {"message", "Refactoring requires LLM integration"}
    });
}

AgentResult CodeAgent::handle_translate(const AgentRequest& request) {
    std::string code = request.get_param<std::string>("code", "");
    std::string from_lang = request.get_param<std::string>("from", "");
    std::string to_lang = request.get_param<std::string>("to", "");
    
    if (code.empty()) {
        return AgentResult::error("Code is empty");
    }
    if (to_lang.empty()) {
        return AgentResult::error("Target language is empty");
    }
    
    // Перевод между языками (заглушка - требует LLM)
    std::ostringstream translated;
    translated << "// Translated from " << from_lang << " to " << to_lang << "\n";
    translated << "// Original code:\n";
    translated << code << "\n\n";
    translated << "// Translation requires LLM integration\n";
    
    return AgentResult::success({
        {"code", translated.str()},
        {"from_language", from_lang},
        {"to_language", to_lang}
    });
}

AgentResult CodeAgent::handle_lint(const AgentRequest& request) {
    std::string code = request.get_param<std::string>("code", "");
    std::string language = request.get_param<std::string>("language", "");
    
    if (code.empty()) {
        return AgentResult::error("Code is empty");
    }
    
    nlohmann::json issues = nlohmann::json::array();
    
    // Простая линтинг-проверка
    std::istringstream iss(code);
    std::string line;
    int line_num = 0;
    
    while (std::getline(iss, line)) {
        line_num++;
        
        // Проверка на длинные строки
        if (line.length() > 120) {
            nlohmann::json issue;
            issue["line"] = line_num;
            issue["severity"] = "warning";
            issue["message"] = "Line exceeds 120 characters";
            issue["rule"] = "max-line-length";
            issues.push_back(issue);
        }
        
        // Проверка на trailing whitespace
        if (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            nlohmann::json issue;
            issue["line"] = line_num;
            issue["severity"] = "info";
            issue["message"] = "Trailing whitespace";
            issue["rule"] = "no-trailing-whitespace";
            issues.push_back(issue);
        }
    }
    
    return AgentResult::success({
        {"issues", issues},
        {"issue_count", static_cast<int>(issues.size())},
        {"clean", issues.empty()}
    });
}

// ============================================================================
// Вспомогательные функции
// ============================================================================

CodeLanguage CodeAgent::detect_language(const std::string& filename) const {
    size_t dot_pos = filename.rfind('.');
    if (dot_pos == std::string::npos) {
        return CodeLanguage::UNKNOWN;
    }
    
    std::string ext = filename.substr(dot_pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".h" || ext == ".hpp") {
        return CodeLanguage::CPP;
    } else if (ext == ".py") {
        return CodeLanguage::PYTHON;
    } else if (ext == ".js" || ext == ".jsx") {
        return CodeLanguage::JAVASCRIPT;
    } else if (ext == ".ts" || ext == ".tsx") {
        return CodeLanguage::TYPESCRIPT;
    } else if (ext == ".java") {
        return CodeLanguage::JAVA;
    } else if (ext == ".cs") {
        return CodeLanguage::CSHARP;
    } else if (ext == ".go") {
        return CodeLanguage::GO;
    } else if (ext == ".rs") {
        return CodeLanguage::RUST;
    } else if (ext == ".rb") {
        return CodeLanguage::RUBY;
    } else if (ext == ".php") {
        return CodeLanguage::PHP;
    } else if (ext == ".swift") {
        return CodeLanguage::SWIFT;
    } else if (ext == ".kt") {
        return CodeLanguage::KOTLIN;
    }
    
    return CodeLanguage::UNKNOWN;
}

CodeLanguage CodeAgent::detect_language_by_content(const std::string& content) const {
    // Простая эвристика по ключевым словам
    if (content.find("#include") != std::string::npos ||
        content.find("std::") != std::string::npos ||
        content.find("cout") != std::string::npos) {
        return CodeLanguage::CPP;
    }
    
    if (content.find("import ") != std::string::npos ||
        content.find("def ") != std::string::npos ||
        content.find("print(") != std::string::npos) {
        return CodeLanguage::PYTHON;
    }
    
    if (content.find("function ") != std::string::npos ||
        content.find("console.log") != std::string::npos ||
        content.find("const ") != std::string::npos) {
        return CodeLanguage::JAVASCRIPT;
    }
    
    if (content.find("public static void main") != std::string::npos ||
        content.find("System.out.println") != std::string::npos) {
        return CodeLanguage::JAVA;
    }
    
    return CodeLanguage::UNKNOWN;
}

std::string CodeAgent::get_extension_for_language(CodeLanguage lang) const {
    switch (lang) {
        case CodeLanguage::CPP: return ".cpp";
        case CodeLanguage::PYTHON: return ".py";
        case CodeLanguage::JAVASCRIPT: return ".js";
        case CodeLanguage::TYPESCRIPT: return ".ts";
        case CodeLanguage::JAVA: return ".java";
        case CodeLanguage::CSHARP: return ".cs";
        case CodeLanguage::GO: return ".go";
        case CodeLanguage::RUST: return ".rs";
        case CodeLanguage::RUBY: return ".rb";
        case CodeLanguage::PHP: return ".php";
        case CodeLanguage::SWIFT: return ".swift";
        case CodeLanguage::KOTLIN: return ".kt";
        default: return "";
    }
}

AgentResult CodeAgent::generate_via_llm(const std::string& prompt,
                                         CodeLanguage language) {
    // Заглушка для будущей LLM интеграции
    (void)prompt;
    (void)language;
    
    return AgentResult::error("LLM integration not implemented");
}

} // namespace agents

// ============================================================================
// C-API экспорт
// ============================================================================

extern "C" {

AGENT_PLUGIN_EXPORT const char* plugin_get_name() {
    return "code_agent";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_version() {
    return "1.0.0";
}

AGENT_PLUGIN_EXPORT const char* plugin_get_api_version() {
    return AGENT_PLUGIN_API_VERSION;
}

AGENT_PLUGIN_EXPORT agents::IAgent* plugin_create_agent() {
    return new agents::CodeAgent();
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

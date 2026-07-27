#pragma once

#include <string>

namespace llama_gui {
namespace core {

/**
 * @brief Специальные значения для chat_template
 */
struct ChatTemplateValues {
    static constexpr const char* AUTO = "auto";              // Автоматически из GGUF
    static constexpr const char* NONE = "none";              // Отключить шаблон
    static constexpr const char* FILE_PREFIX = "file:";      // Префикс для пути к файлу
};

/**
 * @brief Настройки грамматики (Grammar) для llama.cpp
 *
 * Включает все параметры грамматики и шаблонов:
 * - Grammar (грамматика)
 * - JSON Schema (JSON схема)
 * - Chat template (шаблон чата)
 * - System prompt (системный промпт)
 * - Reasoning (формат рассуждений)
 */
struct GrammarSettings {
    // =========================================================================
    // Grammar (грамматика)
    // =========================================================================
    
    /// Правила грамматики (--grammar)
    std::string grammar = "";
    
    /// Файл грамматики (--grammar-file)
    std::string grammar_file = "";

    // =========================================================================
    // JSON Schema (JSON схема)
    // =========================================================================
    
    /// JSON схема (-j, --json-schema)
    std::string json_schema = "";
    
    /// Файл JSON схемы (-jf, --json-schema-file)
    std::string json_schema_file = "";

    // =========================================================================
    // Chat template (шаблон чата)
    // =========================================================================

    /// Шаблон чата (--chat-template)
    /// Специальные значения:
    /// - "auto" = автоматически извлечь из GGUF файла модели
    /// - "none" = отключить шаблон
    /// - "file:/path/to/template.jinja" = загрузить из файла
    /// - "llama-3", "mistral", etc. = встроенный шаблон
    /// - Содержит "{{" = inline-шаблон
    std::string chat_template = ChatTemplateValues::AUTO;

    /// Файл шаблона чата (--chat-template-file)
    std::string chat_template_file = "";
    
    /// Параметры шаблона чата (--chat-template-kwargs)
    std::string chat_template_kwargs = "";
    
    /// Использовать Jinja (--jinja)
    bool use_jinja = false;
    
    /// Не заполнять assistant (--no-prefill-assistant)
    bool no_prefill_assistant = false;

    // =========================================================================
    // System prompt (системный промпт)
    // =========================================================================
    
    /// Файл системного промпта (-spf, --system-prompt-file)
    std::string system_prompt_file = "";
    
    /// Системный промпт по умолчанию
    std::string default_system_prompt = "You are a helpful assistant.";

    // =========================================================================
    // Reasoning (формат рассуждений)
    // =========================================================================
    
    /// Формат рассуждений
    enum class ReasoningFormat {
        None,     /// Без формата
        Deepseek  /// Формат Deepseek
    };
    
    /// Формат рассуждений (--reasoning-format)
    ReasoningFormat reasoning_format = ReasoningFormat::None;
    
    /// Бюджет рассуждений (--reasoning-budget), -1 = неограничен
    int reasoning_budget = -1;

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        if (reasoning_budget < -1) return false;
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;
        
        if (reasoning_budget < -1) {
            errors += "Reasoning budget must be -1 or greater. ";
        }
        
        return errors;
    }

    // =========================================================================
    // Helper methods (вспомогательные методы)
    // =========================================================================
    
    /**
     * @brief Проверка наличия грамматики
     * @return true если задана грамматика или файл грамматики
     */
    bool has_grammar() const {
        return !grammar.empty() || !grammar_file.empty();
    }

    /**
     * @brief Проверка наличия JSON схемы
     * @return true если задана JSON схема или файл схемы
     */
    bool has_json_schema() const {
        return !json_schema.empty() || !json_schema_file.empty();
    }

    /**
     * @brief Проверка наличия шаблона чата
     * @return true если задан шаблон чата или файл шаблона
     */
    bool has_chat_template() const {
        return !chat_template.empty() || !chat_template_file.empty();
    }

    /**
     * @brief Проверка наличия системного промпта
     * @return true если задан системный промпт или файл промпта
     */
    bool has_system_prompt() const {
        return !system_prompt_file.empty() || !default_system_prompt.empty();
    }

    /**
     * @brief Получить строковое представление формата рассуждений
     * @return "none" или "deepseek"
     */
    std::string get_reasoning_format_string() const {
        return reasoning_format == ReasoningFormat::Deepseek ? "deepseek" : "none";
    }

    /**
     * @brief Установить формат рассуждений из строки
     * @param format Строка "none" или "deepseek"
     */
    void set_reasoning_format(const std::string& format) {
        if (format == "deepseek") {
            reasoning_format = ReasoningFormat::Deepseek;
            // Automatically enable unlimited thinking when reasoning is enabled
            if (reasoning_budget == 0) {
                reasoning_budget = -1;  // Unlimited thinking budget
            }
        } else {
            reasoning_format = ReasoningFormat::None;
            // Automatically disable thinking when reasoning is disabled
            reasoning_budget = 0;
        }
    }

    /**
     * @brief Проверка использования рассуждений
     * @return true если используется формат рассуждений
     */
    bool uses_reasoning() const {
        return reasoning_format != ReasoningFormat::None;
    }

    // =========================================================================
    // Helper methods для chat_template
    // =========================================================================

    /**
     * @brief Проверка, используется ли авто-определение шаблона
     * @return true если chat_template == "auto"
     */
    bool is_chat_template_auto() const {
        return chat_template == ChatTemplateValues::AUTO || 
               chat_template == "Auto" || 
               chat_template == "AUTO";
    }

    /**
     * @brief Проверка, отключен ли шаблон
     * @return true если chat_template == "none"
     */
    bool is_chat_template_none() const {
        return chat_template == ChatTemplateValues::NONE ||
               chat_template == "None" ||
               chat_template == "NONE" ||
               chat_template == "disabled";
    }

    /**
     * @brief Проверка, является ли chat_template путем к файлу
     * @return true если начинается с "file:" или содержит .jinja/.j2
     */
    bool is_chat_template_file_path() const {
        if (chat_template.rfind(ChatTemplateValues::FILE_PREFIX, 0) == 0) {
            return true;
        }
        if (chat_template.find(".jinja") != std::string::npos || 
            chat_template.find(".j2") != std::string::npos) {
            return chat_template[0] == '/' || 
                   chat_template[0] == '.' || 
                   chat_template.find('/') != std::string::npos;
        }
        return false;
    }

    /**
     * @brief Получить путь к файлу шаблона из chat_template
     * @return Путь к файлу или пустая строка
     */
    std::string get_chat_template_file_path() const {
        if (chat_template.rfind(ChatTemplateValues::FILE_PREFIX, 0) == 0) {
            return chat_template.substr(std::strlen(ChatTemplateValues::FILE_PREFIX));
        }
        if (is_chat_template_file_path()) {
            return chat_template;
        }
        return "";
    }

    /**
     * @brief Проверка, является ли chat_template встроенным шаблоном
     * @return true если совпадает с известным именем встроенного шаблона
     */
    bool is_chat_template_builtin() const {
        static const std::vector<std::string> builtin_names = {
            "llama-2", "llama-3", "llama-3.1",
            "mistral", "mistral-instruct",
            "qwen-2",
            "gemma",
            "phi-3", "phi",
            "chatglm3",
            "yi",
            "openchat",
            "vicuna"
        };
        for (const auto& name : builtin_names) {
            if (chat_template == name) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Проверка, является ли chat_template inline-шаблоном
     * @return true если содержит "{{"
     */
    bool is_chat_template_inline() const {
        return chat_template.find("{{") != std::string::npos;
    }
};

} // namespace core
} // namespace llama_gui

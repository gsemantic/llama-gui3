#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <mutex>

namespace llama_gui {
namespace core {

/**
 * @brief Результат извлечения chat template
 */
struct ChatTemplateResult {
    std::string template_str;
    std::string source;          // "gguf", "file", "builtin", "inline", "none"
    std::string model_name;      // имя модели (если из GGUF)
    bool success = false;
    std::string error_message;

    ChatTemplateResult() = default;
    ChatTemplateResult(const std::string& tmpl, const std::string& src)
        : template_str(tmpl), source(src), success(true) {}
    
    static ChatTemplateResult error(const std::string& msg) {
        ChatTemplateResult result;
        result.error_message = msg;
        return result;
    }
};

/**
 * @brief Менеджер для управления chat templates
 * 
 * Поддерживает:
 * - Авто-извлечение из GGUF файла модели
 * - Загрузку из внешних .jinja файлов
 * - Встроенные шаблоны для популярных моделей
 * - Кэширование результатов
 */
class ChatTemplateManager {
public:
    static ChatTemplateManager& instance();

    // Запрет копирования
    ChatTemplateManager(const ChatTemplateManager&) = delete;
    ChatTemplateManager& operator=(const ChatTemplateManager&) = delete;

    /**
     * @brief Извлечь chat template из GGUF файла модели
     * @param model_path Путь к .gguf файлу
     * @return ChatTemplateResult с шаблоном или ошибкой
     */
    ChatTemplateResult extract_from_gguf(const std::string& model_path);

    /**
     * @brief Загрузить шаблон из файла
     * @param template_path Путь к .jinja файлу
     * @return ChatTemplateResult с шаблоном или ошибкой
     */
    ChatTemplateResult load_from_file(const std::string& template_path);

    /**
     * @brief Получить встроенный шаблон по названию
     * @param name Название шаблона (llama-2, llama-3, mistral, qwen-2, gemma, phi-3, chatglm3)
     * @return ChatTemplateResult с шаблоном или ошибкой
     */
    ChatTemplateResult get_builtin_template(const std::string& name);

    /**
     * @brief Определить шаблон по имени модели (эвристика)
     * @param model_name Имя модели (например, "llama-3-8b-instruct")
     * @return Название встроенного шаблона или пустая строка
     */
    std::string detect_from_model_name(const std::string& model_name);

    /**
     * @brief Получить или извлечь шаблон с учётом режима
     * @param model_path Путь к модели (для auto/GGUF)
     * @param mode Режим: "auto", "none", "file:/path", название встроенного, inline-шаблон
     * @return ChatTemplateResult с шаблоном или ошибкой
     */
    ChatTemplateResult get_or_extract_template(const std::string& model_path, const std::string& mode);

    /**
     * @brief Получить список доступных встроенных шаблонов
     * @return Вектор с названиями встроенных шаблонов
     */
    std::vector<std::string> get_builtin_template_names() const;

    /**
     * @brief Очистить кэш шаблонов
     */
    void clear_cache();

    /**
     * @brief Проверить, является ли значение специальным (auto/none/file:)
     */
    static bool is_special_value(const std::string& value);
    static bool is_auto(const std::string& value);
    static bool is_none(const std::string& value);
    static bool is_file_path(const std::string& value);
    static std::string get_file_path(const std::string& value);

private:
    ChatTemplateManager() = default;
    ~ChatTemplateManager() = default;

    // Кэш шаблонов: путь к модели -> template
    std::unordered_map<std::string, ChatTemplateResult> gguf_cache_;
    
    // Кэш файлов: путь к файлу -> template
    std::unordered_map<std::string, ChatTemplateResult> file_cache_;

    // Мьютекс для потокобезопасности
    mutable std::mutex mutex_;

    // Встроенные шаблоны
    static const std::unordered_map<std::string, std::string>& get_builtin_templates_map();
};

} // namespace core
} // namespace llama_gui

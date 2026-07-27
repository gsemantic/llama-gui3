#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace llama_gui {
namespace core {

class LlamaInterface;
class RagManager;

/**
 * @brief Результат суммаризации
 */
struct CompressionResult {
    std::string summary;           // Текст суммаризации
    int original_tokens = 0;       // Токены в оригинальных сообщениях
    int compressed_tokens = 0;     // Токены в суммаризации
    double compression_ratio = 0.0; // Коэффициент сжатия
    bool success = false;          // Успешно ли выполнена суммаризация
};

/**
 * @brief Компрессор контекста
 * 
 * Суммаризирует старые сообщения диалога через LLM.
 * Результат индексируется в RAG для последующего поиска.
 */
class ContextCompressor {
public:
    ContextCompressor();
    ~ContextCompressor();

    /**
     * @brief Установить интерфейс к LLM
     */
    void set_llama_interface(LlamaInterface* llama_interface) {
        llama_interface_ = llama_interface;
    }

    /**
     * @brief Установить RAG менеджер для индексации суммаризаций
     */
    void set_rag_manager(RagManager* rag_manager) {
        rag_manager_ = rag_manager;
    }

    /**
     * @brief Суммаризовать группу сообщений
     * @param messages Сообщения для суммаризации (пары role:content)
     * @param conversation_id ID диалога (для индексации в RAG)
     * @param query_hint Подсказка для фокусировки суммаризации
     * @return Результат суммаризации
     */
    CompressionResult summarize_messages(
        const std::vector<std::pair<std::string, std::string>>& messages,
        const std::string& conversation_id = "",
        const std::string& query_hint = "");

    /**
     * @brief Суммаризовать и индексировать в RAG
     * @param messages Сообщения для суммаризации
     * @param conversation_id ID диалога
     * @return Результат суммаризации
     */
    CompressionResult summarize_and_index(
        const std::vector<std::pair<std::string, std::string>>& messages,
        const std::string& conversation_id);

    /**
     * @brief Проверить, доступна ли суммаризация
     */
    bool is_available() const { return llama_interface_ != nullptr; }

private:
    LlamaInterface* llama_interface_ = nullptr;
    RagManager* rag_manager_ = nullptr;

    /**
     * @brief Построить промпт для суммаризации
     */
    std::string build_summary_prompt(
        const std::vector<std::pair<std::string, std::string>>& messages,
        const std::string& query_hint = "");

    /**
     * @brief Извлечь суммаризацию из ответа LLM
     */
    std::string extract_summary(const std::string& llm_response);

    /**
     * @brief Рассчитать коэффициент сжатия
     */
    double calculate_compression_ratio(int original_tokens, int compressed_tokens);
};

} // namespace core
} // namespace llama_gui

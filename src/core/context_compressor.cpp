#include "context_compressor.h"
#include "llama_interface.h"
#include "rag_manager.h"
#include "context_monitor.h"
#include <iostream>
#include <sstream>
#include <future>

namespace llama_gui {
namespace core {

ContextCompressor::ContextCompressor() = default;
ContextCompressor::~ContextCompressor() = default;

CompressionResult ContextCompressor::summarize_messages(
    const std::vector<std::pair<std::string, std::string>>& messages,
    const std::string& conversation_id,
    const std::string& query_hint) {
    
    CompressionResult result;
    
    if (!llama_interface_ || messages.empty()) {
        return result;
    }

    // Подсчитываем токены в оригинальных сообщениях
    int original_tokens = 0;
    for (const auto& [role, content] : messages) {
        original_tokens += ContextMonitor::estimate_tokens(role + ": " + content);
    }
    result.original_tokens = original_tokens;

    // Строим промпт для суммаризации
    std::string prompt = build_summary_prompt(messages, query_hint);

    // Отправляем в LLM
    ChatCompletionRequest request;
    request.model = "summarizer";
    request.max_tokens = 512;
    request.temperature = 0.3f;  // Низкая температура для точной суммаризации
    request.stream = false;

    // Системный промпт
    request.messages.push_back(ChatMessage(
        MessageRole::System,
        "Ты - помощник для суммаризации диалогов. "
        "Создай краткое резюме ключевых моментов разговора. "
        "Сохраняй важные факты, решения и контекст. "
        "Отвечай на русском языке."
    ));

    // Пользовательский промпт с диалогом
    request.messages.push_back(ChatMessage(MessageRole::User, prompt));

    try {
        auto future = llama_interface_->create_chat_completion_async(request);
        auto response = future.get();

        if (!response.choices.empty()) {
            std::string summary = extract_summary(response.choices[0].message.content);
            
            if (!summary.empty()) {
                result.summary = summary;
                result.compressed_tokens = ContextMonitor::estimate_tokens(summary);
                result.compression_ratio = calculate_compression_ratio(
                    original_tokens, result.compressed_tokens);
                result.success = true;

                std::cout << "[CONTEXT COMPRESSOR] Summarized " << messages.size() 
                          << " messages: " << original_tokens << " → " 
                          << result.compressed_tokens << " tokens (ratio: " 
                          << result.compression_ratio << ")" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[CONTEXT COMPRESSOR] Error: " << e.what() << std::endl;
    }

    return result;
}

CompressionResult ContextCompressor::summarize_and_index(
    const std::vector<std::pair<std::string, std::string>>& messages,
    const std::string& conversation_id) {
    
    // Сначала суммаризуем
    CompressionResult result = summarize_messages(messages, conversation_id);
    
    // Если успешно и есть RAG менеджер — индексируем
    if (result.success && rag_manager_ && !result.summary.empty()) {
        // Создаём уникальный ID для суммаризации
        std::string doc_id = "summary_" + conversation_id + "_" + 
                            std::to_string(std::time(nullptr));
        
        // Индексируем как документ в RAG
        // Используем process_text_chunk для добавления в индекс
        bool indexed = rag_manager_->process_text_chunk(
            result.summary, 
            doc_id, 
            0  // chunk_index
        );

        if (indexed) {
            std::cout << "[CONTEXT COMPRESSOR] Indexed summary in RAG: " << doc_id << std::endl;
        } else {
            std::cerr << "[CONTEXT COMPRESSOR] Failed to index summary in RAG" << std::endl;
        }
    }

    return result;
}

std::string ContextCompressor::build_summary_prompt(
    const std::vector<std::pair<std::string, std::string>>& messages,
    const std::string& query_hint) {
    
    std::ostringstream prompt;
    
    prompt << "Пожалуйста, создай краткое резюме следующего диалога:\n\n";
    
    for (const auto& [role, content] : messages) {
        // Ограничиваем длину каждого сообщения
        std::string truncated = content;
        if (truncated.size() > 500) {
            truncated = truncated.substr(0, 500) + "...";
        }
        prompt << role << ": " << truncated << "\n\n";
    }
    
    if (!query_hint.empty()) {
        prompt << "\nФокус резюме: " << query_hint << "\n";
    }
    
    prompt << "\nСохрани ключевые факты, решения и контекст. ";
    prompt << "Отвечай кратко (100-200 слов).";
    
    return prompt.str();
}

std::string ContextCompressor::extract_summary(const std::string& llm_response) {
    // Простое извлечение - убираем маркеры если есть
    std::string summary = llm_response;
    
    // Убираем маркеры суммаризации
    size_t start = summary.find("Резюме:");
    if (start == std::string::npos) {
        start = summary.find("Summary:");
    }
    if (start != std::string::npos) {
        summary = summary.substr(start + 8);
    }
    
    // Убираем ведущие/замыкающие пробелы
    size_t first_non_space = summary.find_first_not_of(" \t\n\r");
    if (first_non_space != std::string::npos) {
        summary = summary.substr(first_non_space);
    }
    
    size_t last_non_space = summary.find_last_not_of(" \t\n\r");
    if (last_non_space != std::string::npos) {
        summary = summary.substr(0, last_non_space + 1);
    }
    
    return summary;
}

double ContextCompressor::calculate_compression_ratio(int original_tokens, int compressed_tokens) {
    if (original_tokens <= 0) return 0.0;
    return static_cast<double>(compressed_tokens) / original_tokens;
}

} // namespace core
} // namespace llama_gui

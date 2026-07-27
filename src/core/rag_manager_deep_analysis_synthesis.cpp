#include "../include/core/rag_manager.h"
#include "../include/core/rag_deep_analysis_utils.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace llama_gui {
namespace core {

using namespace deep_analysis;

// ============================================================================
// ГЕНЕРАЦИЯ РЕЗЮМЕ ДЛЯ ЧАНКА (MAP-ЭТАП)
// ============================================================================

std::string RagManager::generate_chunk_summary(
    const RagChunk& chunk,
    const std::string& query)
{
    std::cout << "[SUMMARY] Generating summary for chunk " << chunk.chunk_index
              << " (doc: " << chunk.document_id << ")" << std::endl;

    std::string system_prompt =
        "Ты - ассистент для анализа документов. Твоя задача - создавать краткие резюме текстов.\n"
        "Создавай резюме (3-5 предложений), которое содержит ключевую информацию и фокусируется на аспектах, релевантных вопросу пользователя.\n"
        "Сохраняй важные факты, цифры, имена. Игнорируй второстепенные детали.\n"
        "Пиши только резюме, без вводных фраз.";

    std::string user_message =
        "Текст для анализа:\n" + chunk.content + "\n\n"
        "Вопрос пользователя: " + query + "\n\n"
        "Создай краткое резюме текста, релевантное вопросу.";

    if (!llama_interface_) {
        std::cerr << "[SUMMARY] Error: LlamaInterface not initialized" << std::endl;
        return "";
    }

    std::cout << "[SUMMARY] Sending summary request to server..." << std::endl;

    ChatCompletionRequest request;
    request.max_tokens = 512;
    request.temperature = 0.3f;
    request.top_p = 0.9f;
    request.repeat_penalty = 1.0f;
    request.stream = false;

    ChatMessage system_msg(MessageRole::System, system_prompt);
    request.messages.push_back(system_msg);

    ChatMessage user_msg(MessageRole::User, user_message);
    request.messages.push_back(user_msg);

    try {
        auto future_response = llama_interface_->create_chat_completion_async(request);

        // For local CPU models: 90s is enough for prompt eval + generation
        // If it takes longer, the model is too slow for deep analysis - fall back to standard RAG
        auto status = future_response.wait_for(std::chrono::seconds(90));

        if (status == std::future_status::ready) {
            auto response = future_response.get();

            if (!response.choices.empty() && !response.choices[0].message.content.empty()) {
                std::string summary = response.choices[0].message.content;

                summary = trim_summary(summary);

                std::cout << "[SUMMARY] Generated summary: " << summary.size() << " chars"
                          << " (~" << estimate_tokens(summary) << " tokens)" << std::endl;

                return summary;
            } else {
                std::cerr << "[SUMMARY] Error: Empty response from server" << std::endl;
                return "[Пустое резюме]";
            }
        } else {
            std::cerr << "[SUMMARY] Error: Request timeout (90s)" << std::endl;
            std::cerr << "[SUMMARY] Try reducing batch size or increasing server context" << std::endl;
            return "[Таймаут генерации резюме]";
        }
    } catch (const std::exception& e) {
        std::cerr << "[SUMMARY] Error: Request failed: " << e.what() << std::endl;
        return "[Ошибка: " + std::string(e.what()) + "]";
    }
}

// ============================================================================
// СИНТЕЗ ФИНАЛЬНОГО ОТВЕТА (REDUCE-ЭТАП)
// ============================================================================

std::string RagManager::synthesize_final_answer(
    const std::string& query,
    const std::vector<std::string>& summaries,
    int target_context_size)
{
    std::cout << "[SYNTHESIS] Synthesizing final answer from " << summaries.size()
              << " summaries" << std::endl;

    std::string context;
    context.reserve(30000);

    for (size_t i = 0; i < summaries.size(); ++i) {
        context += "\n=== Источник " + std::to_string(i+1) + " ===\n";
        context += summaries[i];
    }

    int context_tokens = estimate_tokens(context);
    std::cout << "[SYNTHESIS] Total context: " << context.size() << " chars"
              << " (~" << context_tokens << " tokens)" << std::endl;

    if (context_tokens > target_context_size * 3 / 4) {
        std::cout << "[SYNTHESIS] Context too large, truncating..." << std::endl;
        size_t max_chars = target_context_size * 3;
        if (context.size() > max_chars) {
            context = context.substr(0, max_chars);
            while (!context.empty() &&
                   (static_cast<unsigned char>(context.back()) & 0xC0) == 0x80) {
                context.pop_back();
            }
            context += "\n[...обрезано из-за ограничения размера...]";
        }
    }

    std::string system_prompt =
        "Ты - ассистент для синтеза информации из нескольких источников. "
        "Твоя задача - создать полный, связный ответ на вопрос пользователя на основе предоставленных резюме.";

    std::string user_message =
        "Вопрос пользователя: " + query + "\n\n"
        "Промежуточные резюме из документов:\n" + context + "\n\n"
        "ИНСТРУКЦИЯ:\n"
        "1. Внимательно изучи все промежуточные резюме выше.\n"
        "2. Создай полный, связный ответ на вопрос пользователя.\n"
        "3. Твой ответ должен:\n"
        "   - Интегрировать информацию из всех источников\n"
        "   - Быть логически структурированным\n"
        "   - Содержать конкретные факты и детали\n"
        "   - Избегать повторений\n"
        "4. Если источники противоречат друг другу, укажи на это.\n"
        "5. Если информации недостаточно, честно скажи об этом.\n\n"
        "ОТВЕТ:";

    if (!llama_interface_) {
        std::cerr << "[SYNTHESIS] Error: LlamaInterface not initialized" << std::endl;
        return "";
    }

    std::cout << "[SYNTHESIS] Sending synthesis request to server..." << std::endl;

    ChatCompletionRequest request;

    request.max_tokens = std::min(target_context_size / 2, 1024);
    request.temperature = 0.5f;
    request.top_p = 0.95f;
    request.repeat_penalty = 1.1f;
    request.stream = false;

    ChatMessage system_msg(MessageRole::System, system_prompt);
    request.messages.push_back(system_msg);

    ChatMessage user_msg(MessageRole::User, user_message);
    request.messages.push_back(user_msg);

    try {
        auto future_response = llama_interface_->create_chat_completion_async(request);

        // Wait long enough for slow local CPU models
        auto status = future_response.wait_for(std::chrono::seconds(180));

        if (status == std::future_status::ready) {
            auto response = future_response.get();

            if (!response.choices.empty() && !response.choices[0].message.content.empty()) {
                std::string final_answer = response.choices[0].message.content;

                final_answer = trim_summary(final_answer);

                std::cout << "[SYNTHESIS] Final answer: " << final_answer.size() << " chars"
                          << " (~" << estimate_tokens(final_answer) << " tokens)" << std::endl;

                return final_answer;
            } else {
                std::cerr << "[SYNTHESIS] Error: Empty response from server" << std::endl;
            }
        } else {
            std::cerr << "[SYNTHESIS] Error: Request timeout (180s)" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[SYNTHESIS] Error: Request failed: " << e.what() << std::endl;
    }

    return "";
}

// ============================================================================
// АВТО-УВЕЛИЧЕНИЕ КОНТЕКСТА СЕРВЕРА
// ============================================================================

bool RagManager::auto_adjust_server_context_size(int target_context_size) {
    if (!llama_interface_) {
        std::cerr << "[CONTEXT ADJUST] Error: LlamaInterface not initialized" << std::endl;
        return false;
    }

    std::cout << "[CONTEXT ADJUST] Requesting server to adjust context size to "
              << target_context_size << "..." << std::endl;

    try {
        json server_info_json = llama_interface_->get_server_info();

        if (!server_info_json.is_null()) {
            int current_ctx = server_info_json.value("n_ctx", 0);

            if (current_ctx > 0) {
                std::cout << "[CONTEXT ADJUST] Current server context: "
                          << current_ctx << " tokens" << std::endl;

                if (current_ctx >= target_context_size) {
                    std::cout << "[CONTEXT ADJUST] Server context already sufficient" << std::endl;
                    return true;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[CONTEXT ADJUST] Warning: Failed to get server info: "
                  << e.what() << std::endl;
    }

    std::cerr << "[CONTEXT ADJUST] Server does not support dynamic context adjustment" << std::endl;
    std::cerr << "[CONTEXT ADJUST] Context size must be set when starting the server" << std::endl;
    std::cerr << "[CONTEXT ADJUST] Will work with existing context size" << std::endl;

    return false;
}

} // namespace core
} // namespace llama_gui

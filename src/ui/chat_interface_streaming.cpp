#include "../include/ui/chat_interface.h"
#include "../include/core/state_manager.h"
#include "../include/core/logger.h"
#include "imgui.h"
#include <iostream>
#include <mutex>
#include <chrono>

namespace llama_gui {
namespace ui {

void ChatInterface::start_streaming() {
    std::lock_guard<std::mutex> lock(streaming_mutex_);

    // Reset streaming state
    streaming_active_ = true;
    current_stream_content_ = "";

    // Reset performance metrics for new streaming session
    current_metrics_.response_time_seconds = 0.0;
    current_metrics_.tokens_generated = 0;
    current_metrics_.tokens_per_second = 0.0;
    current_metrics_.is_measuring = false;

    LOG_DEBUG("Streaming started - UI ready to display progressive responses");
}

void ChatInterface::stop_streaming() {
    // Сначала останавливаем HTTP запросы (без блокировки streaming_mutex_)
    // Это предотвращает deadlock с callback'ом
    llama_interface_.stop_streaming_requests();

    // Теперь обновляем состояние UI
    std::lock_guard<std::mutex> lock(streaming_mutex_);

    // Check if streaming was actually active
    if (!streaming_active_) {
        return; // Nothing to stop
    }

    streaming_active_ = false;

    // Store the partial content before clearing
    std::string final_content = current_stream_content_;

    // Clear the current stream content
    current_stream_content_ = "";

    // If we have a partial response, add it as a message
    if (!final_content.empty()) {
        // Get active conversation
        std::vector<llama_gui::core::Conversation*> all_conversations = state_manager_.get_all_conversations();
        llama_gui::core::Conversation* active_conv = nullptr;
        for (llama_gui::core::Conversation* conv : all_conversations) {
            if (conv->is_active) {
                active_conv = conv;
                break;
            }
        }

        if (active_conv) {
            // Add the partial response as an assistant message
            std::string final_msg = final_content + "\n\n[Generation stopped by user]";
            pending_responses_.push_back({final_msg, active_conv->id});

            LOG_DEBUG("Added stopped assistant message to conversation " + active_conv->id);
        }
    }

    LOG_DEBUG("Streaming stopped by user");
}

void ChatInterface::process_pending_responses() {
    // Process any pending responses from async operations
    std::lock_guard<std::mutex> lock(pending_responses_mutex_);

    if (pending_responses_.empty()) {
        return;
    }

    // Stop streaming state BEFORE adding messages to avoid duplication
    {
        std::lock_guard<std::mutex> stream_lock(streaming_mutex_);
        streaming_active_ = false;
        current_stream_content_ = "";
    }

    // Process all pending responses
    for (auto& pending_response : pending_responses_) {
        const auto& content = pending_response.content;
        const auto& conversation_id = pending_response.conversation_id;

        if (!content.empty()) {
            // Add assistant response
            llama_gui::core::Message assistant_msg("assistant", content);
            state_manager_.add_message(conversation_id, assistant_msg);

            // Invalidate cache for this conversation since we added a new message
            invalidate_cache_for_conversation(conversation_id);

            // Force scroll to bottom to show the new message (like web chat)
            if (auto_scroll_) {
                scroll_to_bottom();
            }

            LOG_DEBUG("Added assistant message to conversation " + conversation_id);
        } else {
            // Error response
            llama_gui::core::Message error_msg("assistant", "Error: No response from model");
            state_manager_.add_message(conversation_id, error_msg);

            // Invalidate cache for this conversation since we added a new message
            invalidate_cache_for_conversation(conversation_id);

            // Force scroll to bottom to show the error message
            if (auto_scroll_) {
                scroll_to_bottom();
            }

            LOG_ERROR("Added error message to conversation " + conversation_id);
        }
    }

    // Clear processed responses
    pending_responses_.clear();
}

void ChatInterface::update_performance_metrics(const std::string& content, bool is_final) {
    // Start measuring on first chunk
    if (!current_metrics_.is_measuring) {
        current_metrics_.start_time = std::chrono::steady_clock::now();
        current_metrics_.is_measuring = true;

        // Initialize context metrics
        current_metrics_.total_context = settings_.chat().n_ctx;
        
        // Calculate context used by conversation history
        int history_tokens = 0;
        std::vector<llama_gui::core::Conversation*> all_conversations = state_manager_.get_all_conversations();
        for (llama_gui::core::Conversation* conv : all_conversations) {
            if (conv->is_active) {
                // Count tokens in all messages except the last one (current user message)
                for (size_t i = 0; i < conv->messages.size(); ++i) {
                    const auto& msg = conv->messages[i];
                    // Estimate tokens: ~3.5 chars per token for mixed languages
                    history_tokens += std::max(1, static_cast<int>(msg.content.length() / 3.5));
                }
                break;
            }
        }
        current_metrics_.context_used = history_tokens;
        current_metrics_.remaining_context = std::max(0, 
            current_metrics_.total_context - current_metrics_.context_used);
    }

    if (is_final) {
        // Finalize metrics
        current_metrics_.end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_metrics_.end_time - current_metrics_.start_time);
        current_metrics_.response_time_seconds = duration.count() / 1000.0;

        // Estimate tokens using more accurate formula
        // For mixed languages (Russian + English + code): ~3-4 chars per token
        // Use 3.5 as average for better accuracy
        if (!content.empty()) {
            current_metrics_.tokens_generated = std::max(1, static_cast<int>(content.length() / 3.5));
        } else {
            current_metrics_.tokens_generated = 0;
        }

        // Update total context used (history + generated)
        current_metrics_.context_used += current_metrics_.tokens_generated;
        current_metrics_.remaining_context = std::max(0,
            current_metrics_.total_context - current_metrics_.context_used);

        current_metrics_.tokens_per_second = current_metrics_.response_time_seconds > 0 ?
            current_metrics_.tokens_generated / current_metrics_.response_time_seconds : 0.0;

        current_metrics_.is_measuring = false;

        LOG_INFO("Performance: " + std::to_string(current_metrics_.tokens_generated) + " tokens in " +
                  std::to_string(current_metrics_.response_time_seconds) + "s (" +
                  std::to_string(current_metrics_.tokens_per_second) + " tok/s)");

        // Записать статистику производительности модели
        std::string model_path = settings_.model_loading().model_path;
        if (!model_path.empty()) {
            settings_.model_performance().record_generation(
                model_path,
                current_metrics_.tokens_per_second,
                current_metrics_.response_time_seconds * 1000.0,  // конвертируем в мс
                current_metrics_.tokens_generated,
                current_metrics_.context_used,
                current_metrics_.total_context
            );
        }

        // Note: streaming_active_ will be reset in process_pending_responses()
        // to avoid race conditions
    } else {
        // Update real-time metrics for each chunk
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - current_metrics_.start_time);
        double elapsed_seconds = elapsed.count() / 1000.0;

        // Estimate current tokens using same formula (3.5 chars/token)
        if (!content.empty()) {
            current_metrics_.tokens_generated = std::max(1, static_cast<int>(content.length() / 3.5));
        }

        // Update remaining context (history + current generation)
        int total_used = current_metrics_.context_used + current_metrics_.tokens_generated;
        current_metrics_.remaining_context = std::max(0,
            current_metrics_.total_context - total_used);

        // Update real-time tokens per second
        current_metrics_.tokens_per_second = elapsed_seconds > 0 ?
            current_metrics_.tokens_generated / elapsed_seconds : 0.0;
    }
}

} // namespace ui
} // namespace llama_gui

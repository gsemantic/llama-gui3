#include "../include/ui/chat_interface.h"
#include "../include/core/state_manager.h"
#include "../include/core/model_manager.h"
#include "../include/core/logger.h"
#include "../include/core/env_manager.h"
#include "../include/ui/localization_manager.h"
#include "../include/core/openrouter_client.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>

namespace llama_gui {
namespace ui {

bool ChatInterface::submit_message(const std::string& content) {
    if (content.empty()) {
        return false;
    }
    set_input_text(content);
    send_message();
    return true;
}

void ChatInterface::add_user_message(const std::string& content) {
    if (content.empty()) return;

    std::vector<llama_gui::core::Conversation*> all_conversations = state_manager_.get_all_conversations();
    llama_gui::core::Conversation* active_conv = nullptr;
    for (llama_gui::core::Conversation* conv : all_conversations) {
        if (conv->is_active) {
            active_conv = conv;
            break;
        }
    }

    if (!active_conv) {
        std::string new_conv_id = state_manager_.create_conversation("New Chat");
        active_conv = state_manager_.get_conversation(new_conv_id);
        if (!active_conv) return;
        state_manager_.set_active_conversation(new_conv_id);
    }

    llama_gui::core::Message user_msg("user", content);
    state_manager_.add_message(active_conv->id, user_msg);

    if (active_conv->messages.size() == 1 &&
        (active_conv->title == "New Chat" || active_conv->title.empty())) {
        active_conv->title = generate_conversation_title(content);
    }

    invalidate_cache_for_conversation(active_conv->id);
}

void ChatInterface::add_assistant_message(const std::string& content) {
    if (content.empty()) return;

    std::vector<llama_gui::core::Conversation*> all_conversations = state_manager_.get_all_conversations();
    llama_gui::core::Conversation* active_conv = nullptr;
    for (llama_gui::core::Conversation* conv : all_conversations) {
        if (conv->is_active) {
            active_conv = conv;
            break;
        }
    }

    if (!active_conv) {
        std::string new_conv_id = state_manager_.create_conversation("New Chat");
        active_conv = state_manager_.get_conversation(new_conv_id);
        if (!active_conv) return;
        state_manager_.set_active_conversation(new_conv_id);
    }

    llama_gui::core::Message assistant_msg("assistant", content);
    state_manager_.add_message(active_conv->id, assistant_msg);
    invalidate_cache_for_conversation(active_conv->id);
}

void ChatInterface::send_message() {
    // Проверка: если включен облачный провайдер - используем его
    if (settings_.cloud_provider().enabled && !settings_.cloud_provider().model_id.empty()) {
        send_message_via_openrouter();
        return;
    }

    // OPTIMIZATION: Work directly with input_buffer_ to avoid unnecessary copies
    // Clean in-place by modifying input_buffer_ directly
    std::string cleaned_content;
    cleaned_content.reserve(strlen(input_buffer_)); // Pre-allocate memory

    for (size_t i = 0; i < strlen(input_buffer_); i++) {
        char c = input_buffer_[i];
        unsigned char uc = static_cast<unsigned char>(c);

        // Разрешаем все печатные символы, включая UTF-8
        if (uc >= 32 || uc == '\n' || uc == '\r' || uc == '\t') {
            cleaned_content += c;
        }
        // Пропускаем нулевые символы и другие проблемные
    }

    // OPTIMIZATION: Use swap instead of assignment to avoid copy
    std::string message_content;
    message_content.swap(cleaned_content);

    // Additional safety check: ensure no control characters remain at the beginning
    size_t start_pos = 0;
    while (start_pos < message_content.length()) {
        unsigned char first_char = static_cast<unsigned char>(message_content[start_pos]);
        if (first_char < 32 || first_char == 127) {
            start_pos++;
        } else {
            break;
        }
    }
    if (start_pos > 0) {
        message_content = message_content.substr(start_pos);
    }

    // Final validation: if message is empty after cleaning, return early
    if (message_content.empty()) {
        std::cerr << "WARNING: Message content is empty after cleaning" << std::endl;
        return;
    }

    // Fix UTF-8 encoding issues
    try {
        bool needs_conversion = false;
        for (char c : message_content) {
            if (static_cast<unsigned char>(c) > 127) {
                needs_conversion = true;
                break;
            }
        }

        if (needs_conversion) {
            std::string validated = validate_and_fix_utf8(message_content);

            if (validated.empty() || validated.find_first_not_of(' ') == std::string::npos) {
                // Try to detect and convert from Windows-1251 (common for Cyrillic)
                std::string temp_content;
                temp_content.reserve(strlen(input_buffer_));

                for (char c : input_buffer_) {
                    unsigned char uc = static_cast<unsigned char>(c);
                    if (uc >= 0xC0 && uc <= 0xFF) {
                        static const unsigned char win1251_to_utf8[][2] = {
                            {0xD0, 0x90}, {0xD0, 0x91}, {0xD0, 0x92}, {0xD0, 0x93}, {0xD0, 0x94}, {0xD0, 0x95}, {0xD0, 0x96}, {0xD0, 0x97},
                            {0xD0, 0x98}, {0xD0, 0x99}, {0xD0, 0x9A}, {0xD0, 0x9B}, {0xD0, 0x9C}, {0xD0, 0x9D}, {0xD0, 0x9E}, {0xD0, 0x9F},
                            {0xD0, 0xA0}, {0xD0, 0xA1}, {0xD0, 0xA2}, {0xD0, 0xA3}, {0xD0, 0xA4}, {0xD0, 0xA5}, {0xD0, 0xA6}, {0xD0, 0xA7},
                            {0xD0, 0xA8}, {0xD0, 0xA9}, {0xD0, 0xAA}, {0xD0, 0xAB}, {0xD0, 0xAC}, {0xD0, 0xAD}, {0xD0, 0xAE}, {0xD0, 0xAF},
                            {0xD0, 0xB0}, {0xD0, 0xB1}, {0xD0, 0xB2}, {0xD0, 0xB3}, {0xD0, 0xB4}, {0xD0, 0xB5}, {0xD0, 0xB6}, {0xD0, 0xB7},
                            {0xD0, 0xB8}, {0xD0, 0xB9}, {0xD0, 0xBA}, {0xD0, 0xBB}, {0xD0, 0xBC}, {0xD0, 0xBD}, {0xD0, 0xBE}, {0xD0, 0xBF},
                            {0xD1, 0x80}, {0xD1, 0x81}, {0xD1, 0x82}, {0xD1, 0x83}, {0xD1, 0x84}, {0xD1, 0x85}, {0xD1, 0x86}, {0xD1, 0x87},
                            {0xD1, 0x88}, {0xD1, 0x89}, {0xD1, 0x8A}, {0xD1, 0x8B}, {0xD1, 0x8C}, {0xD1, 0x8D}, {0xD1, 0x8E}, {0xD1, 0x8F}
                        };

                        if (uc >= 0xC0 && uc <= 0xFF) {
                            size_t idx = uc - 0xC0;
                            if (idx < sizeof(win1251_to_utf8)/sizeof(win1251_to_utf8[0])) {
                                temp_content += static_cast<char>(win1251_to_utf8[idx][0]);
                                temp_content += static_cast<char>(win1251_to_utf8[idx][1]);
                            } else {
                                temp_content += '?';
                            }
                        } else {
                            temp_content += c;
                        }
                    } else {
                        temp_content += c;
                    }
                }
                message_content.swap(temp_content);
            } else {
                message_content.swap(validated);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "UTF-8 conversion error: " << e.what() << std::endl;
    }

    // Efficient trim
    size_t start = message_content.find_first_not_of(" \n\r\t");
    size_t end = message_content.find_last_not_of(" \n\r\t");

    if (start == std::string::npos) {
        return;
    }

    if (start > 0 || end < message_content.length() - 1) {
        message_content = message_content.substr(start, end - start + 1);
    }

    if (message_content.empty()) {
        return;
    }

    // ПРОВЕРКА: Загружена ли модель?
    if (!server_ready_) {
        std::cout << "MainWindow: Модель не загружена, показываем диалог загрузки" << std::endl;

        if (model_load_request_callback_) {
            model_load_request_callback_(message_content);
        } else {
            std::cerr << "Ошибка: Модель не загружена. Пожалуйста, выберите модель в настройках." << std::endl;
        }
        return;
    }

    // ПРОВЕРКА: Идет ли загрузка модели?
    if (is_model_loading()) {
        std::cout << "MainWindow: Модель загружается, ждем завершения..." << std::endl;
        pending_query_for_loading_ = message_content;
        return;
    }

    // Get active conversation or create new one
    std::vector<llama_gui::core::Conversation*> all_conversations = state_manager_.get_all_conversations();
    llama_gui::core::Conversation* active_conv = nullptr;

    for (llama_gui::core::Conversation* conv : all_conversations) {
        if (conv->is_active) {
            active_conv = conv;
            break;
        }
    }

    if (!active_conv) {
        std::string new_conv_id = state_manager_.create_conversation("New Chat");
        active_conv = state_manager_.get_conversation(new_conv_id);
        if (!active_conv) {
            std::cerr << "Failed to create new conversation after retry" << std::endl;
            return;
        }
        state_manager_.set_active_conversation(new_conv_id);
    }

    // Проверка чередования ролей: если последнее сообщение от assistant с ошибкой, удаляем его
    if (!active_conv->messages.empty()) {
        const auto& last_msg = active_conv->messages.back();
        if (last_msg.role == "assistant" && 
            (last_msg.content.find("Error:") == 0 || last_msg.content.find("[Generation stopped") != std::string::npos)) {
            // Удаляем сообщение об ошибке или прерывании
            active_conv->messages.pop_back();
            invalidate_cache_for_conversation(active_conv->id);
        }
    }

    // Проверка: если последнее сообщение от user, добавляем assistant ответ-заглушку
    if (!active_conv->messages.empty() && active_conv->messages.back().role == "user") {
        // Последнее сообщение от пользователя, но нет ответа от ассистента
        // Это может произойти после ошибки сервера — добавим пустой ответ
        llama_gui::core::Message placeholder_msg("assistant", "[No response generated]");
        state_manager_.add_message(active_conv->id, placeholder_msg);
        invalidate_cache_for_conversation(active_conv->id);
    }

    // Проверка кэш-истории
    bool use_cache = (settings_.rag().rag_mode == llama_gui::core::RagMode::CacheOnly ||
                      settings_.rag().rag_mode == llama_gui::core::RagMode::Both);

    if (use_cache && rag_enabled_ && rag_manager_ && settings_.rag().enable_rag && settings_.rag().enable_caching) {
        std::string cached_response = rag_manager_->find_cached_response(message_content);

        if (!cached_response.empty()) {
            llama_gui::core::Message user_msg("user", message_content);
            state_manager_.add_message(active_conv->id, user_msg);

            llama_gui::core::Message assistant_msg("assistant", cached_response);
            state_manager_.add_message(active_conv->id, assistant_msg);

            if (active_conv->messages.size() == 2 &&
                (active_conv->title == "New Chat" || active_conv->title.empty())) {
                std::string title = generate_conversation_title(message_content);
                active_conv->title = title;
            }

            cache_current_interaction(message_content, cached_response);
            invalidate_cache_for_conversation(active_conv->id);
            clear_input();
            
            // Статистика: RAG cache hit
            cache_stats_.total_requests++;
            cache_stats_.rag_cache_hits++;
            cache_stats_.total_tokens_saved += static_cast<int>(cached_response.length() / 4);
            
            return;
        }
    }
    
    // Prompt Caching - проверка идентичных запросов
    {
        std::string cached_response;
        if (check_prompt_cache(message_content, cached_response)) {
            // Найдено в prompt cache!
            llama_gui::core::Message user_msg("user", message_content);
            state_manager_.add_message(active_conv->id, user_msg);

            llama_gui::core::Message assistant_msg("assistant", cached_response);
            state_manager_.add_message(active_conv->id, assistant_msg);

            if (active_conv->messages.size() == 2 &&
                (active_conv->title == "New Chat" || active_conv->title.empty())) {
                std::string title = generate_conversation_title(message_content);
                active_conv->title = title;
            }

            invalidate_cache_for_conversation(active_conv->id);
            clear_input();
            
            // Статистика уже обновлена в check_prompt_cache, но нужно добавить saved tokens
            cache_stats_.total_tokens_saved += static_cast<int>(cached_response.length() / 4);
            
            return;
        }
    }

    // === SHOW USER MESSAGE IMMEDIATELY (before slow RAG processing) ===
    // This prevents UI freeze - user sees their message right away
    {
        llama_gui::core::Message user_msg("user", message_content);
        state_manager_.add_message(active_conv->id, user_msg);

        // Rename conversation if first message
        if (active_conv->messages.size() == 1 &&
            (active_conv->title == "New Chat" || active_conv->title.empty())) {
            std::string title = generate_conversation_title(message_content);
            active_conv->title = title;
        }

        invalidate_cache_for_conversation(active_conv->id);
        clear_input();
    }
    // =================================================================

    // === BACKGROUND THREAD: RAG + Streaming (non-blocking) ===
    // All heavy work runs in a background thread so the UI stays responsive
    {
        // Capture everything needed by value before spawning the thread
        std::string msg_copy = message_content;
        std::string conv_id = active_conv->id;
        bool rag_enabled = rag_enabled_ && rag_manager_ && settings_.rag().enable_rag;
        bool use_documents = (settings_.rag().rag_mode == llama_gui::core::RagMode::DocumentsOnly ||
                              settings_.rag().rag_mode == llama_gui::core::RagMode::Both);
        bool has_attachments = !attachments_.empty() && !attachment_contents_.empty();
        std::string attachments_ctx;
        if (has_attachments) {
            attachments_ctx = build_attachments_context();
        }
        // Copy settings that the background thread needs
        auto* settings_ptr = &settings_;
        auto* rag_mgr = rag_manager_;
        auto* compressor = &context_compressor_;
        auto* monitor = &context_monitor_;
        auto* llama = &llama_interface_;
        auto* state = &state_manager_;
        auto* self = this;

        std::thread([self, msg_copy, conv_id, rag_enabled, use_documents, has_attachments, attachments_ctx,
                     settings_ptr, rag_mgr, compressor, monitor, llama, state]() mutable {
            try {
                // --- RAG processing (SLOW - runs in background) ---
                std::string final_content = msg_copy;
                bool has_rag_context = false;
                bool deep_analysis_done = false;

                if (use_documents && rag_enabled && rag_mgr) {
                    self->processing_rag_document_ = true;
                    std::string rag_prompt = self->process_with_rag(msg_copy);

                    // Check if deep analysis returned a final answer (not a RAG prompt)
                    // Deep analysis results don't start with "=== КОНТЕКСТ ==="
                    if (rag_prompt != msg_copy &&
                        rag_prompt.find("=== КОНТЕКСТ ===") == std::string::npos) {
                        // Deep analysis already produced the answer - return directly
                        self->processing_rag_document_ = false;
                        self->pending_responses_.push_back({rag_prompt, conv_id});
                        return;  // Skip streaming - we already have the answer
                    } else if (rag_prompt != msg_copy) {
                        final_content = rag_prompt;
                        has_rag_context = true;
                    } else {
                        self->processing_rag_document_ = false;
                    }
                }

                // Attachments
                if (has_attachments && !has_rag_context && !attachments_ctx.empty()) {
                    final_content = msg_copy + attachments_ctx;
                }

                // --- Prepare messages ---
                std::vector<llama_gui::core::ChatMessage> messages;
                messages.reserve(12);

                std::string system_prompt;
                if (has_rag_context) {
                    system_prompt = "Ты - полезный ассистент с доступом к внешним документам. "
                                    "Используй предоставленный контекст для ответа на вопрос. "
                                    "Отвечай кратко и по делу.";
                    messages.push_back(llama_gui::core::ChatMessage(llama_gui::core::MessageRole::System, system_prompt));
                }

                // Get all messages from conversation
                auto all_convs = state->get_all_conversations();
                llama_gui::core::Conversation* active_conv = nullptr;
                for (auto* c : all_convs) {
                    if (c->is_active) { active_conv = c; break; }
                }
                if (!active_conv) return;

                std::vector<llama_gui::core::ChatMessage> all_messages;
                for (const auto& msg : active_conv->messages) {
                    if (msg.content.find("Error:") == std::string::npos) {
                        llama_gui::core::MessageRole role_enum = llama_gui::core::MessageRole::User;
                        if (msg.role == "assistant") role_enum = llama_gui::core::MessageRole::Assistant;
                        else if (msg.role == "system") role_enum = llama_gui::core::MessageRole::System;
                        all_messages.push_back(llama_gui::core::ChatMessage(role_enum, msg.content));
                    }
                }

                for (size_t i = 0; i < all_messages.size(); ++i) {
                    bool is_last = (i == all_messages.size() - 1 &&
                                    all_messages[i].role == llama_gui::core::MessageRole::User);
                    std::string content = all_messages[i].content;
                    if (has_rag_context && is_last) content = final_content;
                    else if (has_attachments && !has_rag_context && is_last) content = final_content;
                    messages.push_back(llama_gui::core::ChatMessage(all_messages[i].role, content));
                }

                // --- Build request ---
                llama_gui::core::ChatCompletionRequest request;
                std::string model_path = settings_ptr->get_model_path();
                size_t last_slash = model_path.find_last_of("/\\");
                std::string filename = (last_slash == std::string::npos) ? model_path : model_path.substr(last_slash + 1);
                size_t last_dot = filename.find_last_of(".");
                request.model = (last_dot == std::string::npos) ? filename : filename.substr(0, last_dot);
                request.messages = messages;
                request.max_tokens = settings_ptr->chat().max_tokens;
                request.temperature = settings_ptr->chat().temperature;
                request.top_p = settings_ptr->chat().top_p;
                request.top_k = settings_ptr->chat().top_k;
                request.min_p = settings_ptr->chat().min_p;
                request.repeat_penalty = settings_ptr->chat().repeat_penalty;
                request.presence_penalty = settings_ptr->chat().presence_penalty;
                request.frequency_penalty = settings_ptr->chat().frequency_penalty;
                request.mirostat_mode = settings_ptr->chat().mirostat_mode;
                request.mirostat_tau = settings_ptr->chat().mirostat_tau;
                request.mirostat_eta = settings_ptr->chat().mirostat_eta;
                request.stop_on_newline = settings_ptr->chat().stop_on_newline;
                request.stream = true;

                // --- Start streaming (async - callback handles UI updates) ---
                std::string original_prompt = msg_copy;
                std::string rag_prompt_cache = final_content;

                self->start_streaming();
                self->processing_rag_document_ = false;

                llama->create_chat_completion_streaming(request,
                    [self, conv_id, original_prompt, rag_prompt_cache, has_rag_context](const std::string& chunk, bool is_final) {
                        try {
                            if (!chunk.empty()) {
                                std::string clean_chunk;
                                if (chunk.find("\"content\"") != std::string::npos) {
                                    try {
                                        auto json_chunk = nlohmann::json::parse(chunk);
                                        if (json_chunk.contains("choices") && !json_chunk["choices"].empty()) {
                                            auto& choice = json_chunk["choices"][0];
                                            if (choice.contains("delta") && choice["delta"].contains("content")) {
                                                auto content = choice["delta"]["content"];
                                                if (content.is_string()) clean_chunk = content.get<std::string>();
                                            } else if (choice.contains("message") && choice["message"].contains("content")) {
                                                auto content = choice["message"]["content"];
                                                if (content.is_string()) clean_chunk = content.get<std::string>();
                                            }
                                        }
                                    } catch (...) {}
                                }
                                if (!clean_chunk.empty()) {
                                    std::lock_guard<std::mutex> lock(self->streaming_mutex_);
                                    self->current_stream_content_ += clean_chunk;
                                    self->update_performance_metrics(self->current_stream_content_, false);
                                }
                            }
                            if (is_final) {
                                std::string final_content;
                                {
                                    std::lock_guard<std::mutex> lock(self->streaming_mutex_);
                                    final_content = self->current_stream_content_;
                                }
                                self->update_performance_metrics(final_content, true);
                                self->pending_responses_.push_back({final_content, conv_id});
                                int estimated_tokens = static_cast<int>(final_content.length() / 4);
                                self->update_prompt_cache(original_prompt, final_content, estimated_tokens);
                                if (has_rag_context && rag_prompt_cache != original_prompt) {
                                    self->update_rag_prompt_cache(rag_prompt_cache, final_content, estimated_tokens);
                                }
                                if (self->rag_enabled_ && self->settings_.rag().enable_caching) {
                                    self->cache_current_interaction(original_prompt, final_content);
                                }
                                self->cache_stats_.total_requests++;
                                self->cache_stats_.total_tokens_generated += estimated_tokens;
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "Error in streaming callback: " << e.what() << std::endl;
                            self->pending_responses_.push_back({"Error: " + std::string(e.what()), conv_id});
                        }
                    });

            } catch (const std::exception& e) {
                std::cerr << "Error in background RAG/streaming: " << e.what() << std::endl;
                llama_gui::core::Message error_msg("assistant", "Error: " + std::string(e.what()));
                state->add_message(conv_id, error_msg);
            }
        }).detach();
    }
    // ================================================================
}

void ChatInterface::clear_input() {
    input_buffer_[0] = '\0';
    ImGui::SetKeyboardFocusHere();
}

void ChatInterface::set_input_text(const std::string& text) {
    size_t max_len = sizeof(input_buffer_) - 1;
    size_t copy_len = text.length() < max_len ? text.length() : max_len;
    memcpy(input_buffer_, text.c_str(), copy_len);
    input_buffer_[copy_len] = '\0';
    input_focused_ = true;
}

void ChatInterface::open_model_selection_dialog() {
    std::cout << "ChatInterface: Opening model selection dialog" << std::endl;
    if (model_selection_callback_) {
        model_selection_callback_();
    } else {
        std::cout << "ChatInterface: No model selection callback set" << std::endl;
    }
}

void ChatInterface::set_model_selection_callback(std::function<void()> callback) {
    model_selection_callback_ = callback;
    std::cout << "ChatInterface: Model selection callback set" << std::endl;
}

// ============================================================================
// OpenRouter Integration
// ============================================================================

void ChatInterface::send_message_via_openrouter() {
    std::string message_content = input_buffer_;
    
    // Очистка input
    memset(input_buffer_, 0, sizeof(input_buffer_));
    input_focused_ = true;
    
    if (message_content.empty()) {
        return;
    }

    // === RAG обработка для облачной модели ===
    std::string final_message_content = message_content;
    bool has_rag_context = false;

    if (rag_enabled_ && rag_manager_ && settings_.rag().enable_rag) {
        std::string rag_prompt = process_with_rag(message_content, false);  // no cache for cloud models
        if (rag_prompt != message_content) {
            final_message_content = rag_prompt;
            has_rag_context = true;
            std::cout << "[CloudProvider] RAG context added for cloud model" << std::endl;
        }
    }

    // === Добавление содержимого прикреплённых файлов (если RAG не нашёл контекст) ===
    bool has_attachments = !attachments_.empty() && !attachment_contents_.empty();
    if (has_attachments && !has_rag_context) {
        std::string attachments_context = build_attachments_context();
        if (!attachments_context.empty()) {
            final_message_content = message_content + attachments_context;
            std::cout << "[CloudProvider] Adding " << attachments_context.size()
                      << " bytes of file content to message" << std::endl;
        }
    }
    // ==========================================================
    
    // Получаем активную конверсацию
    std::vector<llama_gui::core::Conversation*> all_convs = state_manager_.get_all_conversations();
    llama_gui::core::Conversation* active_conv = nullptr;
    for (auto* conv : all_convs) {
        if (conv->is_active) {
            active_conv = conv;
            break;
        }
    }
    
    if (!active_conv) {
        // Создаём новую конверсацию
        std::string new_conv_id = state_manager_.create_conversation("New Chat");
        active_conv = state_manager_.get_conversation(new_conv_id);
        state_manager_.set_active_conversation(new_conv_id);
    }
    
    // Добавляем сообщение пользователя (с RAG-контекстом если есть)
    llama_gui::core::Message user_msg;
    user_msg.role = "user";
    user_msg.content = final_message_content;
    state_manager_.add_message(active_conv->id, user_msg);
    
    // Получаем настройки облачного провайдера
    const auto& cp = settings_.cloud_provider();
    const std::string& model_id = cp.model_id;
    // API ключ читается из .env файла (provider-specific слот: ключ OpenCode Zen
    // хранится отдельно и не конфликтует с ключами других провайдеров)
    std::string key_name = llama_gui::core::EnvManager::cloud_provider_api_key_name(cp.provider_name, cp.endpoint_url);
    const std::string api_key = llama_gui::core::EnvManager::read_key(
        key_name, settings_.get_profiles_directory());
    
    std::cout << "[CloudProvider] Отправка запроса к модели: " << model_id << std::endl;
    std::cout << "[CloudProvider] API ключ: " << (api_key.empty() ? "НЕТ" : api_key.substr(0, 8) + "...") << std::endl;

    // Создаём клиент с настройками провайдера
    llama_gui::core::OpenRouterClient client(api_key);
    client.set_timeout(cp.timeout_ms);
    if (!cp.endpoint_url.empty()) {
        client.set_base_url(cp.endpoint_url);
    }
    
    // Формируем параметры запроса
    llama_gui::core::OpenRouterRequestParams params;
    params.model = model_id;
    // Лимит токенов вывода берём из настроек облачного соединения.
    // 0 = не ограничено: поле max_tokens не отправляется, и провайдер/модель
    // использует свой максимум (важно для thinking-моделей вроде GLM).
    params.max_tokens = cp.max_output_tokens;
    params.temperature = settings_.chat().temperature;
    params.top_p = settings_.chat().top_p;
    params.stream = false;
    
    // Дополнительные параметры (поддерживаются не всеми моделями)
    if (settings_.chat().presence_penalty != 0.0f) {
        params.presence_penalty = settings_.chat().presence_penalty;
    }
    if (settings_.chat().frequency_penalty != 0.0f) {
        params.frequency_penalty = settings_.chat().frequency_penalty;
    }
    
    std::cout << "[CloudProvider] Параметры: model=" << model_id 
              << ", max_tokens=" << params.max_tokens 
              << ", temp=" << params.temperature 
              << ", top_p=" << params.top_p;
    if (params.presence_penalty != 0.0f) {
        std::cout << ", presence=" << params.presence_penalty;
    }
    if (params.frequency_penalty != 0.0f) {
        std::cout << ", frequency=" << params.frequency_penalty;
    }
    std::cout << std::endl;
    
    // Добавляем системный промпт
    if (!settings_.chat().default_system_prompt.empty()) {
        llama_gui::core::OpenRouterRequestParams::Message sys_msg;
        sys_msg.role = "system";
        sys_msg.content = settings_.chat().default_system_prompt;
        params.messages.push_back(sys_msg);
    }

    // Получаем историю диалога из active_conv (объявлена выше)
    if (active_conv) {
        for (const auto& msg : active_conv->messages) {
            llama_gui::core::OpenRouterRequestParams::Message orch_msg;
            orch_msg.role = msg.role;
            orch_msg.content = msg.content;
            params.messages.push_back(orch_msg);
        }
    }
    
    // Показываем индикатор загрузки
    start_streaming();

    // Отправляем стриминговый запрос (в отдельном потоке, чтобы не блокировать UI)
    std::string api_key_copy = api_key;
    std::string model_id_copy = model_id;
    std::string endpoint_copy = cp.endpoint_url;
    int timeout_copy = cp.timeout_ms;
    std::string conv_id = active_conv ? active_conv->id : "";
    std::string original_prompt = message_content;
    std::string rag_prompt_cache = final_message_content;
    bool has_rag_ctx = has_rag_context;

    params.stream = true;

    std::thread([this, api_key_copy, model_id_copy, endpoint_copy, timeout_copy, params,
                 conv_id, original_prompt, rag_prompt_cache, has_rag_ctx]() {
        llama_gui::core::OpenRouterClient client(api_key_copy);
        client.set_timeout(timeout_copy);
        if (!endpoint_copy.empty()) {
            client.set_base_url(endpoint_copy);
        }

        // Стриминговый колбэк: обновляет UI по мере поступления токенов
        client.complete_streaming_async(params,
            [this, conv_id, original_prompt, rag_prompt_cache, has_rag_ctx, model_id_copy](
                const std::string& token, bool is_done) {
                try {
                    if (!token.empty() && !is_done) {
                        // Обычный токен ответа - накапливаем в стриминговый буфер
                        std::lock_guard<std::mutex> lock(streaming_mutex_);
                        current_stream_content_ += token;
                        update_performance_metrics(current_stream_content_, false);
                    }

                    if (is_done) {
                        std::string final_content;
                        if (!token.empty()) {
                            // Ошибка доставлена вместе с флагом завершения
                            final_content = token;
                            std::cerr << "[CloudProvider] Ошибка: " << token << std::endl;
                        } else {
                            std::lock_guard<std::mutex> lock(streaming_mutex_);
                            final_content = current_stream_content_;
                            std::cout << "[CloudProvider] Ответ получен (модель " << model_id_copy
                                      << "): " << final_content.size() << " символов" << std::endl;
                        }

                        update_performance_metrics(final_content, true);
                        pending_responses_.push_back({final_content, conv_id});

                        int estimated_tokens = static_cast<int>(final_content.length() / 4);
                        if (has_rag_ctx && rag_prompt_cache != original_prompt) {
                            update_rag_prompt_cache(rag_prompt_cache, final_content, estimated_tokens);
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error in cloud streaming callback: " << e.what() << std::endl;
                    pending_responses_.push_back({"Error: " + std::string(e.what()), conv_id});
                }
            });
    }).detach();
}

} // namespace ui
} // namespace llama_gui

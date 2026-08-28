#include "../include/ui/chat_interface.h"
#include "../include/ui/rag_interface.h"
#include "../include/ui/window_coordinator.h"
#include "../include/ui/window_manager.h"
#include "../include/core/state_manager.h"
#include "../include/core/rag_manager.h"
#include "../include/ui/localization_manager.h"
#include "../external/imgui/imgui_internal.h"
#include "imgui.h"
#include <algorithm>
#include <iostream>
#include <chrono>
#include <string>

namespace llama_gui {
namespace ui {

void ChatInterface::render(bool* visible) {
    // Добавляем флаги для корректного отображения окна
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    flags |= ImGuiWindowFlags_NoCollapse;  // Запретить сворачивание

    // Стандартное окно ImGui с заголовком и кнопками управления
    // Кнопка закрытия (×) и сворачивания (─) рисуются автоматически ImGui
    if (!ImGui::Begin("Chat", visible, flags)) {
        ImGui::End();
        return;
    }

    // Dock context menu (right-click on title bar)
    if (window_manager_) {
        WindowCoordinator::renderDockMenuStatic("chat", window_manager_);
    }
    
    ImVec2 window_size = ImGui::GetWindowSize();
    ImVec2 window_pos = ImGui::GetWindowPos();

    process_pending_responses();

    static int frame_counter = 0;
    if (++frame_counter % 200 == 0) {
        cleanup_old_cache_entries();
    }

    static bool show_cache_stats = true;  // Показывать статистику кэша
    if (show_cache_stats) {
        render_cache_stats(&show_cache_stats);
    }

    render_message_list();
    render_input_area();

    // Render RAG mini indicator AFTER all content (overlay in top-right corner)
    render_rag_mini_indicator();

    ImGui::End();
}

void ChatInterface::render_message_list() {
    // Высота input area = 5 строк текста + кнопки + отступы (должна совпадать с render_input_area)
    // +4px запас на рамку MessagesScroll и padding окна Chat
    float input_area_height = ImGui::GetTextLineHeight() * 5
                            + ImGui::GetStyle().FramePadding.y * 2
                            + ImGui::GetTextLineHeight()
                            + ImGui::GetStyle().ItemSpacing.y * 3
                            + ImGui::GetStyle().FramePadding.y
                            + ImGui::GetStyle().WindowPadding.y * 2
                            + ImGui::GetStyle().FrameBorderSize * 2;
    float available_height = ImGui::GetContentRegionAvail().y - input_area_height;
    if (available_height < 150.0f) available_height = 150.0f;

    ImGui::BeginChild("MessagesScroll", ImVec2(0, available_height), true);

    // Скролл-детекция ВНУТРИ дочернего окна MessagesScroll
    {
        static float last_scroll_y = 0.0f;
        float current_scroll_y = ImGui::GetScrollY();
        float max_scroll_y = ImGui::GetScrollMaxY();

        // Пользователь скроллит вверх — отключаем автопркрутку
        if (last_scroll_y > 0 && current_scroll_y < last_scroll_y - 10.0f) {
            auto_scroll_ = false;
        }

        // Пользователь долистал до самого низа — включаем обратно
        if (max_scroll_y > 0 && current_scroll_y >= max_scroll_y - 10.0f) {
            auto_scroll_ = true;
        }

        last_scroll_y = current_scroll_y;
    }

    std::vector<llama_gui::core::Conversation*> all_conversations = state_manager_.get_all_conversations();
    const llama_gui::core::Conversation* active_conversation = nullptr;
    for (llama_gui::core::Conversation* conv : all_conversations) {
        if (conv->is_active) {
            active_conversation = conv;
            break;
        }
    }

    if (!active_conversation) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", TR("chat.no_active_conversation"));
        ImGui::EndChild();
        return;
    }

    std::string formatted_title = TRF("chat.title", "Conversation: %s");
    size_t pos = formatted_title.find("%s");
    if (pos != std::string::npos) {
        formatted_title.replace(pos, 2, active_conversation->title);
    }
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", formatted_title.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    static const ImVec4 USER_COLOR(0.0f, 0.5f, 1.0f, 1.0f);
    static const ImVec4 ASSISTANT_COLOR(0.0f, 0.8f, 0.0f, 1.0f);
    static const ImVec4 SYSTEM_COLOR(0.8f, 0.8f, 0.0f, 1.0f);
    static const ImVec4 DEFAULT_COLOR(0.0f, 0.0f, 0.0f, 1.0f);

    // Режим выделения действителен только для активной беседы и существующего индекса
    if (selectable_message_index_ >= 0 &&
        (selectable_conversation_id_ != active_conversation->id ||
         static_cast<size_t>(selectable_message_index_) >= active_conversation->messages.size())) {
        exit_message_select_mode();
    }

    static std::string cached_user_role, cached_assistant_role, cached_system_role;
    static bool roles_cached = false;

    for (size_t i = 0; i < active_conversation->messages.size(); ++i) {
        const auto& message = active_conversation->messages[i];

        const ImVec4& message_color = (message.role == "user") ? USER_COLOR :
                                       (message.role == "assistant") ? ASSISTANT_COLOR :
                                       (message.role == "system") ? SYSTEM_COLOR : DEFAULT_COLOR;

        ImGui::BeginGroup();

        if (!roles_cached) {
            cached_user_role = TRF("message.user", "User");
            cached_assistant_role = TRF("message.assistant", "Assistant");
            cached_system_role = TRF("message.system", "System");
            roles_cached = true;
        }

        const std::string& role_text = (message.role == "user") ? cached_user_role :
                                       (message.role == "assistant") ? cached_assistant_role :
                                       (message.role == "system") ? cached_system_role : message.role;

        ImGui::TextColored(message_color, "%s:", role_text.c_str());

        const bool in_select_mode = (static_cast<int>(i) == selectable_message_index_);
        bool render_as_selectable = in_select_mode;

        if (in_select_mode) {
            // Кнопка выхода из режима выделения на строке заголовка сообщения
            ImGui::SameLine();
            float close_btn_w = ImGui::CalcTextSize("×").x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - close_btn_w);
            if (ImGui::SmallButton(("×##exit_select_" + std::to_string(i)).c_str())) {
                // Выходим ДО рендера поля: буфер будет очищен, поэтому в этом кадре
                // рисуем обычный TextWrapped, а не InputTextMultiline с пустым буфером
                exit_message_select_mode();
                render_as_selectable = false;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", TRF("chat.exit_select_mode", "Exit selection mode"));
            }
        }

        if (render_as_selectable) {
            // Readonly InputTextMultiline: выделение мышью и Ctrl+C работают нативно.
            // Экземпляр существует максимум один — нагрузка сопоставима с TextWrapped.
            float wrap_width = ImGui::GetContentRegionAvail().x;
            float text_height = ImGui::CalcTextSize(selectable_buffer_.data(), nullptr, false, wrap_width).y
                              + ImGui::GetStyle().FramePadding.y * 2;
            if (selectable_focus_requested_) {
                ImGui::SetKeyboardFocusHere();
                selectable_focus_requested_ = false;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, message_color);
            ImGui::InputTextMultiline(
                ("##msg_select_" + std::to_string(i)).c_str(),
                selectable_buffer_.data(),
                selectable_buffer_.size(),
                ImVec2(wrap_width, text_height),
                ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap |
                ImGuiInputTextFlags_NoHorizontalScroll);
            ImGui::PopStyleColor();

            // Состояние поля фиксируем ДО меню: после EndPopup LastItemData
            // указывает на последний пункт меню, а не на поле
            const ImGuiID sel_id = ImGui::GetItemID();
            const bool sel_field_active = ImGui::IsItemActive();

            // Контекстное меню с ПРЯМЫМ копированием выделения из состояния поля —
            // без эмуляции клавиш. Выделение читается даже у неактивного поля.
            if (sel_id != 0 && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup("##msg_select_menu");
            }

            // Выход по Esc — только когда контекстное меню не открыто
            if (sel_field_active && !ImGui::IsPopupOpen("##msg_select_menu") &&
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                exit_message_select_mode();
            }

            if (ImGui::BeginPopup("##msg_select_menu")) {
                ImGuiInputTextState* sel_state = ImGui::GetInputTextState(sel_id);
                const bool has_selection = sel_state && sel_state->HasSelection();

                if (ImGui::MenuItem(TRF("context.copy", "Copy"), "Ctrl+C", false, has_selection)) {
                    int sel_s = sel_state->GetSelectionStart();
                    int sel_e = sel_state->GetSelectionEnd();
                    if (sel_s > sel_e) { int t = sel_s; sel_s = sel_e; sel_e = t; }
                    std::string selected_text(selectable_buffer_.data() + sel_s,
                                              static_cast<size_t>(sel_e - sel_s));
                    ImGui::SetClipboardText(selected_text.c_str());
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem(TRF("chat.copy_message", "Copy Entire Message"))) {
                    ImGui::SetClipboardText(message.content.c_str());
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem(TRF("context.select_all", "Select All"), "Ctrl+A")) {
                    if (sel_state) sel_state->SelectAll();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(TRF("chat.exit_select_mode", "Exit selection mode (Esc)"))) {
                    ImGui::CloseCurrentPopup();
                    exit_message_select_mode();
                }
                ImGui::EndPopup();
            }

            // ВАЖНО: пока меню открыто, поле НЕ активируем — активация вызывает
            // FocusWindow() на окне чата, а фокус вне попапа закрывает его
            // (меню «мелькает»). Активность возвращаем полю ПОСЛЕ закрытия меню:
            // TryToPreserveState восстанавливает курсор и выделение, выделение
            // снова видно, работают нативные Ctrl+C / Ctrl+A.
            const bool menu_open_now = ImGui::IsPopupOpen("##msg_select_menu");
            if (select_menu_open_prev_ && !menu_open_now && selectable_message_index_ >= 0 &&
                !selectable_focus_requested_) {
                GImGui->NavNextActivateId = sel_id;
                GImGui->NavNextActivateFlags = ImGuiActivateFlags_TryToPreserveState;
            }
            select_menu_open_prev_ = menu_open_now;
        } else {
            // Рендеринг текста сообщения — TextWrapped, без child window
            ImGui::PushTextWrapPos(0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, message_color);
            ImGui::TextWrapped("%s", message.content.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }

        // Контекстное меню + Ctrl+C для копирования
        if (!in_select_mode && ImGui::IsItemHovered()) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup(("copy_menu_" + std::to_string(i)).c_str());
            }
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
                ImGui::SetClipboardText(message.content.c_str());
            }
        }

        if (!in_select_mode && ImGui::BeginPopup(("copy_menu_" + std::to_string(i)).c_str())) {
            if (ImGui::MenuItem(TRF("chat.copy_message", "Copy Entire Message"), "Ctrl+C")) {
                ImGui::SetClipboardText(message.content.c_str());
            }
            if (ImGui::MenuItem(TRF("chat.select_text", "Select Text"))) {
                selectable_buffer_.assign(message.content.begin(), message.content.end());
                selectable_buffer_.push_back('\0');
                selectable_conversation_id_ = active_conversation->id;
                selectable_message_index_ = static_cast<int>(i);
                selectable_focus_requested_ = true;
            }
            ImGui::EndPopup();
        }

        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::Spacing();
    }

    if (auto_scroll_) {
        // Автопркрутка только если пользователь уже у низа (чтобы не дёргало)
        float scroll_pos = ImGui::GetScrollY();
        float scroll_max = ImGui::GetScrollMaxY();
        if (scroll_max <= 0.0f || scroll_pos >= scroll_max - 100.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
    }

    if (streaming_active_) {
        ImGui::BeginGroup();

        if (current_stream_content_.empty()) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", TRF("message.processing", "Processing request..."));
            ImGui::SameLine();

            static float dots_time = 0.0f;
            dots_time += ImGui::GetIO().DeltaTime;
            int dots = static_cast<int>(dots_time * 2.0f) % 3;
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%.*s", dots + 1, "...");
        } else {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", TRF("message.generating", "Assistant (generating):"));
            ImGui::SameLine();

            static float dots_time = 0.0f;
            dots_time += ImGui::GetIO().DeltaTime;
            int dots = static_cast<int>(dots_time * 2.0f) % 3;
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%.*s", dots + 1, "...");

            static std::string stream_display_buffer;
            stream_display_buffer = current_stream_content_;
            const size_t max_display_length = 10000;
            if (stream_display_buffer.length() > max_display_length) {
                stream_display_buffer = stream_display_buffer.substr(0, max_display_length) + "...";
            }

            ImGui::PushTextWrapPos(0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.8f, 0.0f, 1.0f));
            ImGui::TextWrapped("%s", stream_display_buffer.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }

        ImGui::EndGroup();

        // Принудительная прокрутка вниз при каждом кадре стриминга
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
}

void ChatInterface::exit_message_select_mode() {
    selectable_message_index_ = -1;
    selectable_conversation_id_.clear();
    selectable_buffer_.clear();
    selectable_buffer_.shrink_to_fit();
    selectable_focus_requested_ = false;
    select_menu_open_prev_ = false;
}

void ChatInterface::render_typing_indicator() {
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Assistant: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "%s", current_stream_content_.c_str());
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "█");
}

void ChatInterface::render_performance_metrics() {
    // Render metrics in a single compact line
    if (current_metrics_.is_measuring) {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f),
                          TRF("performance.generating_short", "Generating: %d tok | %.1f tok/s | %ds | Context: %d/%d"),
                          current_metrics_.tokens_generated,
                          current_metrics_.tokens_per_second,
                          static_cast<int>(current_metrics_.response_time_seconds),
                          current_metrics_.context_used + current_metrics_.tokens_generated,
                          current_metrics_.total_context);
    } else if (current_metrics_.response_time_seconds > 0) {
        if (current_metrics_.total_context > 0) {
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f),
                              TRF("performance.completed_short", "✓ %d tok | %.1f tok/s | %ds | Context: %d/%d"),
                              current_metrics_.tokens_generated,
                              current_metrics_.tokens_per_second,
                              static_cast<int>(current_metrics_.response_time_seconds),
                              current_metrics_.context_used,
                              current_metrics_.total_context);
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f),
                               TRF("performance.completed_short_noctx", "✓ %d tok | %.1f tok/s | %ds | Context: %d"),
                               current_metrics_.tokens_generated,
                               current_metrics_.tokens_per_second,
                               static_cast<int>(current_metrics_.response_time_seconds),
                               current_metrics_.context_used);
        }
        // Расход облачного провайдера (USD)
        if (settings_.cloud_provider().enabled &&
            (cache_stats_.last_cost_usd > 0 || cache_stats_.total_cost_usd > 0)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                               " | Cost: $%.4f (session: $%.4f)",
                               cache_stats_.last_cost_usd, cache_stats_.total_cost_usd);
        }
    }
}

void ChatInterface::render_cache_stats(bool* visible) {
    if (!visible || !*visible) return;
    
    // Показываем статистику только если были запросы
    if (cache_stats_.total_requests == 0) return;
    
    ImGui::Separator();
    
    // Заголовок с кнопкой закрытия
    if (ImGui::Button("×", ImVec2(20, 0))) {
        *visible = false;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.0f), "Cache Statistics");
    
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(50, 0))) {
        reset_cache_stats();
    }
    
    // Основная статистика
    ImGui::Separator();
    
    // Процент попаданий в кэш
    double hit_rate = cache_stats_.get_cache_hit_rate();
    ImGui::Text("Cache Hit Rate: %.1f%%", hit_rate);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Percentage of requests served from cache");
    }
    
    // Экономия токенов
    double savings_rate = cache_stats_.get_token_savings_rate();
    ImGui::Text("Token Savings: %.1f%%", savings_rate);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Percentage of tokens saved by caching");
    }
    
    // Детальная статистика
    ImGui::Separator();
    ImGui::Text("Total Requests: %d", cache_stats_.total_requests);
    ImGui::SameLine();
    ImGui::Text(" | Prompt Cache: %d", cache_stats_.prompt_cache_hits);
    ImGui::SameLine();
    ImGui::Text(" | RAG Cache: %d", cache_stats_.rag_cache_hits);
    
    ImGui::Text("Tokens Generated: %d", cache_stats_.total_tokens_generated);
    ImGui::SameLine();
    ImGui::Text(" | Saved: %d", cache_stats_.total_tokens_saved);
    
    // Расход денег (облачный провайдер)
    if (settings_.cloud_provider().enabled) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Cost (session): $%.4f", cache_stats_.total_cost_usd);
        ImGui::SameLine();
        ImGui::Text(" | Last: $%.4f", cache_stats_.last_cost_usd);
        if (cache_stats_.last_prompt_tokens > 0 || cache_stats_.last_completion_tokens > 0) {
            ImGui::Text("Last request tokens: in=%d out=%d (total %d)",
                        cache_stats_.last_prompt_tokens,
                        cache_stats_.last_completion_tokens,
                        cache_stats_.last_prompt_tokens + cache_stats_.last_completion_tokens);
        }
    }

    // Время сессии
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - cache_stats_.session_start);
    ImGui::Text("Session Duration: %d min", static_cast<int>(duration.count()));
    
    ImGui::Separator();
}

void ChatInterface::render_rag_mini_indicator() {
    if (!rag_interface_ || !rag_enabled_) return;

    bool is_indexing = rag_interface_->is_indexing();
    float progress = rag_interface_->get_indexing_progress();
    llama_gui::core::IndexingPhase phase = rag_interface_->get_indexing_phase();

    if (!is_indexing && phase != llama_gui::core::IndexingPhase::Complete &&
        phase != llama_gui::core::IndexingPhase::Error) {
        return;
    }

    // Get chat window position for anchoring
    ImVec2 chat_pos = ImGui::GetWindowPos();
    ImVec2 chat_size = ImGui::GetWindowSize();

    float indicator_width = 180.0f;
    float indicator_height = 28.0f;

    // Position in top-right corner of chat window
    ImGui::SetNextWindowPos(ImVec2(
        chat_pos.x + chat_size.x - indicator_width - 8,
        chat_pos.y + 32  // Below title bar
    ), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(indicator_width, indicator_height), ImGuiCond_Always);

    ImGuiWindowFlags win_flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##rag_mini", nullptr, win_flags)) {
        if (is_indexing) {
            const char* phase_text = "";
            switch (phase) {
                case llama_gui::core::IndexingPhase::Parsing:   phase_text = "PARSE"; break;
                case llama_gui::core::IndexingPhase::Chunking:  phase_text = "CHUNK"; break;
                case llama_gui::core::IndexingPhase::Embedding: phase_text = "EMBED"; break;
                case llama_gui::core::IndexingPhase::Saving:    phase_text = "SAVE"; break;
                default: phase_text = "RAG"; break;
            }

            char label[64];
            snprintf(label, sizeof(label), "%s %d%%", phase_text, static_cast<int>(progress * 100));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.3f, 0.5f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.7f, 1.0f, 0.9f));
            ImGui::ProgressBar(progress, ImVec2(-1, 0), label);
            ImGui::PopStyleColor(2);
        } else if (phase == llama_gui::core::IndexingPhase::Complete) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "RAG Index Ready");
        } else if (phase == llama_gui::core::IndexingPhase::Error) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "RAG Index Error");
        }
    }
    ImGui::End();
}

void ChatInterface::scroll_to_bottom() {
    ImGui::SetScrollHereY(1.0f);
    ImGui::SetScrollHereY(1.0f);
}

} // namespace ui
} // namespace llama_gui

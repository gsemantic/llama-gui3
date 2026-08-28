#include "../include/ui/chat_interface.h"
#include "../include/core/state_manager.h"
#include "../include/ui/localization_manager.h"
#include "../include/ui/file_dialog_helper.h"
#include "../include/ui/file_dialog_manager.h"
#include "imgui.h"
#include <iostream>
#include <algorithm>

namespace llama_gui {
namespace ui {

static int InputFieldCallback(ImGuiInputTextCallbackData* data) {
    ChatInterface* chat_interface = (ChatInterface*)data->UserData;

    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        chat_interface->set_input_cursor_pos(data->CursorPos);
        chat_interface->set_input_selection_start(data->SelectionStart);
        chat_interface->set_input_selection_end(data->SelectionEnd);

        if (data->SelectionStart != data->SelectionEnd) {
            int start = std::min(data->SelectionStart, data->SelectionEnd);
            int end = std::max(data->SelectionStart, data->SelectionEnd);
            chat_interface->set_input_selected_text(std::string(data->Buf + start, end - start));
        } else {
            chat_interface->set_input_selected_text("");
        }
    }

    // Filter control characters (but allow UTF-8 - bytes > 127 are valid)
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        unsigned int c = static_cast<unsigned int>(data->EventChar);
        // Allow printable ASCII, whitespace, and all high bytes (UTF-8)
        if (c >= 32 || c == '\n' || c == '\r' || c == '\t' || c > 127) {
            return 0; // Allow
        }
        return 1; // Filter out control characters
    }

    return 0;
}

void ChatInterface::render_input_area() {
    float input_area_height = ImGui::GetTextLineHeight() * 5   // 5 строк текста
                            + ImGui::GetStyle().FramePadding.y * 2
                            + ImGui::GetTextLineHeight()       // кнопки
                            + ImGui::GetStyle().ItemSpacing.y * 3
                            + ImGui::GetStyle().FramePadding.y;
    ImGui::BeginChild("InputArea", ImVec2(0, input_area_height), false);

    render_attachments();

    float input_width = ImGui::GetContentRegionAvail().x;

    // Вычисляем высоту поля ввода ТОЧНО по контенту (как в сообщениях)
    float content_height = ImGui::CalcTextSize(input_buffer_, nullptr, true, input_width).y
                           + ImGui::GetStyle().FramePadding.y * 2;
    
    // Минимальная высота - 5 строк
    float min_height = ImGui::GetTextLineHeight() * 5 + ImGui::GetStyle().FramePadding.y * 2;
    if (content_height < min_height) {
        content_height = min_height;
    }

    ImGui::InputTextMultiline(
        "##chat_input",
        input_buffer_,
        sizeof(input_buffer_),
        ImVec2(input_width, content_height),  // Высота ТОЧНО по контенту
        ImGuiInputTextFlags_AllowTabInput |
        // Убрали CtrlEnterForNewLine — теперь Enter добавляет строку, а Ctrl+Enter отправляет
        ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_WordWrap |
        ImGuiInputTextFlags_CallbackCharFilter |
        ImGuiInputTextFlags_NoHorizontalScroll,  // Убираем горизонтальную прокрутку
        InputFieldCallback,
        this
    );

    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("input_context_menu");
    }

    if (ImGui::BeginPopup("input_context_menu")) {
        const std::string& selected = get_input_selected_text();
        bool has_selection = !selected.empty();

        if (ImGui::MenuItem(TRF("context.cut", "Cut"), "Ctrl+X", false, has_selection)) {
            ImGui::SetClipboardText(selected.c_str());
            int start = get_input_selection_start();
            int end = get_input_selection_end();
            std::string current_text = input_buffer_;
            current_text.erase(start, end - start);
            strncpy(input_buffer_, current_text.c_str(), sizeof(input_buffer_) - 1);
            input_buffer_[sizeof(input_buffer_) - 1] = '\0';
            set_input_cursor_pos(start);
            set_input_selection_start(0);
            set_input_selection_end(0);
        }
        if (ImGui::MenuItem(TRF("context.copy", "Copy"), "Ctrl+C", false, has_selection)) {
            ImGui::SetClipboardText(selected.c_str());
        }
        if (ImGui::MenuItem(TRF("context.paste", "Paste"), "Ctrl+V")) {
            const char* clipboard = ImGui::GetClipboardText();
            if (clipboard && strlen(clipboard) > 0) {
                std::string current_text = input_buffer_;
                std::string paste_text = clipboard;

                if (has_selection) {
                    int start = get_input_selection_start();
                    int end = get_input_selection_end();
                    current_text.replace(start, end - start, paste_text);
                } else {
                    int cursor_pos = get_input_cursor_pos();
                    current_text.insert(cursor_pos, paste_text);
                }

                if (current_text.size() >= sizeof(input_buffer_)) {
                    current_text = current_text.substr(0, sizeof(input_buffer_) - 1);
                }

                strncpy(input_buffer_, current_text.c_str(), sizeof(input_buffer_) - 1);
                input_buffer_[sizeof(input_buffer_) - 1] = '\0';
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(TRF("context.clear", "Clear"), "Del")) {
            input_buffer_[0] = '\0';
        }
        ImGui::EndPopup();
    }

    if (ImGui::IsItemFocused()) {
        input_focused_ = true;
    } else {
        input_focused_ = false;
    }

    // Enter теперь только переводит строку (в InputTextMultiline)
    // Ctrl+Enter или кнопка "Отправить" для отправки
    bool ctrl_enter_pressed = ImGui::IsKeyPressed(ImGuiKey_Enter) && ImGui::GetIO().KeyCtrl;
    bool numpad_enter_pressed = ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) && ImGui::GetIO().KeyCtrl;

    ImGui::Spacing();

    bool model_loading = is_model_loading_ && is_model_loading_->load();
    if (model_loading && model_load_progress_) {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
        std::string status = model_load_status_ ? *model_load_status_ : "Загрузка...";
        ImGui::ProgressBar(*model_load_progress_, ImVec2(-1, 20), status.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    bool can_send = strlen(input_buffer_) > 0 && !streaming_active_ && !model_loading;

    if (model_loading) {
        ImGui::BeginDisabled();
    }

    // Кнопка "Отправить" — всегда работает
    bool send_button_pressed = ImGui::Button(TRF("chat.send", "Отправить"), ImVec2(100, 0));

    // Ctrl+Enter или Ctrl+NumpadEnter для отправки
    if (send_button_pressed || ctrl_enter_pressed || numpad_enter_pressed) {
        if (can_send) {
            send_message();
        }
    }

    if (model_loading) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Модель загружается, отправка заблокирована...");
        }
    }

    ImGui::SameLine();

    // Кнопка "Остановить" — всегда видима, активна только во время генерации
    bool stop_active = streaming_active_ || processing_rag_document_;
    if (!stop_active) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(TRF("chat.stop", "Остановить"), ImVec2(100, 0))) {
        stop_streaming();
    }
    if (!stop_active) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Остановить генерацию (активно во время ответа модели)");
        }
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(Ctrl+Enter)");

    ImGui::SameLine();

    if (ImGui::Button(TRF("chat.attach", "Attach"), ImVec2(100, 0))) {
        auto on_attach = [this](const std::string& file_path) {
            if (!file_path.empty()) {
                add_file_attachment(file_path);
            }
        };
        if (file_dialog_manager_) {
            file_dialog_manager_->pick_file("Select File to Attach", std::move(on_attach));
        } else {
            FileDialogHelper dialog_helper;
            dialog_helper.open_file_dialog("Select File to Attach", std::move(on_attach));
        }
    }

    ImGui::SameLine();

    if (ImGui::Button(TRF("chat.select_model", "Select Model"), ImVec2(120, 0))) {
        open_model_selection_dialog();
    }

    ImGui::SameLine();

    if (ImGui::Button(TRF("chat.clear", "Clear"), ImVec2(100, 0))) {
        clear_input();
    }

    ImGui::SameLine();

    float remaining_width = ImGui::GetContentRegionAvail().x;
    if (remaining_width > 120) {
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 120);
        if (ImGui::Checkbox(TRF("chat.auto_scroll", "Auto-scroll"), &auto_scroll_)) {
            if (auto_scroll_) {
                scroll_to_bottom();
            }
        }
    }

    // Render performance metrics line
    if (streaming_active_ || processing_rag_document_ || current_metrics_.is_measuring || current_metrics_.response_time_seconds > 0) {
        ImGui::Spacing();

        // Индикатор чтения RAG-документа
        if (processing_rag_document_) {
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "📄 %s",
                TRF("chat.processing_rag", "Reading document..."));
            ImGui::SameLine();
        }

        // Render performance metrics
        render_performance_metrics();
    }

    ImGui::EndChild();
}

void ChatInterface::render_attachments() {
    if (attachments_.empty()) {
        return;
    }

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                      TRF("chat.attachments", "Attachments:"));

    for (size_t i = 0; i < attachments_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        std::string filename = attachments_[i];
        size_t last_slash = filename.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            filename = filename.substr(last_slash + 1);
        }

        ImGui::Text("📎 %s", filename.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("×")) {
            remove_attachment(i);
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
}

} // namespace ui
} // namespace llama_gui

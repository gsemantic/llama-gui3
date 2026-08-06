#include "dialog_manager.h"
#include "../external/imgui/imgui.h"

namespace llama_gui {
namespace ui {

void DialogManager::renderHelpDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::BeginPopupModal(dialog.title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Llama GUI - Chat Interface for Language Models");
        ImGui::Separator();
        ImGui::Text("Version: 1.0.0");
        ImGui::Text("Build: Development");
        ImGui::Separator();
        ImGui::Text("This application provides a modern chat interface");
        ImGui::Text("for interacting with language models.");
        ImGui::Separator();
        ImGui::Text("Features:");
        ImGui::BulletText("Multiple conversation management");
        ImGui::BulletText("Model selection and configuration");
        ImGui::BulletText("Export/import conversations");
        ImGui::BulletText("Customizable interface");

        if (ImGui::Button("OK")) {
            dialog.visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
#endif
}

void DialogManager::renderAboutDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::BeginPopupModal(dialog.title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Llama GUI");
        ImGui::Separator();
        ImGui::Text("Version: 1.0.0");
        ImGui::Text("Build: Development");
        ImGui::Text("Framework: Dear ImGui + C++17");
        ImGui::Separator();
        ImGui::Text("A modern chat interface for language models.");
        ImGui::Text("Built with performance and usability in mind.");
        ImGui::Separator();

        if (ImGui::Button("OK")) {
            dialog.visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
#endif
}

void DialogManager::renderKeyboardShortcutsDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::BeginPopupModal(dialog.title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Keyboard Shortcuts");
        ImGui::Separator();

        // Список горячих клавиш
        ImGui::Text("File Operations:");
        ImGui::BulletText("Ctrl+N - New Chat");
        ImGui::BulletText("Ctrl+O - Open File");
        ImGui::BulletText("Ctrl+S - Save File");
        ImGui::BulletText("Alt+F4 - Exit");

        ImGui::Text("Edit Operations:");
        ImGui::BulletText("Ctrl+, - Open Settings");

        ImGui::Text("Chat Operations:");
        ImGui::BulletText("Enter - Send Message");
        ImGui::BulletText("Shift+Enter - New Line");
        ImGui::BulletText("Escape - Clear Input");

        if (ImGui::Button("OK")) {
            dialog.visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
#endif
}

void DialogManager::renderServerStatusDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::BeginPopupModal(dialog.title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Server Status");
        ImGui::Separator();
        ImGui::TextWrapped(dialog.message.c_str());

        if (!dialog.details.empty()) {
            ImGui::Separator();
            ImGui::Text("Details:");
            ImGui::TextWrapped(dialog.details.c_str());
        }

        if (ImGui::Button("OK")) {
            dialog.visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
#endif
}

void DialogManager::renderConfirmationDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::BeginPopupModal(dialog.title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(dialog.message.c_str());
        ImGui::Separator();

        float button_width = 100.0f;
        float total_width = button_width * dialog.buttons.size() + 20.0f * (dialog.buttons.size() - 1);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - total_width) * 0.5f);

        for (const auto& button : dialog.buttons) {
            if (ImGui::Button(button.text.c_str(), ImVec2(button_width, 0))) {
                dialog.visible = false;
                ImGui::CloseCurrentPopup();

                // Выполняем callback если есть
                if (button.callback) {
                    button.callback();
                }
            }
            if (&button != &dialog.buttons.back()) {
                ImGui::SameLine();
            }
        }

        ImGui::EndPopup();
    }
#endif
}

void DialogManager::renderInfoDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::BeginPopupModal(dialog.title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(dialog.message.c_str());

        if (!dialog.details.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped(dialog.details.c_str());
        }

        if (ImGui::Button("OK")) {
            dialog.visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
#endif
}

void DialogManager::renderErrorDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::BeginPopupModal(dialog.title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error:");
        ImGui::TextWrapped(dialog.message.c_str());

        if (!dialog.details.empty()) {
            ImGui::Separator();
            ImGui::Text("Details:");
            ImGui::TextWrapped(dialog.details.c_str());
        }

        if (ImGui::Button("OK")) {
            dialog.visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
#endif
}

void DialogManager::renderFileDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    // Простая реализация диалога файлов (в реальном проекте нужна более сложная логика)
    if (ImGui::BeginPopupModal(dialog.title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("File Dialog (Placeholder)");
        ImGui::Text("This is a simplified file dialog.");
        ImGui::Text("In a full implementation, this would show:");
        ImGui::BulletText("File browser");
        ImGui::BulletText("Filter options");
        ImGui::BulletText("Path navigation");

        if (ImGui::Button("OK")) {
            dialog.visible = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            dialog.visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
#endif
}

void DialogManager::renderDocumentationDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::Begin(dialog.title.c_str(), &dialog.visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Documentation dialog - TODO: Implement");
        if (ImGui::Button("Close")) {
            dialog.visible = false;
        }
    }
    ImGui::End();
#endif
}

void DialogManager::renderAboutDevsDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::Begin(dialog.title.c_str(), &dialog.visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Llama GUI - About Developers");
        ImGui::TextWrapped("Developed by the Llama GUI team");
        if (ImGui::Button("Close")) {
            dialog.visible = false;
        }
    }
    ImGui::End();
#endif
}

void DialogManager::renderAuditLogDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::Begin(dialog.title.c_str(), &dialog.visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Audit Log dialog - TODO: Implement");
        if (ImGui::Button("Close")) {
            dialog.visible = false;
        }
    }
    ImGui::End();
#endif
}

void DialogManager::renderConsoleDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::Begin(dialog.title.c_str(), &dialog.visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Console dialog - TODO: Implement");
        if (ImGui::Button("Close")) {
            dialog.visible = false;
        }
    }
    ImGui::End();
#endif
}

void DialogManager::renderCheckUpdatesDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::Begin(dialog.title.c_str(), &dialog.visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Check Updates dialog - TODO: Implement");
        if (ImGui::Button("Close")) {
            dialog.visible = false;
        }
    }
    ImGui::End();
#endif
}

void DialogManager::renderMetricsDialog(Dialog& dialog) {
#ifdef USE_IMGUI
    if (ImGui::Begin(dialog.title.c_str(), &dialog.visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Metrics dialog - TODO: Implement");
        if (ImGui::Button("Close")) {
            dialog.visible = false;
        }
    }
    ImGui::End();
#endif
}

} // namespace ui
} // namespace llama_gui

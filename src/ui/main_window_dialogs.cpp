#include "main_window.h"
#include "advanced_menu_system.h"
#include <iostream>
#include <cstring>

namespace llama_gui {
namespace ui {

void MainWindow::show_workspace_save_dialog() {
    show_workspace_save_dialog_ = true;
}

void MainWindow::show_workspace_load_dialog() {
    show_workspace_load_dialog_ = true;
}

void MainWindow::render_workspace_save_dialog() {
    if (!show_workspace_save_dialog_) {
        return;
    }

    if (ImGui::Begin("Save Workspace", &show_workspace_save_dialog_, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char workspace_name[256] = "";
        ImGui::InputText("Name", workspace_name, sizeof(workspace_name));

        bool name_exists = workspace_layout_manager_.exists(workspace_name);
        if (name_exists) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "'%s' already exists", workspace_name);
        }

        if (workspace_name[0] != '\0') {
            if (name_exists) {
                if (ImGui::Button("Overwrite")) {
                    save_workspace(workspace_name);
                    show_workspace_save_dialog_ = false;
                }
            } else {
                if (ImGui::Button("Save")) {
                    save_workspace(workspace_name);
                    workspace_name[0] = '\0';
                    show_workspace_save_dialog_ = false;
                }
            }
            ImGui::SameLine();
        }

        if (ImGui::Button("Cancel")) {
            show_workspace_save_dialog_ = false;
        }
    }
    ImGui::End();
}

void MainWindow::render_workspace_load_dialog() {
    if (!show_workspace_load_dialog_) {
        return;
    }

    if (ImGui::Begin("Load Workspace", &show_workspace_load_dialog_, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::vector<std::string> workspaces = workspace_layout_manager_.list();

        if (workspaces.empty()) {
            ImGui::TextWrapped("No workspaces found.");
        } else {
            for (const auto& ws : workspaces) {
                ImGui::PushID(ws.c_str());
                if (ImGui::Button("X", ImVec2(20, 0))) {
                    show_workspace_delete_confirm_ = true;
                    std::strncpy(workspace_overwrite_name_, ws.c_str(), sizeof(workspace_overwrite_name_) - 1);
                    workspace_overwrite_name_[sizeof(workspace_overwrite_name_) - 1] = '\0';
                }
                ImGui::SameLine();
                if (ImGui::Selectable(ws.c_str(), false)) {
                    load_workspace(ws);
                    show_workspace_load_dialog_ = false;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }

        if (ImGui::Button("Close")) {
            show_workspace_load_dialog_ = false;
        }
    }
    ImGui::End();

    // Диалог подтверждения удаления
    if (show_workspace_delete_confirm_) {
        ImGui::Begin("Confirm Delete", &show_workspace_delete_confirm_, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("Delete '%s'?", workspace_overwrite_name_);
        if (ImGui::Button("Yes, delete")) {
            delete_workspace(workspace_overwrite_name_);
            show_workspace_delete_confirm_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            show_workspace_delete_confirm_ = false;
        }
        ImGui::End();
    }
}

void MainWindow::show_about_dialog() {
    std::cout << "MainWindow: Showing about dialog" << std::endl;
    show_about_ = true;
}

void MainWindow::show_help_dialog() {
    std::cout << "MainWindow: Showing help dialog" << std::endl;
    show_help_ = true;
}

void MainWindow::open_model_directory_dialog() {
    std::cout << "MainWindow: Opening model directory dialog" << std::endl;
    // TODO: Implement model directory dialog
    show_info("Model Directory", "Model directory dialog not implemented yet");
}

void MainWindow::open_model_selection_dialog() {
    std::cout << "MainWindow: Opening model selection dialog" << std::endl;
    show_model_selection_dialog_ = true;
}

void MainWindow::render_model_selection_dialog() {
    if (!show_model_selection_dialog_) {
        return;
    }

    if (ImGui::Begin("Select Model", &show_model_selection_dialog_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Model selection dialog");
        
        if (ImGui::Button("OK")) {
            show_model_selection_dialog_ = false;
        }
        
        if (ImGui::Button("Cancel")) {
            show_model_selection_dialog_ = false;
        }
    }
    ImGui::End();
}

void MainWindow::load_model_from_path(const std::string& model_path) {
    std::cout << "MainWindow: Loading model from path: " << model_path << std::endl;

    // Update settings with new model path
    settings_.set_model_path(model_path);

    // Update server manager and restart with new model
    if (server_manager_) {
        server_manager_->set_model_path(model_path);
        server_manager_->restart_server();
        show_info("Model", "Модель переключена: " + model_path);
    }
}

void MainWindow::render_model_load_dialog() {
    if (!show_model_load_dialog_) {
        return;
    }

    if (ImGui::Begin("Loading Model", &show_model_load_dialog_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Loading model...");
        ImGui::ProgressBar(model_load_progress_, ImVec2(0, 0));
        ImGui::Text("%s", model_load_status_.c_str());

        if (model_load_progress_ >= 1.0f) {
            show_model_load_dialog_ = false;
        }
    }
    ImGui::End();
}

void MainWindow::reset_workspace() {
    // Сбрасываем все окна к значениям по умолчанию
    show_chat_ = true;
    show_conversations_ = false;
    show_files_ = false;
    show_rag_ = false;
    show_agents_ = false;
    show_settings_ = false;
    show_cloud_services_ = false;
    show_rag_settings_ = false;
    show_profile_manager_ = false;
    show_backup_manager_ = false;
    show_grid_snapping_ = false;

    std::cout << "✓ Workspace reset to default" << std::endl;
}

} // namespace ui
} // namespace llama_gui

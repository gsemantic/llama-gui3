/**
 * @file agent_panel.cpp
 * @brief Реализация панели управления агентами
 */

#include "ui/agent_panel.h"
#include "imgui.h"
#include "../include/ui/localization_manager.h"
#include <algorithm>

namespace llama_gui {
namespace ui {

// ============================================================================
// AgentPanel implementation
// ============================================================================

AgentPanel::AgentPanel() = default;

AgentPanel::~AgentPanel() = default;

bool AgentPanel::initialize(agents::AgentRegistry* registry) {
    if (!registry) {
        return false;
    }

    registry_ = registry;
    refresh_agents();

    return true;
}

void AgentPanel::render(bool* visible) {
    if (!visible || !*visible) {
        return;
    }

    // Позиция и размер устанавливаются в main_window_rendering_core.cpp перед вызовом render()
    // Не устанавливаем SetNextWindowSize здесь, чтобы не переопределять сохраненные размеры workspace

    // Стандартное окно ImGui с заголовком и кнопками управления
    // Кнопка закрытия (×) и сворачивания (─) рисуются автоматически ImGui
    if (!ImGui::Begin(TR("agents.title"), visible, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    // Statistics
    if (show_statistics_) {
        render_statistics();
    }

    // Список агентов
    ImGui::Text(TRF("agents.loaded_count", "Loaded Agents (%zu)"), agents_.size());
    ImGui::Separator();

    for (const auto& agent : agents_) {
        render_agent_item(agent);
    }

    // Help
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    ImGui::TextWrapped(TR("agents.select_agent"));
    ImGui::PopStyleColor();

    ImGui::End();
}

void AgentPanel::refresh_agents() {
    if (!registry_) {
        return;
    }
    
    agents_.clear();
    auto agent_list = registry_->list_agents();
    
    total_agents_ = static_cast<int>(agent_list.size());
    enabled_agents_ = 0;
    disabled_agents_ = 0;
    
    for (const auto& info : agent_list) {
        AgentInfoUI ui_info;
        ui_info.name = info.name;
        ui_info.description = info.description;
        ui_info.version = info.version;
        ui_info.is_builtin = info.is_builtin;
        ui_info.is_enabled = registry_->is_agent_enabled(info.name);
        ui_info.progress = 0.0f;
        
        // Определение статуса
        if (!ui_info.is_enabled) {
            ui_info.status = AgentStatusUI::Disabled;
            disabled_agents_++;
        } else {
            auto* agent = registry_->get_agent(info.name);
            if (agent && agent->is_ready()) {
                ui_info.status = AgentStatusUI::Ready;
                enabled_agents_++;
            } else {
                ui_info.status = AgentStatusUI::Error;
            }
        }
        
        agents_.push_back(ui_info);
    }
    
    // Сортировка: включённые первыми
    std::sort(agents_.begin(), agents_.end(),
              [](const AgentInfoUI& a, const AgentInfoUI& b) {
                  if (a.is_enabled != b.is_enabled) {
                      return a.is_enabled;
                  }
                  return a.name < b.name;
              });
}

void AgentPanel::render_agent_item(const AgentInfoUI& info) {
    ImGui::PushID(info.name.c_str());
    
    // Цвет статуса
    float r, g, b;
    get_status_color(info.status, &r, &g, &b);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(r, g, b, 1.0f));
    
    // Иконка статуса
    const char* status_icon = "";
    switch (info.status) {
        case AgentStatusUI::Ready:    status_icon = "[●] "; break;
        case AgentStatusUI::Busy:     status_icon = "[◍] "; break;
        case AgentStatusUI::Disabled: status_icon = "[○] "; break;
        case AgentStatusUI::Error:    status_icon = "[✖] "; break;
        default:                      status_icon = "[?] "; break;
    }
    
    // Кнопка агента
    bool is_selected = (selected_agent_ == info.name);
    if (is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.7f, 1.0f));
    }
    
    char button_label[256];
    snprintf(button_label, sizeof(button_label), "%s%s (%s)", 
             status_icon, info.name.c_str(), info.version);
    
    if (ImGui::Button(button_label, ImVec2(-100, 0))) {
        selected_agent_ = info.name;
        if (on_agent_selected_) {
            on_agent_selected_(info.name);
        }
    }
    
    if (is_selected) {
        ImGui::PopStyleColor();
    }
    
    ImGui::SameLine();
    
    // Toggle включения/выключения
    char toggle_label[32];
    snprintf(toggle_label, sizeof(toggle_label), "%s", info.is_enabled ? "Disable" : "Enable");
    
    if (ImGui::Button(toggle_label, ImVec2(80, 0))) {
        set_agent_enabled(info.name, !info.is_enabled);
        if (on_agent_action_) {
            on_agent_action_(info.name, info.is_enabled ? "disable" : "enable");
        }
    }
    
    ImGui::PopStyleColor();
    
    // Описание (если выбран)
    if (is_selected) {
        ImGui::Indent();
        ImGui::TextWrapped("  %s", info.description);

        if (info.is_builtin) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.0f, 1.0f));
            ImGui::Text(TR("agents.builtin_badge"));
            ImGui::PopStyleColor();
        }

        // Прогресс бар (если занят)
        if (info.status == AgentStatusUI::Busy && info.progress > 0) {
            ImGui::ProgressBar(info.progress, ImVec2(-1, 0));
        }

        // Действия
        ImGui::Separator();
        if (ImGui::Button(TR("button.settings"))) {
            render_agent_settings(info);
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("button.execute"))) {
            if (on_agent_action_) {
                on_agent_action_(info.name, "execute");
            }
        }
        ImGui::Unindent();
    }

    ImGui::Separator();

    ImGui::PopID();
}

void AgentPanel::render_agent_settings(const AgentInfoUI& info) {
    // Заглушка для будущих настроек агента
    ImGui::OpenPopup(TR("agents.settings_popup"));

    if (ImGui::BeginPopupModal(TR("agents.settings_popup"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(TRF("agents.settings_for", "Settings for: %s"), info.name.c_str());
        ImGui::Separator();
        ImGui::Text(TRF("agents.version", "Version: %s"), info.version);
        ImGui::Text(TRF("agents.builtin", "Built-in: %s"), info.is_builtin ? TR("general.yes") : TR("general.no"));
        ImGui::Text(TRF("agents.status_label", "Status: %s"), status_to_string(info.status));

        ImGui::Separator();
        ImGui::Text(TR("agents.settings_placeholder"));

        if (ImGui::Button(TR("button.close"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void AgentPanel::render_statistics() {
    ImGui::Separator();
    ImGui::Text(TR("agents.statistics_label"));
    ImGui::BulletText(TRF("agents.statistics_total", "Total agents: %d"), total_agents_);
    ImGui::BulletText(TRF("agents.statistics_enabled", "Enabled: %d"), enabled_agents_);
    ImGui::BulletText(TRF("agents.statistics_disabled", "Disabled: %d"), disabled_agents_);
    ImGui::Separator();
}

void AgentPanel::set_agent_enabled(const std::string& name, bool enabled) {
    if (!registry_) {
        return;
    }
    
    registry_->set_agent_enabled(name, enabled);
    refresh_agents();
}

AgentInfoUI AgentPanel::get_agent_info(const std::string& name) const {
    for (const auto& info : agents_) {
        if (info.name == name) {
            return info;
        }
    }
    
    AgentInfoUI empty;
    empty.status = AgentStatusUI::Unknown;
    return empty;
}

void AgentPanel::set_on_agent_selected(std::function<void(const std::string&)> callback) {
    on_agent_selected_ = std::move(callback);
}

void AgentPanel::set_on_agent_action(std::function<void(const std::string&, const std::string&)> callback) {
    on_agent_action_ = std::move(callback);
}

bool AgentPanel::is_visible() const {
    return visible_;
}

void AgentPanel::set_visible(bool visible) {
    visible_ = visible;
}

// ============================================================================
// Вспомогательные функции
// ============================================================================

const char* AgentPanel::status_to_string(AgentStatusUI status) const {
    switch (status) {
        case AgentStatusUI::Ready:    return "Ready";
        case AgentStatusUI::Busy:     return "Busy";
        case AgentStatusUI::Disabled: return "Disabled";
        case AgentStatusUI::Error:    return "Error";
        default:                      return "Unknown";
    }
}

void AgentPanel::get_status_color(AgentStatusUI status, float* r, float* g, float* b) const {
    switch (status) {
        case AgentStatusUI::Ready:
            *r = 0.0f; *g = 0.8f; *b = 0.0f;  // Green
            break;
        case AgentStatusUI::Busy:
            *r = 1.0f; *g = 0.8f; *b = 0.0f;  // Yellow
            break;
        case AgentStatusUI::Disabled:
            *r = 0.5f; *g = 0.5f; *b = 0.5f;  // Gray
            break;
        case AgentStatusUI::Error:
            *r = 0.8f; *g = 0.0f; *b = 0.0f;  // Red
            break;
        default:
            *r = 0.5f; *g = 0.5f; *b = 0.5f;  // Gray
            break;
    }
}

} // namespace ui
} // namespace llama_gui

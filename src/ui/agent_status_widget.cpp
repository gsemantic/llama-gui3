/**
 * @file agent_status_widget.cpp
 * @brief Реализация виджета статуса агентов
 */

#include "ui/agent_status_widget.h"
#include "../include/ui/localization_manager.h"
#include "imgui.h"
#include <iostream>

namespace llama_gui {
namespace ui {

// ============================================================================
// AgentStatusWidget implementation
// ============================================================================

AgentStatusWidget::AgentStatusWidget() = default;

AgentStatusWidget::~AgentStatusWidget() = default;

void AgentStatusWidget::initialize(agents::AgentRegistry* registry) {
    registry_ = registry;
    update();
}

void AgentStatusWidget::render() {
    if (!registry_) {
        return;
    }

    // Отрисовка в статусной строке
#ifdef USE_IMGUI
    ImGui::Separator();

    if (ImGui::BeginMainMenuBar()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));
        ImGui::Text(TR("agents.status_label_short"));
        ImGui::PopStyleColor();

        ImGui::SameLine();

        // Индикаторы агентов
        auto agent_list = registry_->list_agents();
        for (const auto& info : agent_list) {
            bool enabled = registry_->is_agent_enabled(info.name);
            auto* agent = registry_->get_agent(info.name);
            bool ready = agent && agent->is_ready();

            render_agent_indicator(info.name, enabled, ready);
            ImGui::SameLine();
        }

        // Активный агент
        if (!active_agent_.empty()) {
            ImGui::SameLine();
            ImGui::Separator();
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
            ImGui::Text(TRF("agents.active_count", "Active: %s"), active_agent_.c_str());
            ImGui::PopStyleColor();
        }

        // Статистика
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();

        ImGui::Text(TRF("agents.count_format", "%d/%d"), active_count_, total_count_);

        ImGui::EndMainMenuBar();
    }
#else
    // Для тестов просто выводим информацию
    std::cout << "Agents: " << active_count_ << "/" << total_count_;
    if (!active_agent_.empty()) {
        std::cout << " Active: " << active_agent_;
    }
    std::cout << std::endl;
#endif
}

void AgentStatusWidget::update() {
    if (!registry_) {
        return;
    }
    
    auto agent_list = registry_->list_agents();
    total_count_ = static_cast<int>(agent_list.size());
    active_count_ = 0;
    
    for (const auto& info : agent_list) {
        if (registry_->is_agent_enabled(info.name)) {
            auto* agent = registry_->get_agent(info.name);
            if (agent && agent->is_ready()) {
                active_count_++;
            }
        }
    }
}

void AgentStatusWidget::set_active_agent(const std::string& name) {
    active_agent_ = name;
}

void AgentStatusWidget::clear_active_agent() {
    active_agent_.clear();
}

int AgentStatusWidget::get_active_count() const {
    return active_count_;
}

int AgentStatusWidget::get_total_count() const {
    return total_count_;
}

// ============================================================================
// Вспомогательные функции
// ============================================================================

void AgentStatusWidget::render_agent_indicator(
        const std::string& name, bool enabled, bool ready) {
    
    // Цвет индикатора
    ImVec4 color;
    const char* tooltip = "";
    
    if (!enabled) {
        color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);  // Gray - disabled
        tooltip = "Disabled";
    } else if (!ready) {
        color = ImVec4(0.8f, 0.0f, 0.0f, 1.0f);  // Red - error
        tooltip = "Error";
    } else {
        color = ImVec4(0.0f, 0.8f, 0.0f, 1.0f);  // Green - ready
        tooltip = "Ready";
    }
    
    // Если это активный агент - мигающий индикатор
    if (name == active_agent_) {
        float alpha = 0.5f + 0.5f * ImGui::GetTime();  // Blink
        color.w = alpha;
    }
    
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Text("[●]");
    ImGui::PopStyleColor();
    
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%s: %s", tooltip, name.c_str());
        ImGui::EndTooltip();
    }
}

} // namespace ui
} // namespace llama_gui

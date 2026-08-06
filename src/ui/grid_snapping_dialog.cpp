#include "../include/ui/grid_snapping_dialog.h"
#include "../include/ui/localization_manager.h"
#include "imgui.h"
#include <iostream>

namespace llama_gui {
namespace ui {

void GridSnappingDialog::render(bool* open) {
    if (!show_ || !grid_system_) {
        return;
    }

    const auto& settings = grid_system_->getSettings();
    
    // Инициализация временных настроек при первом открытии
    if (temp_grid_size_ == 0) {
        temp_grid_size_ = settings.grid_size;
        temp_fine_divisor_ = settings.fine_grid_divisor;
        temp_snap_threshold_ = settings.snap_threshold;
    }

    if (!ImGui::Begin(TR("grid_snapping.title"), open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (open && !*open) show_ = false;
        return;
    }

    // Заголовок
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), TR("grid_snapping.description"));
    ImGui::Separator();
    ImGui::Spacing();

    // Включение/выключение примагничивания
    if (ImGui::Checkbox(TR("grid_snapping.enable"), &const_cast<bool&>(settings.enabled))) {
        grid_system_->setEnabled(settings.enabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("grid_snapping.enable.tooltip"));
        ImGui::EndTooltip();
    }
    ImGui::Spacing();

    // Размер сетки
    ImGui::Text(TR("grid_snapping.grid_size"));
    if (ImGui::SliderInt("##grid_size_slider", &temp_grid_size_, 4, 64, "%d px")) {
        grid_system_->setGridSize(temp_grid_size_);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("grid_snapping.grid_size.tooltip"));
        ImGui::EndTooltip();
    }
    ImGui::Spacing();

    // Режим точной подстройки (fine-tuning)
    ImGui::Text(TR("grid_snapping.fine_tuning"));
    if (ImGui::Checkbox(TR("grid_snapping.fine_tuning.enable"), &const_cast<bool&>(settings.enable_fine_tuning))) {
        grid_system_->setFineTuningMode(settings.enable_fine_tuning, settings.fine_grid_divisor);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("grid_snapping.fine_tuning.tooltip"));
        ImGui::EndTooltip();
    }
    
    // Делитель мелкой сетки
    if (settings.enable_fine_tuning) {
        ImGui::SameLine();
        ImGui::Text("  ");
        ImGui::SameLine();
        if (ImGui::SliderInt(TR("grid_snapping.fine_divisor"), &temp_fine_divisor_, 2, 16, "%d")) {
            grid_system_->setFineTuningMode(settings.enable_fine_tuning, temp_fine_divisor_);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text(TR("grid_snapping.fine_divisor.tooltip"), 
                       grid_system_->getFineGridSize());
            ImGui::EndTooltip();
        }
        
        // Информация о текущем размере мелкой сетки
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), 
                          TR("grid_snapping.current_fine_size"), 
                          grid_system_->getFineGridSize());
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Примагничивание позиции
    if (ImGui::Checkbox(TR("grid_snapping.snap_position"), &const_cast<bool&>(settings.snap_position))) {
        // Настройка применена
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("grid_snapping.snap_position.tooltip"));
        ImGui::EndTooltip();
    }

    // Примагничивание размера
    if (ImGui::Checkbox(TR("grid_snapping.snap_size"), &const_cast<bool&>(settings.snap_size))) {
        // Настройка применена
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("grid_snapping.snap_size.tooltip"));
        ImGui::EndTooltip();
    }
    ImGui::Spacing();

    // Порог примагничивания
    ImGui::Text(TR("grid_snapping.snap_threshold"));
    if (ImGui::SliderFloat("##snap_threshold_slider", &temp_snap_threshold_, 1.0f, 32.0f, "%.1f px")) {
        grid_system_->setSnapThreshold(temp_snap_threshold_);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("grid_snapping.snap_threshold.tooltip"));
        ImGui::EndTooltip();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Визуализация сетки
    if (ImGui::Checkbox(TR("grid_snapping.show_grid_overlay"), &const_cast<bool&>(settings.show_grid_overlay))) {
        grid_system_->setShowGridOverlay(settings.show_grid_overlay);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("grid_snapping.show_grid_overlay.tooltip"));
        ImGui::EndTooltip();
    }
    ImGui::Spacing();

    // Кнопки действий
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button(TR("grid_snapping.snap_all"))) {
        // Вызов callback для примагничивания всех окон
        if (snap_all_callback_) {
            snap_all_callback_();
            std::cout << "GridSnappingDialog: Snap all windows requested" << std::endl;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("grid_snapping.snap_all.tooltip"));
        ImGui::EndTooltip();
    }
    
    ImGui::SameLine();
    
    if (ImGui::Button(TR("grid_snapping.reset"))) {
        grid_system_->resetToDefaults();
        temp_grid_size_ = grid_system_->getGridSize();
        temp_fine_divisor_ = grid_system_->getSettings().fine_grid_divisor;
        temp_snap_threshold_ = grid_system_->getSettings().snap_threshold;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text(TR("grid_snapping.reset.tooltip"));
        ImGui::EndTooltip();
    }
    
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();

    if (ImGui::Button(TR("close"))) {
        show_ = false;
        *open = false;
    }

    ImGui::End();
}

} // namespace ui
} // namespace llama_gui

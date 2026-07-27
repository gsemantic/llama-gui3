#pragma once

#include <string>
#include <memory>
#include <vector>
#include "core/settings.h"

namespace llama_gui {
namespace ui {

using Settings = llama_gui::core::Settings;

/**
 * @brief Диалог расширенных настроек
 * 
 * Содержит сгруппированные настройки в категориях:
 * - GPU & Hardware (GPU, Cache)
 * - Sampling & Generation (Sampling Basic, Sampling Advanced, Context, RoPE)
 * - Model & Server (Model Loading, Batch, Server Runtime, Grammar, Control Vectors)
 * - System (Logging, Performance, Advanced, Output, Tensor Override)
 */
class AdvancedSettingsDialog {
public:
    AdvancedSettingsDialog(Settings& settings);
    ~AdvancedSettingsDialog();

    // Управление отображением
    void show() { show_dialog_ = true; }
    void hide() { show_dialog_ = false; }
    bool is_visible() const { return show_dialog_; }

    // Отрисовка
    void render();

    // Показать конкретную вкладку
    enum class SettingsTab {
        GPU,
        Cache,
        SamplingBasic,
        SamplingAdvanced,
        Context,
        RoPE,
        ModelLoading,
        Batch,
        ServerRuntime,
        Grammar,
        ControlVectors,
        Logging,
        Performance,
        Advanced,
        Output,
        TensorOverride
    };

    void show_tab(SettingsTab tab) {
        // Если диалог уже открыт и вкладка той же группы - не сбрасываем состояние
        bool already_open = show_dialog_;
        bool same_group = false;
        
        if (already_open) {
            // Проверяем, та же ли это группа
            if ((tab == SettingsTab::GPU || tab == SettingsTab::Cache) && gpu_hardware_expanded_) same_group = true;
            else if ((tab == SettingsTab::SamplingBasic || tab == SettingsTab::SamplingAdvanced || 
                      tab == SettingsTab::Context || tab == SettingsTab::RoPE) && sampling_generation_expanded_) same_group = true;
            else if ((tab == SettingsTab::ModelLoading || tab == SettingsTab::Batch || 
                      tab == SettingsTab::ServerRuntime || tab == SettingsTab::Grammar || 
                      tab == SettingsTab::ControlVectors) && model_server_expanded_) same_group = true;
            else if ((tab == SettingsTab::Logging || tab == SettingsTab::Performance || 
                      tab == SettingsTab::Advanced || tab == SettingsTab::Output || 
                      tab == SettingsTab::TensorOverride) && system_expanded_) same_group = true;
        }
        
        show_dialog_ = true;
        current_tab_ = tab;
        
        // Раскрываем нужную группу только если это не та же самая группа
        if (!already_open || !same_group) {
            gpu_hardware_expanded_ = false;
            sampling_generation_expanded_ = false;
            model_server_expanded_ = false;
            system_expanded_ = false;
            switch (tab) {
                case SettingsTab::GPU:
                case SettingsTab::Cache:
                    gpu_hardware_expanded_ = true;
                    break;
                case SettingsTab::SamplingBasic:
                case SettingsTab::SamplingAdvanced:
                case SettingsTab::Context:
                case SettingsTab::RoPE:
                    sampling_generation_expanded_ = true;
                    break;
                case SettingsTab::ModelLoading:
                case SettingsTab::Batch:
                case SettingsTab::ServerRuntime:
                case SettingsTab::Grammar:
                case SettingsTab::ControlVectors:
                    model_server_expanded_ = true;
                    break;
                case SettingsTab::Logging:
                case SettingsTab::Performance:
                case SettingsTab::Advanced:
                case SettingsTab::Output:
                case SettingsTab::TensorOverride:
                    system_expanded_ = true;
                    break;
            }
        }
    }

    // Получить текущую вкладку
    SettingsTab get_current_tab() const { return current_tab_; }

private:
    // Группы настроек
    void render_gpu_hardware_group();
    void render_sampling_generation_group();
    void render_model_server_group();
    void render_system_group();

    // Отдельные вкладки настроек
    void render_gpu_tab();
    void render_cache_tab();
    void render_sampling_basic_tab();
    void render_sampling_advanced_tab();
    void render_context_tab();
    void render_rope_tab();
    void render_model_loading_tab();
    void render_batch_tab();
    void render_server_runtime_tab();
    void render_grammar_tab();
    void render_control_vectors_tab();
    void render_logging_tab();
    void render_performance_tab();
    void render_advanced_tab();
    void render_output_tab();
    void render_tensor_override_tab();

    // Обработчики
    void save_settings();
    void apply_settings();
    void reset_settings();
    void cancel_settings();

    // Вспомогательные методы
    static void HelpMarker(const std::string& desc);

    // Ссылка на настройки
    Settings& settings_;

    // Состояние UI
    bool show_dialog_ = false;
    bool settings_modified_ = false;
    std::string status_message_;

    // Состояние раскрытия групп - по умолчанию раскрываем GPU & Hardware
    bool gpu_hardware_expanded_ = true;
    bool sampling_generation_expanded_ = false;
    bool model_server_expanded_ = false;
    bool system_expanded_ = false;

    // Текущая выбранная вкладка
    SettingsTab current_tab_ = SettingsTab::GPU;
};

} // namespace ui
} // namespace llama_gui

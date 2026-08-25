#pragma once

#include <string>
#include <memory>
#include <functional>
#include "core/settings.h"
#include "core/server_manager.h"
#include "system_prompt_settings.h"
#include "quick_settings_dialog.h"
#include "advanced_settings_dialog.h"
#include "workspace_manager.h"
#include "cloud_services_dialog.h"

// Forward declarations for new settings dialogs
namespace llama_gui {
namespace core {
    class ConfigManager;
}
namespace ui {
    class ModelSettingsDialog;
    class GPUSettingsDialog;
    class SamplingBasicDialog;
    class SamplingAdvancedDialog;
    class ContextDialog;
    class RoPEDialog;
    class AdvancedDialog;
    class GrammarDialog;
    class ServerRuntimeDialog;
    class BatchDialog;
    class LoggingDialog;
}
}

namespace llama_gui {
namespace ui {

using Settings = llama_gui::core::Settings;
using ServerManager = llama_gui::core::ServerManager;

/**
 * Settings dialog for configuring application preferences
 *
 * Рефакторинг: вместо 16+ вкладок сразу - два уровня:
 * - Quick Settings (быстрые настройки): Server, Chat, Models, UI
 * - Advanced Settings (расширенные): сгруппированные по категориям
 */
class SettingsDialog {
public:
    SettingsDialog(Settings& settings, ServerManager* server_manager = nullptr, WorkspaceManager* workspace_manager = nullptr, llama_gui::core::ConfigManager* config_manager = nullptr);
    ~SettingsDialog();

    // Main rendering
    void render();

    // Dialog control
    void show() {
        saved_model_path_ = settings_.get_model_path();
        show_dialog_ = true;
    }
    void hide() {
        if (settings_modified_) {
            save_settings();
            settings_modified_ = false;
        }
        // Always check for model change (ModelSettingsDialog sets its own modified_ flag)
        check_model_changed();
        show_dialog_ = false;
    }
    bool is_visible() const { return show_dialog_; }

    // Показать быстрые настройки
    void show_quick_settings() {
        if (quick_settings_dialog_) {
            quick_settings_dialog_->show();
        }
    }

    // Показать расширенные настройки
    void show_advanced_settings() {
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show();
        }
    }

    // Установить ссылку на CloudServicesDialog (для кнопки в settings_dialog_server_chat.cpp)
    void set_cloud_services_dialog(CloudServicesDialog* dlg) { cloud_services_dialog_ = dlg; }

    // Callback для переподключения LlamaInterface после запуска сервера
    void set_server_started_callback(std::function<void()> callback) { server_started_callback_ = std::move(callback); }

    // Callback для уведомления о смене модели (для перезапуска сервера)
    void set_model_changed_callback(std::function<void(const std::string&)> callback) { model_changed_callback_ = std::move(callback); }

    // Колбэк для выбора файла модели (browse button)
    void setModelBrowseCallback(std::function<void(std::function<void(const std::string&)>)> cb);

    // =========================================================================
    // Методы для открытия конкретных вкладок настроек из меню
    // =========================================================================

    // Быстрые настройки (Quick Settings)
    void show_server_settings() {
        show_dialog_ = true;
        show_quick_ = true;
        current_tab_ = SettingsTab::Server;
    }

    void show_chat_settings() {
        show_dialog_ = true;
        show_quick_ = true;
        current_tab_ = SettingsTab::Chat;
    }

    void show_models_settings() {
        show_dialog_ = true;
        show_quick_ = true;
        current_tab_ = SettingsTab::Models;
    }

    void show_ui_settings() {
        show_dialog_ = true;
        show_quick_ = true;
        current_tab_ = SettingsTab::UI;
    }

    // Безопасность: SSL/TLS и токен доступа (ранее вкладка не была достижима)
    void show_security_settings() {
        show_dialog_ = true;
        show_quick_ = true;
        current_tab_ = SettingsTab::Security;
    }

    // Настройки GPU и оборудования (GPU & Hardware)
    void show_gpu_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::GPU);
        }
    }

    void show_cache_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::Cache);
        }
    }

    // Настройки сэмплирования и генерации (Sampling & Generation)
    void show_sampling_basic_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::SamplingBasic);
        }
    }

    void show_sampling_advanced_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::SamplingAdvanced);
        }
    }

    void show_context_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::Context);
        }
    }

    void show_rope_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::RoPE);
        }
    }

    // Настройки модели и сервера (Model & Server)
    void show_model_loading_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::ModelLoading);
        }
    }

    void show_batch_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::Batch);
        }
    }

    void show_server_runtime_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::ServerRuntime);
        }
    }

    void show_grammar_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::Grammar);
        }
    }

    void show_control_vectors_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::ControlVectors);
        }
    }

    // Системные настройки (System)
    void show_logging_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::Logging);
        }
    }

    void show_performance_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::Performance);
        }
    }

    void show_advanced_settings_tab() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::Advanced);
        }
    }

    void show_output_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::Output);
        }
    }

    void show_tensor_override_settings() {
        show_dialog_ = true;
        show_quick_ = false;
        if (advanced_settings_dialog_) {
            advanced_settings_dialog_->show_tab(AdvancedSettingsDialog::SettingsTab::TensorOverride);
        }
    }

private:
    // Старые методы (для обратной совместимости)
    void render_server_settings();
    void render_chat_settings();
    void render_ui_settings();
    void render_file_settings();
    void render_performance_settings();
    void render_security_settings();
    void render_model_settings();
    void render_advanced_settings();

    // Button handlers
    void save_settings();
    void reset_settings();
    void apply_settings();
    void cancel_settings();
    void check_model_changed();

    // Backup for rollback
    void backup_current_settings();
    void restore_backup();

    // Helper
    static void HelpMarker(const std::string& desc);

    // References
    Settings& settings_;
    ServerManager* server_manager_;
    llama_gui::core::ConfigManager* config_manager_;
    std::function<void()> server_started_callback_;
    std::function<void(const std::string&)> model_changed_callback_;

    // System prompt settings module
    std::unique_ptr<SystemPromptSettings> system_prompt_settings_;

    // =========================================================================
    // Новые диалоги (двухуровневая система)
    // =========================================================================

    // Диалог быстрых настроек
    std::unique_ptr<QuickSettingsDialog> quick_settings_dialog_;

    // Диалог расширенных настроек
    std::unique_ptr<AdvancedSettingsDialog> advanced_settings_dialog_;

    // Cloud services dialog pointer (set from MainWindow)
    CloudServicesDialog* cloud_services_dialog_ = nullptr;

    // =========================================================================
    // Старые диалоги (для обратной совместимости, будут удалены)
    // =========================================================================

    // Model settings module
    std::unique_ptr<ModelSettingsDialog> model_settings_dialog_;

    // GPU settings module
    std::unique_ptr<GPUSettingsDialog> gpu_settings_dialog_;

    // Sampling settings modules
    std::unique_ptr<SamplingBasicDialog> sampling_basic_dialog_;
    std::unique_ptr<SamplingAdvancedDialog> sampling_advanced_dialog_;

    // Context and RoPE settings modules
    std::unique_ptr<ContextDialog> context_dialog_;
    std::unique_ptr<RoPEDialog> rope_dialog_;

    // Advanced and Grammar settings modules
    std::unique_ptr<AdvancedDialog> advanced_dialog_;
    std::unique_ptr<GrammarDialog> grammar_dialog_;

    // Server Runtime, Batch and Logging settings modules
    std::unique_ptr<ServerRuntimeDialog> server_runtime_dialog_;
    std::unique_ptr<BatchDialog> batch_dialog_;
    std::unique_ptr<LoggingDialog> logging_dialog_;

    // Workspace manager for mode checking
    WorkspaceManager* workspace_manager_ = nullptr;

    // UI state
    bool show_dialog_ = false;
    bool show_quick_ = true; // По умолчанию показываем быстрые настройки

    // Model path tracking for change detection
    std::string saved_model_path_;
    
    // Текущая активная вкладка
    enum class SettingsTab {
        // Quick Settings
        Server,
        Chat,
        Models,
        UI,
        Security,
        // Advanced Settings
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
    
    SettingsTab current_tab_ = SettingsTab::Server;
    bool settings_modified_ = false;
    std::string backup_json_;
    std::string status_message_;
};

} // namespace ui
} // namespace llama_gui

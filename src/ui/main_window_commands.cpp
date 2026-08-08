#include "main_window.h"
#include "commands_workspace.h"
#include "commands_settings.h"
#include <iostream>

namespace llama_gui {
namespace ui {

void MainWindow::connectWorkspaceCommands() {
    std::cout << "Connecting workspace commands..." << std::endl;

    registerCommand("save_workspace", CommandFactory::createFunctionalCommand(
        "save_workspace",
        [this]() {
            // Быстрое сохранение в текущее workspace (или "default")
            std::string name = workspace_layout_manager_.currentName();
            if (name.empty()) name = "default";
            save_workspace(name);
        },
        "Сохранить текущую конфигурацию окон",
        "",
        nullptr
    ));

    registerCommand("save_workspace_as", CommandFactory::createFunctionalCommand(
        "save_workspace_as",
        [this]() { show_workspace_save_dialog(); },
        "Сохранить с произвольным именем",
        "",
        nullptr
    ));

    registerCommand("load_workspace", CommandFactory::createFunctionalCommand(
        "load_workspace",
        [this]() { show_workspace_load_dialog(); },
        "Загрузить конфигурацию default",
        "",
        nullptr
    ));

    registerCommand("load_workspace_as", CommandFactory::createFunctionalCommand(
        "load_workspace_as",
        [this]() { show_workspace_load_dialog(); },
        "Выбрать конфигурацию из списка",
        "",
        nullptr
    ));

    registerCommand("reset_workspace", CommandFactory::createFunctionalCommand(
        "reset_workspace",
        [this]() { reset_workspace(); },
        "Сбросить к настройкам по умолчанию",
        "",
        nullptr
    ));

    std::cout << "✓ Connected workspace commands" << std::endl;
}

void MainWindow::connectAdditionalWindowCommands() {
    std::cout << "Connecting additional window commands..." << std::endl;

    try {
        registerCommand("toggle_quick_settings", CommandFactory::createFunctionalCommand(
            "toggle_quick_settings",
            [this]() {
                quick_settings_dialog_->show();
            },
            "Quick Settings",
            "",
            nullptr
        ));

        // Commands referenced by menu items
        registerCommand("show_profile_manager", CommandFactory::createFunctionalCommand(
            "show_profile_manager",
            [this]() { show_profile_manager(); },
            "Open Profile Manager", "Ctrl+Shift+P", nullptr
        ));

        registerCommand("show_backup_manager", CommandFactory::createFunctionalCommand(
            "show_backup_manager",
            [this]() {
                if (backup_manager_dialog_) {
                    backup_manager_dialog_->setOpen(true);
                    window_coordinator_.bringToFront("backup_manager");
                }
            },
            "Open Backup Manager", "Ctrl+Shift+B", nullptr
        ));

        registerCommand("save_current_profile", CommandFactory::createFunctionalCommand(
            "save_current_profile",
            [this]() { save_current_profile(); },
            "Save Current Profile", "Ctrl+Shift+S", nullptr
        ));

        registerCommand("show_about_devs", CommandFactory::createFunctionalCommand(
            "show_about_devs",
            [this]() { dialog_manager_.showAboutDevsDialog(); },
            "Show About Developers", "", nullptr
        ));

        registerCommand("show_documentation", CommandFactory::createFunctionalCommand(
            "show_documentation",
            [this]() { dialog_manager_.showDocumentationDialog(); },
            "Open Documentation", "", nullptr
        ));

        registerCommand("show_audit_log", CommandFactory::createFunctionalCommand(
            "show_audit_log",
            [this]() { dialog_manager_.showAuditLogDialog(); },
            "Show Audit Log", "", nullptr
        ));

        registerCommand("show_console", CommandFactory::createFunctionalCommand(
            "show_console",
            [this]() { dialog_manager_.showConsoleDialog(); },
            "Show Console", "", nullptr
        ));

        registerCommand("toggle_fullscreen", CommandFactory::createFunctionalCommand(
            "toggle_fullscreen",
            [this]() { toggle_fullscreen(); },
            "Toggle Fullscreen", "F11", nullptr
        ));

        // Grid Snapping (реальное переключение диалога примагничивания)
        registerCommand("show_grid_snapping_dialog", CommandFactory::createFunctionalCommand(
            "show_grid_snapping_dialog",
            [this]() {
                show_grid_snapping_ = !show_grid_snapping_;
                window_manager_.setWindowVisible("grid_snapping", show_grid_snapping_);
                if (show_grid_snapping_) {
                    grid_snapping_dialog_->show();
                    window_coordinator_.bringToFront("grid_snapping");
                }
            },
            "Grid Snapping", "", nullptr));

        // toggle_window_grid_snapping — используется меню Window (createWindowToggleItem)
        registerCommand("toggle_window_grid_snapping", CommandFactory::createFunctionalCommand(
            "toggle_window_grid_snapping",
            [this]() {
                window_manager_.toggleWindow("grid_snapping");
                if (window_manager_.isWindowVisible("grid_snapping")) {
                    grid_snapping_dialog_->show();
                    window_coordinator_.bringToFront("grid_snapping");
                }
                syncWindowFlagsFromManager();
            },
            "Toggle Grid Snapping Window", "", nullptr));

        // Console (реальная команда — открывает диалог консоли)
        registerCommand("toggle_console", CommandFactory::createFunctionalCommand(
            "toggle_console",
            [this]() { dialog_manager_.showConsoleDialog(); },
            "Toggle Console", "", nullptr
        ));

#ifdef ENABLE_LLAMA_BENCH
        // Llama Bench (реальный диалог сравнения моделей)
        registerCommand("open_llama_bench", CommandFactory::createFunctionalCommand(
            "open_llama_bench",
            [this]() { openLlamaBenchDialog(); },
            "Open Llama Bench", "", nullptr
        ));
#endif

        std::cout << "✓ Connected additional window commands" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "ERROR in connectAdditionalWindowCommands(): " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "UNKNOWN ERROR in connectAdditionalWindowCommands()" << std::endl;
    }
}

void MainWindow::connectSettingsMenuCommands() {
    std::cout << "Connecting settings menu commands..." << std::endl;

    int commands_count = 0;

    registerCommand("open_settings_server", CommandFactory::createFunctionalCommand(
        "open_settings_server",
        [this]() { settings_dialog_->show_server_settings(); },
        "Open Server Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_chat", CommandFactory::createFunctionalCommand(
        "open_settings_chat",
        [this]() { settings_dialog_->show_chat_settings(); },
        "Open Chat Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_models", CommandFactory::createFunctionalCommand(
        "open_settings_models",
        [this]() { settings_dialog_->show_models_settings(); },
        "Open Model Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_ui", CommandFactory::createFunctionalCommand(
        "open_settings_ui",
        [this]() { settings_dialog_->show_ui_settings(); },
        "Open UI Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_cloud", CommandFactory::createFunctionalCommand(
        "open_settings_cloud",
        [this]() {
            cloud_services_dialog_->open();
            window_coordinator_.bringToFront("cloud_services");
        },
        "Open Cloud Services Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_gpu", CommandFactory::createFunctionalCommand(
        "open_settings_gpu",
        [this]() { settings_dialog_->show_gpu_settings(); },
        "Open GPU Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_cache", CommandFactory::createFunctionalCommand(
        "open_settings_cache",
        [this]() { settings_dialog_->show_cache_settings(); },
        "Open Cache Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_sampling_basic", CommandFactory::createFunctionalCommand(
        "open_settings_sampling_basic",
        [this]() { settings_dialog_->show_sampling_basic_settings(); },
        "Open Basic Sampling Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_sampling_advanced", CommandFactory::createFunctionalCommand(
        "open_settings_sampling_advanced",
        [this]() { settings_dialog_->show_sampling_advanced_settings(); },
        "Open Advanced Sampling Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_context", CommandFactory::createFunctionalCommand(
        "open_settings_context",
        [this]() { settings_dialog_->show_context_settings(); },
        "Open Context Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_rope", CommandFactory::createFunctionalCommand(
        "open_settings_rope",
        [this]() { settings_dialog_->show_rope_settings(); },
        "Open RoPE Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_model_loading", CommandFactory::createFunctionalCommand(
        "open_settings_model_loading",
        [this]() { settings_dialog_->show_model_loading_settings(); },
        "Open Model Loading Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_batch", CommandFactory::createFunctionalCommand(
        "open_settings_batch",
        [this]() { settings_dialog_->show_batch_settings(); },
        "Open Batch Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_server_runtime", CommandFactory::createFunctionalCommand(
        "open_settings_server_runtime",
        [this]() { settings_dialog_->show_server_runtime_settings(); },
        "Open Server Runtime Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_grammar", CommandFactory::createFunctionalCommand(
        "open_settings_grammar",
        [this]() { settings_dialog_->show_grammar_settings(); },
        "Open Grammar Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_control_vectors", CommandFactory::createFunctionalCommand(
        "open_settings_control_vectors",
        [this]() { settings_dialog_->show_control_vectors_settings(); },
        "Open Control Vectors Settings",
        "",
        nullptr
    ));

    // =========================================================================
    // Системные настройки (System) — реальные вкладки AdvancedSettingsDialog
    // =========================================================================

    registerCommand("open_settings_logging", CommandFactory::createFunctionalCommand(
        "open_settings_logging",
        [this]() { settings_dialog_->show_logging_settings(); },
        "Open Logging Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_performance", CommandFactory::createFunctionalCommand(
        "open_settings_performance",
        [this]() { settings_dialog_->show_performance_settings(); },
        "Open Performance Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_advanced", CommandFactory::createFunctionalCommand(
        "open_settings_advanced",
        [this]() { settings_dialog_->show_advanced_settings_tab(); },
        "Open Advanced Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_output", CommandFactory::createFunctionalCommand(
        "open_settings_output",
        [this]() { settings_dialog_->show_output_settings(); },
        "Open Output Settings",
        "",
        nullptr
    ));

    registerCommand("open_settings_tensor_override", CommandFactory::createFunctionalCommand(
        "open_settings_tensor_override",
        [this]() { settings_dialog_->show_tensor_override_settings(); },
        "Open Tensor Override Settings",
        "",
        nullptr
    ));

    // Меню Performance и Logging (Admin workspace) используют те же команды,
    // что и вкладки System в меню Settings.

    commands_count = 21;

    std::cout << "✓ Connected settings menu commands (" << commands_count << " commands)" << std::endl;
}

void MainWindow::connectDeveloperCommands() {
    std::cout << "Connecting developer commands..." << std::endl;

    // Реальные инструменты Dear ImGui (окна создаются в renderDeveloperTools())
    registerCommand("show_metrics", CommandFactory::createFunctionalCommand(
        "show_metrics",
        [this]() { show_metrics_window_ = true; },
        "Show Metrics/Debugger", "", nullptr
    ));

    registerCommand("show_style_editor", CommandFactory::createFunctionalCommand(
        "show_style_editor",
        [this]() { show_style_editor_window_ = true; },
        "Show Style Editor", "", nullptr
    ));

    registerCommand("show_font_selector", CommandFactory::createFunctionalCommand(
        "show_font_selector",
        [this]() { show_font_selector_window_ = true; },
        "Show Font Selector", "", nullptr
    ));

    registerCommand("show_debug_log", CommandFactory::createFunctionalCommand(
        "show_debug_log",
        [this]() { show_debug_log_window_ = true; },
        "Show Debug Log", "", nullptr
    ));

    registerCommand("start_item_picker", CommandFactory::createFunctionalCommand(
        "start_item_picker",
        []() { ImGui::DebugStartItemPicker(); },
        "Start Item Picker", "", nullptr
    ));

    std::cout << "✓ Connected developer commands" << std::endl;
}

void MainWindow::connectStubCommands() {
    std::cout << "Connecting stub commands (show 'not implemented' message)..." << std::endl;

    auto stub = [this](const std::string& feature_name) {
        return [this, feature_name]() {
            dialog_manager_.showInfo("Заглушка",
                "Функция '" + feature_name + "' ещё не реализована.");
        };
    };

    // Регистрирует команду-заглушку и помечает её как неактивную (отображается серым)
    auto registerStub = [this, &stub](const std::string& name, const std::string& feature_name) {
        registerCommand(name, CommandFactory::createFunctionalCommand(
            name, stub(feature_name), "Stub", "", nullptr));
        if (command_manager_) {
            command_manager_->markCommandAsStub(name);
        }
    };

    // Settings stubs
    registerStub("open_auth_settings", "Auth Settings");
    registerStub("open_ssl_settings", "SSL Settings");
    registerStub("open_extensions", "Extensions");
    registerStub("open_plugins", "Plugins");

    // View stubs
    registerStub("reload_ui", "Reload UI");
    registerStub("toggle_performance", "Performance Overlay");
    registerStub("toggle_smart_redraw", "Smart Redraw");
    registerStub("toggle_vsync", "VSync");
    registerStub("toggle_fps_limit", "FPS Limit");

    // Help stubs
    registerStub("check_updates", "Check for Updates");

    // Agents stubs
    registerStub("agents_status", "Agent Status");
    registerStub("agents_list", "List Agents");
    registerStub("rag", "RAG Search");
    registerStub("search", "Web Search");
    registerStub("code", "Generate Code");
    registerStub("summarize", "Summarize");

    // Developer stubs
    registerStub("toggle_debug_mode", "Debug Mode");
    registerStub("show_command_manager_state", "Command Manager State");
    registerStub("show_window_manager_state", "Window Manager State");
    registerStub("export_debug_info", "Export Debug Info");
    registerStub("clear_cache", "Clear Cache");
    registerStub("show_logger_info", "Logger Info");
    registerStub("validate_files", "Validate Files");

    // Logging stubs
    registerStub("flush_logs", "Flush Logs");
    registerStub("export_logs", "Export Logs");
    registerStub("view_logs", "View Logs");
    registerStub("toggle_log_level", "Toggle Log Level");
    registerStub("toggle_log_to_file", "Toggle Log to File");

    // Misc stubs
    registerStub("toggle_flash_style_colors", "Flash Style Colors");
    registerStub("toggle_verify_ssl", "Verify SSL");
    registerStub("toggle_group_rects", "Show Group Rects");

    std::cout << "✓ Connected stub commands" << std::endl;
}

} // namespace ui
} // namespace llama_gui

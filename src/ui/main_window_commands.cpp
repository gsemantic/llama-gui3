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
            [this]() { if (backup_manager_dialog_) backup_manager_dialog_->setOpen(true); },
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

    commands_count = 17;

    std::cout << "✓ Connected settings menu commands (" << commands_count << " commands)" << std::endl;
}

void MainWindow::connectStubCommands() {
    std::cout << "Connecting stub commands (show 'not implemented' message)..." << std::endl;

    auto stub = [this](const std::string& feature_name) {
        return [this, feature_name]() {
            dialog_manager_.showInfo("Заглушка",
                "Функция '" + feature_name + "' ещё не реализована.");
        };
    };

    // Settings stubs
    registerCommand("open_settings_tensor_override", CommandFactory::createFunctionalCommand(
        "open_settings_tensor_override", stub("Tensor Override Settings"), "Stub", "", nullptr));
    registerCommand("open_settings_ini_viewer", CommandFactory::createFunctionalCommand(
        "open_settings_ini_viewer", stub("INI File Viewer"), "Stub", "", nullptr));
    registerCommand("open_settings_logging", CommandFactory::createFunctionalCommand(
        "open_settings_logging", stub("Logging Settings"), "Stub", "", nullptr));
    registerCommand("open_settings_performance", CommandFactory::createFunctionalCommand(
        "open_settings_performance", stub("Performance Settings"), "Stub", "", nullptr));
    registerCommand("open_settings_advanced", CommandFactory::createFunctionalCommand(
        "open_settings_advanced", stub("Advanced Settings"), "Stub", "", nullptr));
    registerCommand("open_settings_output", CommandFactory::createFunctionalCommand(
        "open_settings_output", stub("Output Settings"), "Stub", "", nullptr));
    registerCommand("open_auth_settings", CommandFactory::createFunctionalCommand(
        "open_auth_settings", stub("Auth Settings"), "Stub", "", nullptr));
    registerCommand("open_ssl_settings", CommandFactory::createFunctionalCommand(
        "open_ssl_settings", stub("SSL Settings"), "Stub", "", nullptr));
    registerCommand("open_extensions", CommandFactory::createFunctionalCommand(
        "open_extensions", stub("Extensions"), "Stub", "", nullptr));
    registerCommand("open_plugins", CommandFactory::createFunctionalCommand(
        "open_plugins", stub("Plugins"), "Stub", "", nullptr));
    registerCommand("open_llama_bench", CommandFactory::createFunctionalCommand(
        "open_llama_bench", stub("LLaMA Benchmark"), "Stub", "", nullptr));

    // Settings viewer (real, not stub)
    registerCommand("toggle_window_settings_viewer", CommandFactory::createFunctionalCommand(
        "toggle_window_settings_viewer",
        [this]() { settings_viewer_dialog_->show(); },
        "Toggle Settings Viewer", "Ctrl+Alt+I", nullptr));

    // View stubs
    registerCommand("show_grid_snapping_dialog", CommandFactory::createFunctionalCommand(
        "show_grid_snapping_dialog",
        [this]() {
            show_grid_snapping_ = !show_grid_snapping_;
            window_manager_.setWindowVisible("grid_snapping", show_grid_snapping_);
            if (show_grid_snapping_) {
                grid_snapping_dialog_->show();
            }
        },
        "Grid Snapping", "", nullptr));
    registerCommand("reload_ui", CommandFactory::createFunctionalCommand(
        "reload_ui", stub("Reload UI"), "Stub", "", nullptr));
    registerCommand("toggle_performance", CommandFactory::createFunctionalCommand(
        "toggle_performance", stub("Performance Overlay"), "Stub", "", nullptr));
    registerCommand("toggle_smart_redraw", CommandFactory::createFunctionalCommand(
        "toggle_smart_redraw", stub("Smart Redraw"), "Stub", "", nullptr));
    registerCommand("toggle_vsync", CommandFactory::createFunctionalCommand(
        "toggle_vsync", stub("VSync"), "Stub", "", nullptr));
    registerCommand("toggle_fps_limit", CommandFactory::createFunctionalCommand(
        "toggle_fps_limit", stub("FPS Limit"), "Stub", "", nullptr));

    // Help stubs
    registerCommand("check_updates", CommandFactory::createFunctionalCommand(
        "check_updates", stub("Check for Updates"), "Stub", "", nullptr));

    // Agents stubs
    registerCommand("agents_status", CommandFactory::createFunctionalCommand(
        "agents_status", stub("Agent Status"), "Stub", "", nullptr));
    registerCommand("agents_list", CommandFactory::createFunctionalCommand(
        "agents_list", stub("List Agents"), "Stub", "", nullptr));
    registerCommand("rag", CommandFactory::createFunctionalCommand(
        "rag", stub("RAG Search"), "Stub", "", nullptr));
    registerCommand("search", CommandFactory::createFunctionalCommand(
        "search", stub("Web Search"), "Stub", "", nullptr));
    registerCommand("code", CommandFactory::createFunctionalCommand(
        "code", stub("Generate Code"), "Stub", "", nullptr));
    registerCommand("summarize", CommandFactory::createFunctionalCommand(
        "summarize", stub("Summarize"), "Stub", "", nullptr));

    // Developer stubs
    registerCommand("show_style_editor", CommandFactory::createFunctionalCommand(
        "show_style_editor", stub("Style Editor"), "Stub", "", nullptr));
    registerCommand("show_font_selector", CommandFactory::createFunctionalCommand(
        "show_font_selector", stub("Font Selector"), "Stub", "", nullptr));
    registerCommand("show_debug_log", CommandFactory::createFunctionalCommand(
        "show_debug_log", stub("Debug Log"), "Stub", "", nullptr));
    registerCommand("start_item_picker", CommandFactory::createFunctionalCommand(
        "start_item_picker", stub("Item Picker"), "Stub", "", nullptr));
    registerCommand("toggle_debug_mode", CommandFactory::createFunctionalCommand(
        "toggle_debug_mode", stub("Debug Mode"), "Stub", "", nullptr));
    registerCommand("show_command_manager_state", CommandFactory::createFunctionalCommand(
        "show_command_manager_state", stub("Command Manager State"), "Stub", "", nullptr));
    registerCommand("show_window_manager_state", CommandFactory::createFunctionalCommand(
        "show_window_manager_state", stub("Window Manager State"), "Stub", "", nullptr));
    registerCommand("export_debug_info", CommandFactory::createFunctionalCommand(
        "export_debug_info", stub("Export Debug Info"), "Stub", "", nullptr));
    registerCommand("clear_cache", CommandFactory::createFunctionalCommand(
        "clear_cache", stub("Clear Cache"), "Stub", "", nullptr));
    registerCommand("show_logger_info", CommandFactory::createFunctionalCommand(
        "show_logger_info", stub("Logger Info"), "Stub", "", nullptr));
    registerCommand("validate_files", CommandFactory::createFunctionalCommand(
        "validate_files", stub("Validate Files"), "Stub", "", nullptr));

    // Logging stubs
    registerCommand("flush_logs", CommandFactory::createFunctionalCommand(
        "flush_logs", stub("Flush Logs"), "Stub", "", nullptr));
    registerCommand("export_logs", CommandFactory::createFunctionalCommand(
        "export_logs", stub("Export Logs"), "Stub", "", nullptr));
    registerCommand("view_logs", CommandFactory::createFunctionalCommand(
        "view_logs", stub("View Logs"), "Stub", "", nullptr));
    registerCommand("toggle_log_level", CommandFactory::createFunctionalCommand(
        "toggle_log_level", stub("Toggle Log Level"), "Stub", "", nullptr));
    registerCommand("toggle_log_to_file", CommandFactory::createFunctionalCommand(
        "toggle_log_to_file", stub("Toggle Log to File"), "Stub", "", nullptr));
    registerCommand("toggle_console", CommandFactory::createFunctionalCommand(
        "toggle_console", stub("Toggle Console"), "Stub", "", nullptr));

    // Misc stubs
    registerCommand("toggle_flash_style_colors", CommandFactory::createFunctionalCommand(
        "toggle_flash_style_colors", stub("Flash Style Colors"), "Stub", "", nullptr));
    registerCommand("toggle_verify_ssl", CommandFactory::createFunctionalCommand(
        "toggle_verify_ssl", stub("Verify SSL"), "Stub", "", nullptr));
    registerCommand("toggle_group_rects", CommandFactory::createFunctionalCommand(
        "toggle_group_rects", stub("Show Group Rects"), "Stub", "", nullptr));

    std::cout << "✓ Connected stub commands" << std::endl;
}

} // namespace ui
} // namespace llama_gui

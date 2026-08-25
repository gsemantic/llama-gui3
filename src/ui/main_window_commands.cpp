#include "main_window.h"
#include "commands_workspace.h"
#include "commands_settings.h"
#include "../../include/core/logger.h"
#include <iostream>
#include <fstream>

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

void MainWindow::connectSecondaryCommands() {
    std::cout << "Connecting secondary commands..." << std::endl;

    // Сохранение текущего профиля (персистентность изменений из меню)
    auto save_active_profile = [this]() {
        std::string profile = settings_.get_current_profile_name();
        settings_.save_profile(profile.empty() ? "default" : profile);
    };

    auto logger_level_name = [](llama_gui::core::Logger::Level level) -> std::string {
        switch (level) {
            case llama_gui::core::Logger::Level::None:    return "None";
            case llama_gui::core::Logger::Level::Error:   return "Error";
            case llama_gui::core::Logger::Level::Warning: return "Warning";
            case llama_gui::core::Logger::Level::Info:    return "Info";
            case llama_gui::core::Logger::Level::Debug:   return "Debug";
        }
        return "?";
    };

    // =========================================================================
    // Security
    // =========================================================================

    // SSL/TLS и токен доступа редактируются на реальной вкладке Security
    registerCommand("open_security_settings", CommandFactory::createFunctionalCommand(
        "open_security_settings",
        [this]() { settings_dialog_->show_security_settings(); },
        "Open security settings (SSL/TLS, access token)", "", nullptr));

    registerCommand("toggle_verify_ssl", CommandFactory::createFunctionalCommand(
        "toggle_verify_ssl",
        [this, save_active_profile]() {
            bool& verify = settings_.server().verify_ssl;
            verify = !verify;
            save_active_profile();
            // Обратная связь — галочка в меню (check_func), лог для консоли
            std::cout << "Verify SSL: " << (verify ? "ON" : "OFF")
                      << " (применяется к новым https-подключениям)" << std::endl;
        },
        "Toggle SSL certificate verification", "", nullptr));

    // =========================================================================
    // Performance
    // =========================================================================

    // Оверлей производительности = реальное окно ImGui Metrics
    registerCommand("toggle_performance", CommandFactory::createFunctionalCommand(
        "toggle_performance",
        [this]() { show_metrics_window_ = !show_metrics_window_; },
        "Toggle performance overlay window", "", nullptr));

    registerCommand("toggle_vsync", CommandFactory::createFunctionalCommand(
        "toggle_vsync",
        [this, save_active_profile]() {
            bool& vsync = settings_.performance().enable_vsync;
            vsync = !vsync;
            applied_swap_interval_ = vsync ? 1 : 0;
            if (sdl_window_ && gl_context_) {
                SDL_GL_SetSwapInterval(applied_swap_interval_);
            }
            save_active_profile();
            dialog_manager_.showInfo("V-Sync",
                std::string("Вертикальная синхронизация: ") + (vsync ? "ВКЛ" : "ВЫКЛ"));
        },
        "Toggle V-Sync", "", nullptr));

    registerCommand("toggle_fps_limit", CommandFactory::createFunctionalCommand(
        "toggle_fps_limit",
        [this, save_active_profile]() {
            static const int kLimits[] = {60, 120, 240, 30};
            int& fps = settings_.performance().target_fps;
            size_t idx = 0;
            for (size_t i = 0; i < 4; ++i) {
                if (fps == kLimits[i]) { idx = (i + 1) % 4; break; }
            }
            fps = kLimits[idx];
            save_active_profile();
            dialog_manager_.showInfo("Ограничение FPS",
                "Целевой FPS: " + std::to_string(fps));
        },
        "Cycle FPS limit (60 → 120 → 240 → 30)", "", nullptr));

    registerCommand("toggle_smart_redraw", CommandFactory::createFunctionalCommand(
        "toggle_smart_redraw",
        [this, save_active_profile]() {
            bool& smart = settings_.performance().enable_smart_redraw;
            smart = !smart;
            save_active_profile();
            dialog_manager_.showInfo("Умная перерисовка",
                std::string("Режим: ") + (smart ? "ВКЛ" : "ВЫКЛ") +
                "\nИзменение применится после перезапуска приложения.");
        },
        "Toggle smart redraw mode", "", nullptr));

    // =========================================================================
    // Logging / Debug
    // =========================================================================

    // Просмотр логов = встроенное окно Dear ImGui Debug Log
    registerCommand("view_logs", CommandFactory::createFunctionalCommand(
        "view_logs",
        [this]() { show_debug_log_window_ = true; },
        "Show application log window", "", nullptr));

    registerCommand("toggle_log_level", CommandFactory::createFunctionalCommand(
        "toggle_log_level",
        [logger_level_name]() {
            auto& logger = llama_gui::core::Logger::instance();
            using L = llama_gui::core::Logger::Level;
            L level = logger.get_level();
            int next = static_cast<int>(level);
            do {
                next = next >= static_cast<int>(L::Debug)
                           ? static_cast<int>(L::Error) : next + 1;
            } while (static_cast<L>(next) == L::None);  // None не даём: глушит всё
            logger.set_level(static_cast<L>(next));
            std::cout << "Log level → " << logger_level_name(logger.get_level()) << std::endl;
        },
        "Cycle log level (Error → Warning → Info → Debug)", "", nullptr));

    registerCommand("show_logger_info", CommandFactory::createFunctionalCommand(
        "show_logger_info",
        [this, logger_level_name]() {
            auto& logger = llama_gui::core::Logger::instance();
            const auto& perf = settings_.performance();
            dialog_manager_.showInfo("Logger",
                std::string("Текущий уровень: ") + logger_level_name(logger.get_level()) +
                "\nОтладочный режим: " + (logger.is_debug_mode() ? "да" : "нет") +
                "\nЛог в файл настройках: " + (perf.log_to_file ? "вкл (не реализовано)" : "выкл") +
                "\nФайл лога: " + (perf.log_file_path.empty() ? "<не задан>" : perf.log_file_path) +
                "\n\nЛоггер выводит сообщения в консоль (stdout/stderr).");
        },
        "Show Logger information", "", nullptr));

    registerCommand("toggle_debug_mode", CommandFactory::createFunctionalCommand(
        "toggle_debug_mode",
        [this, save_active_profile]() {
            auto& logger = llama_gui::core::Logger::instance();
            bool enable = !logger.is_debug_mode();
            logger.set_debug_mode(enable);
            settings_.performance().debug_mode = enable;  // персистентность
            save_active_profile();
            std::cout << "Debug mode " << (enable ? "ENABLED" : "disabled") << std::endl;
        },
        "Toggle verbose debug logging", "", nullptr));

    // =========================================================================
    // Application state
    // =========================================================================

    registerCommand("show_command_manager_state", CommandFactory::createFunctionalCommand(
        "show_command_manager_state",
        [this]() {
            const auto stats = command_manager_->getStatistics();
            const auto names = command_manager_->getAllCommandNames();
            std::string text = "Зарегистрировано команд: " + std::to_string(stats.total_commands) +
                "\nГорячих клавиш: " + std::to_string(stats.total_shortcuts) +
                "\nИстория (undo): " + std::to_string(stats.history_size);
            text += "\n\nКоманды:";
            for (const auto& n : names) {
                text += "\n  • " + n;
                if (command_manager_->isCommandStub(n)) text += "  [заглушка]";
            }
            dialog_manager_.showInfo("Состояние CommandManager", text);
        },
        "Show Command Manager state and statistics", "", nullptr));

    registerCommand("show_window_manager_state", CommandFactory::createFunctionalCommand(
        "show_window_manager_state",
        [this]() {
            const auto states = window_manager_.getAllWindowStates();
            std::string text = "Зарегистрировано окон: " + std::to_string(states.size()) + "\n";
            for (const auto& w : states) {
                text += "\n  • " + w.name +
                    "  pos=(" + std::to_string((int)w.position.x) + "," + std::to_string((int)w.position.y) + ")" +
                    "  size=" + std::to_string((int)w.size.x) + "x" + std::to_string((int)w.size.y) +
                    (w.visible ? "  [видимо]" : "  [скрыто]");
            }
            dialog_manager_.showInfo("Состояние WindowManager", text);
        },
        "Show Window Manager state and window positions", "", nullptr));

    registerCommand("export_debug_info", CommandFactory::createFunctionalCommand(
        "export_debug_info",
        [this]() {
            file_dialog_manager_->pick_save("Export Debug Info", "debug_info.txt",
                [this](const std::string& path) {
                    if (path.empty()) return;
                    std::ofstream file(path);
                    if (!file.is_open()) {
                        dialog_manager_.showError("Экспорт отладочной информации",
                            "Не удалось открыть файл:\n" + path);
                        return;
                    }
                    const auto stats = command_manager_->getStatistics();
                    file << "=== Llama GUI debug info ===\n";
                    file << "Команд зарегистрировано: " << stats.total_commands << "\n";
                    file << "Горячих клавиш: " << stats.total_shortcuts << "\n";
                    file << "Окон в WindowManager: " << window_manager_.getAllWindowNames().size() << "\n";
                    for (const auto& w : window_manager_.getAllWindowStates()) {
                        file << "  " << w.name << " pos=(" << w.position.x << "," << w.position.y
                             << ") size=" << w.size.x << "x" << w.size.y
                             << (w.visible ? " visible" : " hidden") << "\n";
                    }
                    file.close();
                    dialog_manager_.showInfo("Экспорт отладочной информации",
                        "Сохранено в:\n" + path);
                });
        },
        "Export debug information to file", "", nullptr));

    // =========================================================================
    // Tools
    // =========================================================================

    registerCommand("open_plugins", CommandFactory::createFunctionalCommand(
        "open_plugins",
        [this]() {
            const auto plugins = plugin_manager_ ? plugin_manager_->list_plugins()
                                                 : std::vector<plugin::PluginInfo>();
            std::string text = "Загружено плагинов: " + std::to_string(plugins.size()) + "\n";
            for (const auto& p : plugins) {
                text += "\n  • " + p.name + " v" + p.version +
                        "\n    " + (p.description.empty() ? "<без описания>" : p.description);
            }
            dialog_manager_.showInfo("Плагины", text);
        },
        "Show loaded plugins", "", nullptr));

    registerCommand("reload_ui", CommandFactory::createFunctionalCommand(
        "reload_ui",
        [this]() {
            advanced_menu_system_.rebuildModernMenu();
            applyMenuToggleBindings();
            force_ui_update_ = true;
            std::cout << "UI reloaded by user request" << std::endl;
        },
        "Rebuild menus and refresh UI state", "", nullptr));

    std::cout << "✓ Connected secondary commands" << std::endl;
}

} // namespace ui
} // namespace llama_gui

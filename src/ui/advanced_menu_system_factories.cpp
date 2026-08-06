#include "../include/ui/advanced_menu_system.h"
#include "../include/ui/localization_manager.h"
#include <algorithm>
#include <iostream>

namespace llama_gui {
namespace ui {

// =========================================================================
// Фабричные методы для создания меню
// =========================================================================

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createFileMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "File";
    menu->name = TRF("menu.file", "File");
    std::cout << "  createFileMenu: name = " << menu->name << std::endl;

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.file.open", "Open"),
        "open_file", "Ctrl+O", "Open a file"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.file.save", "Save"),
        "save_file", "Ctrl+S", "Save current conversation"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.file.save_as", "Save As"),
        "save_file_as", "", "Save conversation as..."));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.file.exit", "Exit"),
        "exit_app", "Alt+F4", "Exit the application"));

    return menu;
}

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createSettingsMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "Settings";
    menu->name = TRF("menu.settings", "Settings");

    // Быстрые настройки (Quick Settings)
    AdvancedMenuItem quick_settings_submenu;
    quick_settings_submenu.name = TRF("menu.settings.quick", "Quick Settings");
    quick_settings_submenu.type = AdvancedMenuItemType::Submenu;
    quick_settings_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.quick.server", "Server"),
        "open_settings_server", "", "Open server settings"));
    quick_settings_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.quick.chat", "Chat"),
        "open_settings_chat", "", "Open chat settings"));
    quick_settings_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.quick.models", "Models"),
        "open_settings_models", "", "Open model settings"));
    quick_settings_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.quick.ui", "UI"),
        "open_settings_ui", "", "Open UI settings"));
    quick_settings_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.quick.cloud", "Cloud"),
        "open_settings_cloud", "", "Open cloud services settings (OpenRouter, etc.)"));
    menu->items.push_back(quick_settings_submenu);

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    // Настройки GPU и оборудования
    AdvancedMenuItem gpu_hardware_submenu;
    gpu_hardware_submenu.name = TRF("menu.settings.gpu_hardware", "GPU & Hardware");
    gpu_hardware_submenu.type = AdvancedMenuItemType::Submenu;
    gpu_hardware_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.gpu", "GPU"),
        "open_settings_gpu", "", "Open GPU settings"));
    gpu_hardware_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.cache", "Cache"),
        "open_settings_cache", "", "Open cache settings"));
    menu->items.push_back(gpu_hardware_submenu);

    // Настройки сэмплирования и генерации
    AdvancedMenuItem sampling_generation_submenu;
    sampling_generation_submenu.name = TRF("menu.settings.sampling_generation", "Sampling & Generation");
    sampling_generation_submenu.type = AdvancedMenuItemType::Submenu;
    sampling_generation_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.sampling_basic", "Sampling Basic"),
        "open_settings_sampling_basic", "", "Open basic sampling settings"));
    sampling_generation_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.sampling_advanced", "Sampling Advanced"),
        "open_settings_sampling_advanced", "", "Open advanced sampling settings"));
    sampling_generation_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.context", "Context"),
        "open_settings_context", "", "Open context settings"));
    sampling_generation_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.rope", "RoPE"),
        "open_settings_rope", "", "Open RoPE settings"));
    menu->items.push_back(sampling_generation_submenu);

    // Настройки модели и сервера
    AdvancedMenuItem model_server_submenu;
    model_server_submenu.name = TRF("menu.settings.model_server", "Model & Server");
    model_server_submenu.type = AdvancedMenuItemType::Submenu;
    model_server_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.model_loading", "Model Loading"),
        "open_settings_model_loading", "", "Open model loading settings"));
    model_server_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.batch", "Batch"),
        "open_settings_batch", "", "Open batch settings"));
    model_server_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.server_runtime", "Server Runtime"),
        "open_settings_server_runtime", "", "Open server runtime settings"));
    model_server_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.grammar", "Grammar"),
        "open_settings_grammar", "", "Open grammar settings"));
    model_server_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.control_vectors", "Control Vectors"),
        "open_settings_control_vectors", "", "Open control vectors settings"));
    menu->items.push_back(model_server_submenu);

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    // Управление сервером (перенесено из меню "Модель")
    AdvancedMenuItem server_control_submenu;
    server_control_submenu.name = TRF("menu.settings.server_control", "Server Control");
    server_control_submenu.type = AdvancedMenuItemType::Submenu;
    server_control_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.model.server.start", "Start Server"),
        "server_start", "", "Start the model server"));
    server_control_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.model.server.stop", "Stop Server"),
        "server_stop", "", "Stop the model server"));
    server_control_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.model.server.restart", "Restart Server"),
        "server_restart", "", "Restart the model server"));
    server_control_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.model.server.status", "Server Status"),
        "server_status", "", "Show server status"));
    menu->items.push_back(server_control_submenu);

    // Системные настройки
    AdvancedMenuItem system_submenu;
    system_submenu.name = TRF("menu.settings.system", "System");
    system_submenu.type = AdvancedMenuItemType::Submenu;
    system_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.logging", "Logging"),
        "open_settings_logging", "", "Open logging settings"));
    system_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.performance", "Performance"),
        "open_settings_performance", "", "Open performance settings"));
    system_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.advanced", "Advanced"),
        "open_settings_advanced", "", "Open advanced settings"));
    system_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.output", "Output"),
        "open_settings_output", "", "Open output settings"));
    system_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.tensor_override", "Tensor Override"),
        "open_settings_tensor_override", "", "Open tensor override settings"));
    system_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.ini_viewer", "INI File Viewer"),
        "open_settings_ini_viewer", "", "View and edit all settings in INI format (like php.ini)"));
    menu->items.push_back(system_submenu);

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    // Управление профилями
    AdvancedMenuItem profiles_submenu;
    profiles_submenu.name = TRF("menu.settings.profiles", "Profiles");
    profiles_submenu.type = AdvancedMenuItemType::Submenu;
    profiles_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.profiles.manage", "Manage Profiles"),
        "show_profile_manager", "Ctrl+Shift+P", "Open profile manager"));
    profiles_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.profiles.save_current", "Save Current Profile"),
        "save_current_profile", "Ctrl+Shift+S", "Save current profile"));
    menu->items.push_back(profiles_submenu);

    // Резервные копии
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.backups", "Backups"),
        "show_backup_manager", "Ctrl+Shift+B", "Open backup manager"));

    // Quick Settings
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.quick_settings", "Quick Settings"),
        "toggle_quick_settings", "Ctrl+Alt+Q", "Quick settings dialog"));

    // Settings Viewer
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.settings.ini_viewer", "INI File Viewer"),
        "toggle_window_settings_viewer", "Ctrl+Alt+I", "View and edit all settings in INI format"));

    return menu;
}

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createViewMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "View";
    menu->name = TRF("menu.view", "View");

    // Элементы управления окнами (перенесено из меню "Окно")
    menu->items.push_back(AdvancedMenuItemFactory::createWindowToggleItem(
        TRF("menu.window.conversations", "Conversations"),
        "conversations", "Ctrl+1", "Toggle conversations panel"));
    menu->items.push_back(AdvancedMenuItemFactory::createWindowToggleItem(
        TRF("menu.window.files", "Files"),
        "files", "Ctrl+2", "Toggle files panel"));
    menu->items.push_back(AdvancedMenuItemFactory::createWindowToggleItem(
        TRF("menu.window.chat", "Chat"),
        "chat", "Ctrl+3", "Toggle chat panel"));
    menu->items.push_back(AdvancedMenuItemFactory::createWindowToggleItem(
        TRF("menu.window.rag", "RAG"),
        "rag", "Ctrl+4", "Toggle RAG panel"));
    menu->items.push_back(AdvancedMenuItemFactory::createWindowToggleItem(
        TRF("menu.window.agents", "Agents"),
        "agents", "Ctrl+5", "Toggle agents panel"));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    // Подменю "Рабочая область" (перенесено из меню "Окно")
    AdvancedMenuItem workspace_submenu;
    workspace_submenu.name = TRF("menu.window.workspace", "Workspace");
    workspace_submenu.type = AdvancedMenuItemType::Submenu;
    workspace_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.window.workspace.save", "Сохранить"),
        "save_workspace", "", "Сохранить текущую конфигурацию окон (default)"));
    workspace_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.window.workspace.save_as", "Сохранить как..."),
        "save_workspace_as", "", "Сохранить с произвольным именем"));
    workspace_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createSeparator());
    workspace_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.window.workspace.load_default", "Загрузить по умолчанию"),
        "load_workspace", "", "Загрузить конфигурацию default"));
    workspace_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.window.workspace.load_as", "Загрузить другую..."),
        "load_workspace_as", "", "Выбрать конфигурацию из списка"));
    workspace_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createSeparator());
    workspace_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.window.workspace.reset", "Сбросить"),
        "reset_workspace", "", "Сбросить к настройкам по умолчанию"));
    menu->items.push_back(workspace_submenu);

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    // Дополнительные элементы вида
    menu->items.push_back(AdvancedMenuItemFactory::createWindowToggleItem(
        TRF("menu.view.status_bar", "Status Bar"),
        "status_bar", "", "Toggle status bar"));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    // Fullscreen toggle
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.view.fullscreen", "Fullscreen"),
        "toggle_fullscreen", "F11", "Toggle fullscreen mode"));
    
    // Grid Snapping (примагничивание по сетке)
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.view.grid_snapping_settings", "Grid Snapping Settings..."),
        "show_grid_snapping_dialog", "", "Configure window grid snapping"));

    return menu;
}

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createWindowMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "Window";
    menu->name = TRF("menu.window", "Window");

    // Список окон заполняется динамически из WindowManager,
    // чтобы не зависеть от порядка инициализации меню/окон.
    populateWindowMenu(*menu);

    return menu;
}

void AdvancedMenuSystem::populateWindowMenu(AdvancedMenu& menu) {
    menu.items.clear();
    if (!window_manager_) return;

    const auto windows = window_manager_->getAllWindowStates();

    // Сортируем по имени для стабильного порядка (WindowManager хранит окна в unordered_map)
    std::vector<WindowState> sorted = windows;
    std::sort(sorted.begin(), sorted.end(), [this](const WindowState& a, const WindowState& b) {
        std::string na = window_manager_->getImGuiName(a.name);
        std::string nb = window_manager_->getImGuiName(b.name);
        if (na.empty()) na = a.name;
        if (nb.empty()) nb = b.name;
        return na < nb;
    });

    for (const auto& ws : sorted) {
        // Пропускаем служебные окна (начинаются с "_")
        if (ws.name.empty() || ws.name[0] == '_') continue;

        std::string display = window_manager_->getImGuiName(ws.name);
        if (display.empty()) display = ws.name;

        menu.items.push_back(AdvancedMenuItemFactory::createWindowToggleItem(
            display, ws.name, "",
            "Show/hide window '" + display + "'"));
    }
}

void AdvancedMenuSystem::refreshWindowMenu() {
    // Окна могут регистрироваться после построения меню — обновляем список каждый кадр.
    // Ищем по menu_key (стабильному), а не по локализованному имени: menus_map_
    // хранит ключ "Окно"/"Window", поэтому поиск по строке "Window" дал бы nullptr.
    for (auto& menu : menus_ordered_) {
        if (menu && menu->menu_key == "Window") {
            populateWindowMenu(*menu);
            return;
        }
    }
}

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createHelpMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "Help";
    menu->name = TRF("menu.help", "Help");

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.help.documentation", "Documentation"),
        "show_documentation", "", "Open documentation"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.help.keyboard_shortcuts", "Keyboard Shortcuts"),
        "show_shortcuts", "", "Show keyboard shortcuts"));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    // Подменю "About"
    AdvancedMenuItem about_submenu;
    about_submenu.name = TRF("menu.help.about", "About");
    about_submenu.type = AdvancedMenuItemType::Submenu;
    about_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.help.about.app", "About Application"),
        "show_about", "", "Show information about the application"));
    about_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.help.about.devs", "About Developers"),
        "show_about_devs", "", "Show information about developers"));
    about_submenu.submenu_items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.help.check_updates", "Check for Updates"),
        "check_updates", "", "Check for application updates"));
    menu->items.push_back(about_submenu);

    return menu;
}

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createAgentsMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "Agents";
    menu->name = TRF("menu.agents", "Agents");

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.agents.panel", "Agents Panel"),
        "toggle_window_agents", "", "Show/hide Agents Panel"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.agents.status", "Agent Status"),
        "agents_status", "", "Show agent system status"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.agents.list", "List Agents"),
        "agents_list", "", "List all available agents"));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.agents.rag_search", "RAG Search"),
        "rag", "", "Search documents using RAG"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.agents.web_search", "Web Search"),
        "search", "", "Search the web"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.agents.code", "Generate Code"),
        "code", "", "Generate code with AI"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.agents.summarize", "Summarize"),
        "summarize", "", "Summarize text"));

    return menu;
}

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createDeveloperMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "Developer";
    menu->name = TRF("menu.developer", "Developer");

    // ImGui Tools submenu items
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.tools.metrics", "Metrics/Debugger"),
        "show_metrics", "", "Show Dear ImGui Metrics/Debugger window"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.tools.style_editor", "Style Editor"),
        "show_style_editor", "", "Show Dear ImGui Style Editor"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.tools.font_selector", "Font Selector"),
        "show_font_selector", "", "Show Dear ImGui Font Selector"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.tools.debug_log", "Debug Log"),
        "show_debug_log", "", "Show Dear ImGui Debug Log"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.tools.item_picker", "Item Picker"),
        "start_item_picker", "", "Start Dear ImGui Item Picker"));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    // Debug Options submenu items
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.debug.debug_mode", "Debug Mode"),
        "toggle_debug_mode", "", "Enable/disable debug mode"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.debug.show_group_rects", "Show Group Rects"),
        "toggle_group_rects", "", "Show layout group rects for debugging"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.debug.flash_style_colors", "Flash Style Colors"),
        "toggle_flash_style_colors", "", "Flash style colors for visual debugging"));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    // Application State submenu items
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.state.command_manager", "Show Command Manager State"),
        "show_command_manager_state", "", "Show Command Manager state and statistics"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.state.window_manager", "Show Window Manager State"),
        "show_window_manager_state", "", "Show Window Manager state and window positions"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.state.logger", "Show Logger Info"),
        "show_logger_info", "", "Show Logger information and statistics"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.state.export_debug", "Export Debug Info"),
        "export_debug_info", "", "Export debug information to file"));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    // Utility commands
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.reload_ui", "Reload UI"),
        "reload_ui", "", "Reload the user interface"));
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.developer.clear_cache", "Clear Cache"),
        "clear_cache", "", "Clear application caches"));

    return menu;
}

// =========================================================================
// Новые меню для workspace
// =========================================================================

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createToolsMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "Tools";
    menu->name = TRF("menu.tools", "Tools");

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.tools.plugins", "Plugins"),
        "open_plugins", "", "Manage plugins"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.tools.extensions", "Extensions"),
        "open_extensions", "", "Manage extensions"));

#ifdef ENABLE_LLAMA_BENCH
    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.tools.llama_bench", "Llama Bench"),
        "open_llama_bench", "", "Compare model profiles with llama-bench"));
#endif

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.tools.console", "Console"),
        "toggle_console", "", "Toggle developer console"));

    return menu;
}

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createPerformanceMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "Performance";
    menu->name = TRF("menu.performance", "Performance");

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.performance.overlay", "Performance Overlay"),
        "toggle_performance", "", "Toggle performance overlay"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.performance.settings", "Performance Settings"),
        "open_performance_settings", "", "Open performance settings"));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.performance.vsync", "V-Sync"),
        "toggle_vsync", "", "Toggle V-Sync"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.performance.fps_limit", "FPS Limit"),
        "toggle_fps_limit", "", "Toggle FPS limit"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.performance.smart_redraw", "Smart Redraw"),
        "toggle_smart_redraw", "", "Toggle smart redraw mode"));

    return menu;
}

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createSecurityMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "Security";
    menu->name = TRF("menu.security", "Security");

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.security.ssl", "SSL/TLS Settings"),
        "open_ssl_settings", "", "Configure SSL/TLS"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.security.auth_token", "Auth Token"),
        "open_auth_settings", "", "Manage authentication token"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.security.verify_ssl", "Verify SSL"),
        "toggle_verify_ssl", "", "Toggle SSL verification"));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.security.file_validation", "File Validation"),
        "validate_files", "", "Validate uploaded files"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.security.audit_log", "Audit Log"),
        "show_audit_log", "", "Show security audit log"));

    return menu;
}

std::unique_ptr<AdvancedMenu> AdvancedMenuSystem::createLoggingMenu() {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = "Logging";
    menu->name = TRF("menu.logging", "Logging");

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.logging.settings", "Logging Settings"),
        "open_logging_settings", "", "Configure logging"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.logging.view_logs", "View Logs"),
        "view_logs", "", "View application logs"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.logging.log_level", "Log Level"),
        "toggle_log_level", "", "Change log level"));

    menu->items.push_back(AdvancedMenuItemFactory::createSeparator());

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.logging.log_to_file", "Log to File"),
        "toggle_log_to_file", "", "Toggle file logging"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.logging.flush_logs", "Flush Logs"),
        "flush_logs", "", "Flush log buffers"));

    menu->items.push_back(AdvancedMenuItemFactory::createCommandItem(
        TRF("menu.logging.export_logs", "Export Logs"),
        "export_logs", "", "Export logs to file"));

    return menu;
}

} // namespace ui
} // namespace llama_gui

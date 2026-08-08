#pragma once

#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include <queue>
#include <fstream>
#include <unistd.h>
#include <atomic>
#include "core/llama_interface.h"
#include "core/server_manager.h"
#include "core/state_manager.h"
#include "core/settings.h"
#include "core/model_manager.h"
#include "layout_controller.h"
#include "input_handler.h"

#ifdef USE_SDL2
#include <SDL.h>
#endif

#ifdef USE_OPENGL
#include <GL/glew.h>
#endif

#include "imgui.h"
#include "chat_interface.h"
#include "file_manager.h"
#include "conversation_manager.h"
#include "settings_dialog.h"
#include "cloud_services_dialog.h"
#include "dialog_manager.h"
#include "localization_manager.h"
#include "language_selector.h"
#include "window_coordinator.h"
#include "workspace_applier.h"
#include "workspace_layout_manager.h"
#include "core/rag_manager.h"
#include "core/rag_settings.h"
#include "ui/rag_interface.h"
#include "ui/rag_settings_dialog.h"
#include "ui/settings_viewer_dialog.h"
#include "ui/grid_snapping_dialog.h"
#include "ui/profile_manager_dialog.h"
#include "ui/backup_manager_dialog.h"
#ifdef ENABLE_LLAMA_BENCH
#include "ui/llama_bench_dialog.h"
#endif
// #include "agents/agents.h"  // ОТКЛЮЧЕНО: агенты временно отключены
#include "command_manager.h"
#include "advanced_menu_system.h"
#include "workspace_manager.h"
#include "ui/conversation_file_manager.h"
#include "ui/file_dialog_manager.h"
#include "ui/model_manager.h"
#include "ui/performance_monitor.h"
#include "plugins/plugin_manager.h"


namespace llama_gui {
namespace ui {

using llama_gui::core::Settings;
using llama_gui::core::StateManager;
using llama_gui::core::LlamaInterface;

class ChatInterface;
class FileManager;
class ConversationManager;
class SettingsDialog;
class SettingsViewerDialog;

// Forward declarations
class CommandManager;
class ConversationFileManager;
class FileDialogManager;
class PerformanceMonitor;

// Dialog result structure
struct PendingDialogResult {
    enum class DialogResultType {
        ModelLoad,
        FileSave,
        None
    };
    
    DialogResultType type = DialogResultType::None;
    bool confirmed = false;
    std::string model_path;
    std::string file_path;
};

/**
 * Main application window using Dear ImGui
 * Handles the overall layout and coordination of UI components
 */
class MainWindow {
public:
    MainWindow(StateManager& state_manager, Settings& settings, LlamaInterface& llama_interface);
    ~MainWindow();

    // Initialize the window and all subsystems
    bool initialize(int width = 1200, int height = 800);

    // Main application loop
    void run();

    // Shutdown
    void shutdown();

    // Main render method
    void render();

    // Plugin system host (загружает и вызывает плагины приложения)
    void initializePlugins();
    
    // New UI Management System methods
    void initializeNewUISystem();
    void setupCommandCallbacks();
    void syncWindowFlagsFromManager();
    
    // Открыть диалоговое окно по имени (для Window menu / toggle-команд)
    void showWindowByName(const std::string& window_name);
    // Скрыть диалоговое окно по имени
    void hideWindowByName(const std::string& window_name);
    
    // Window management
    void set_title(const std::string& title);
    void set_size(int width, int height);
    void set_position(int x, int y);
    
    // State callbacks
    void on_conversation_changed(const llama_gui::core::StateEvent& event);
    void on_server_state_changed();
    void on_settings_changed();
    void on_state_changed(const std::string& key);
    
    // File operations
    void open_file(const std::string& file_path);
    void save_conversation(const std::string& conversation_id, const std::string& file_path);
    void export_conversations(const std::string& file_path);

    // Conversation file operations
    void open_conversation_file();
    void save_current_conversation();
    void save_current_conversation_as(const std::string& file_path);

    // Model directory selection
    void open_model_directory_dialog();

    // Model loading
    void load_model_from_path(const std::string& model_path);

    // Workspace management
    void save_workspace(const std::string& name);
    void load_workspace(const std::string& name);
    void reset_workspace();
    void show_workspace_save_dialog();
    void show_workspace_load_dialog();
    void render_workspace_save_dialog();
    void render_workspace_load_dialog();
    void delete_workspace(const std::string& name);
    std::vector<std::string> get_workspace_list() const;
    
    // Utility methods
    void show_about_dialog();
    void show_help_dialog();
    void show_help(const std::string& help_type);
    void open_settings();
    void on_server_control_command(ServerControlCommand::Action action);
    void show_keyboard_shortcuts();
    void toggle_fullscreen();
    
    // Error handling
    void show_error(const std::string& title, const std::string& message);
    void show_warning(const std::string& title, const std::string& message);
    void show_info(const std::string& title, const std::string& message);
    
    // Settings access
    Settings& get_settings() { return settings_; }
    const Settings& get_settings() const { return settings_; }
    
    // ConfigManager access
    llama_gui::core::ConfigManager& get_config_manager() { return config_manager_; }
    const llama_gui::core::ConfigManager& get_config_manager() const { return config_manager_; }

    // Profile management
    void show_profile_manager();
    void save_current_profile();
    void load_profile(const std::string& profile_name);

    // Backup management

    // Settings viewer
    void show_settings_viewer();

    // Command registration
    void registerCommand(const std::string& name, std::unique_ptr<Command> command);
    void connectWorkspaceCommands();
    void connectAdditionalWindowCommands();
    void connectSettingsMenuCommands();
    void connectDeveloperCommands();
    void connectStubCommands();
    void create_backup();
    void restore_from_backup(const std::string& backup_path);

#ifdef ENABLE_LLAMA_BENCH
    // Llama Bench
    void openLlamaBenchDialog();
#endif

    // State manager access
    StateManager& get_state_manager() { return state_manager_; }
    const StateManager& get_state_manager() const { return state_manager_; }
    
    // Llama interface access
    LlamaInterface& get_llama_interface() { return llama_interface_; }
    const LlamaInterface& get_llama_interface() const { return llama_interface_; }

    // File dialog methods - public interface
    bool try_open_embedding_model_file_dialog(std::string& selected_path);
    bool try_zenity_embedding_model_file_dialog(std::string& selected_path);
    bool try_kdialog_embedding_model_file_dialog(std::string& selected_path);
    bool try_python_embedding_model_file_dialog(std::string& selected_path);

private:
    // Platform-specific initialization
    bool init_opengl();
    bool init_sdl2();
    void cleanup_opengl();
    void cleanup_sdl2();
    void load_fonts_with_cyrillic();
    void reload_fonts();  // Перезагрузка шрифтов при смене языка

    // Main render loop
    void render_ui();

    // UI setup
    void setup_imgui_style();
    void on_first_frame();
    
    // Menu bar and main layout
    void render_main_layout();
    void render_status_bar();
    
    // Window layout management
    void setup_window_layout();
    void save_window_layout();
    void load_window_layout();

    // Обновление локализованных ImGui-имён окон (смена языка) с сохранением позиций/размеров
    void refreshLocalizedWindowNames();
    
    // Theme and styling
    void apply_theme();
    void render_custom_styling();
    
    // Performance monitoring
    void update_performance_metrics();
    void render_performance_overlay();

    // Developer tools
    void renderDeveloperTools();
    void renderCommandManagerWindow();
    void renderWindowManagerWindow();
    void renderLoggerInfoWindow();
    void exportDebugInfo();

    // File dialog helper methods
    bool try_open_native_file_dialog(std::string& selected_path);
    bool try_zenity_file_dialog(std::string& selected_path);
    bool try_kdialog_file_dialog(std::string& selected_path);
    bool try_python_file_dialog(std::string& selected_path);

    // Event handling
    
    // Core components (references)
    Settings& settings_;
    StateManager& state_manager_;
    LlamaInterface& llama_interface_;
    std::shared_ptr<ServerManager> server_manager_;
    std::unique_ptr<llama_gui::core::RagManager> rag_manager_;

    // UI components
    std::unique_ptr<ChatInterface> chat_interface_;
    std::unique_ptr<FileManager> file_manager_;
    std::unique_ptr<ConversationManager> conversation_manager_;
    std::unique_ptr<SettingsDialog> settings_dialog_;
    std::unique_ptr<CloudServicesDialog> cloud_services_dialog_;
    std::unique_ptr<RagInterface> rag_interface_;
    std::unique_ptr<RagSettingsDialog> rag_settings_dialog_;
    std::unique_ptr<SettingsViewerDialog> settings_viewer_dialog_;
    std::unique_ptr<GridSnappingDialog> grid_snapping_dialog_;
    std::unique_ptr<ProfileManagerDialog> profile_manager_dialog_;
    std::unique_ptr<BackupManagerDialog> backup_manager_dialog_;
    std::unique_ptr<QuickSettingsDialog> quick_settings_dialog_;
#ifdef ENABLE_LLAMA_BENCH
    std::unique_ptr<LlamaBenchDialog> llama_bench_dialog_;
#endif

    // Agent System - ОТКЛЮЧЕНО: агенты временно отключены
    // agents::AgentRegistry agent_registry_;
    // agents::AgentContext agent_context_;
    // std::unique_ptr<AgentChatIntegration> agent_chat_integration_;

    // Configuration Manager
    llama_gui::core::ConfigManager config_manager_;

    // New UI Management System
    std::unique_ptr<CommandManager> command_manager_;
    AdvancedMenuSystem advanced_menu_system_;
    WindowManager window_manager_;
    WorkspaceManager workspace_manager_;
    DialogManager dialog_manager_;
    LayoutController layout_controller_;
    InputHandler input_handler_;
    WindowCoordinator window_coordinator_;
    WorkspaceApplier workspace_applier_;
    WorkspaceLayoutManager workspace_layout_manager_;

    // Stage 3 Managers
    std::unique_ptr<ConversationFileManager> conversation_file_manager_;
    std::unique_ptr<FileDialogManager> file_dialog_manager_;
    std::unique_ptr<ModelManager> model_manager_;
    std::unique_ptr<PerformanceMonitor> performance_monitor_;

    // Plugin system host
    std::unique_ptr<llama_gui::plugin::PluginManager> plugin_manager_;
    

    
    // Platform state
#ifdef USE_SDL2
    SDL_Window* sdl_window_ = nullptr;
    SDL_GLContext gl_context_ = nullptr;
#endif
    
    // OpenGL state
#ifdef USE_OPENGL
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
#endif
    
    // Window state
    std::string title_ = "Llama GUI";
    int width_ = 1200;
    int height_ = 800;
    bool is_initialized_ = false;
    bool is_running_ = false;

    // Fullscreen state (saved before entering fullscreen)
    int pre_fullscreen_x_ = 0;
    int pre_fullscreen_y_ = 0;
    int pre_fullscreen_w_ = 1200;
    int pre_fullscreen_h_ = 800;
    
    // UI state
    bool show_menu_bar_ = true;
    bool show_status_bar_ = true;
    bool show_performance_overlay_ = false;
    bool compact_mode_ = false;
    bool is_fullscreen_ = false;
    bool show_demo_window_ = false;
    bool show_metrics_window_ = false;
    bool show_style_editor_window_ = false;
    bool show_debug_log_window_ = false;
    bool show_font_selector_window_ = false;
    bool show_command_manager_window_ = false;
    bool show_window_manager_window_ = false;
    bool show_logger_info_window_ = false;
    bool developer_mode_enabled_ = false;
    bool show_group_rects_ = false;
    bool flash_style_colors_ = false;
    bool first_frame_ = true;
    
    // Language change pending flag
    bool pending_language_change_ = false;
    bool pending_reconnect_ = false;
    bool force_ui_update_ = false;
    bool force_apply_window_positions_ = false;  // Для принудительного применения позиций после примагничивания
    bool prev_mouse_left_ = false;  // Для отслеживания отпускания кнопки мыши (примагничивание)
    
    // Dialogs
    bool show_about_ = false;
    bool show_help_ = false;
    bool show_keyboard_shortcuts_ = false;
    bool show_open_file_dialog_ = false;
    bool show_save_file_dialog_ = false;
    bool show_profile_manager_ = false;
    bool show_open_conversation_dialog_ = false;
    bool show_save_conversation_dialog_ = false;
    bool show_workspace_save_dialog_ = false;
    bool show_workspace_load_dialog_ = false;
    bool show_workspace_overwrite_confirm_ = false;
    bool show_workspace_delete_confirm_ = false;
    char workspace_overwrite_name_[256] = "";
    bool show_chat_ = true;  // Флаг показа окна чата
    bool show_conversations_ = false;  // Флаг показа окна бесед
    bool show_files_ = false;  // Флаг показа окна файлов
    bool show_rag_ = false;  // Флаг показа окна RAG
    bool show_agents_ = false;  // Флаг показа окна агентов
    bool show_settings_ = false;  // Флаг показа настроек
    bool show_cloud_services_ = false;  // Флаг показа облачных сервисов
    bool show_rag_settings_ = false;  // Флаг показа настроек RAG
    bool show_backup_manager_ = false;  // Флаг показа менеджера резервных копий
    bool show_grid_snapping_ = false;  // Флаг показа настроек сетки
    bool show_settings_viewer_ = false;  // Флаг показа Settings Viewer
#ifdef ENABLE_LLAMA_BENCH
    bool show_llama_bench_ = false;
#endif
    std::string workspace_save_name_;
    std::string workspace_load_name_;
    
    // Error dialogs
    std::vector<std::pair<std::string, std::string>> error_queue_;
    std::vector<std::pair<std::string, std::string>> warning_queue_;
    std::vector<std::pair<std::string, std::string>> info_queue_;
    
    // Performance metrics
    float frame_time_ = 0.0f;
    float fps_ = 0.0f;
    int frame_count_ = 0;
    uint32_t last_frame_time_ = 0;

    // Performance optimization variables
    uint32_t last_performance_update_time_ = 0;
    uint32_t last_user_activity_time_ = 0;
    bool is_idle_ = false;
    std::atomic<int> pending_dialog_results_size_{0};

    // Dirty flags for smart redraw
    bool ui_dirty_menu_ = true;
    bool menu_rendered_ = false;  // Флаг: меню уже отрисовано один раз

    // Workspace state
    bool workspace_just_loaded_ = false;  // Флаг: workspace только что загружен
    
    // File dialog paths
    std::string last_open_path_;
    std::string last_save_path_;

    // Conversation file tracking
    std::string current_conversation_path_;  // Path to currently loaded conversation file
    bool conversation_modified_ = false;      // Track if current conversation was modified
    
    // Custom styling
    ImFont* default_font_ = nullptr;
    ImFont* monospace_font_ = nullptr;
    float ui_scale_ = 1.0f;
    
    // Layout configuration
    float left_panel_width_ = 250.0f;
    float right_panel_width_ = 300.0f;
    float bottom_panel_height_ = 200.0f;
    
    // Async dialog result handling
    std::mutex pending_dialog_results_mutex_;
    std::queue<std::pair<std::string, std::string>> pending_dialog_results_;
    std::unique_ptr<PendingDialogResult> pending_dialog_result_;

    // Model selection state
    std::string pending_model_path_;
    bool show_model_selection_dialog_ = false;

    // Model loading dialog state
    bool show_model_load_dialog_ = false;
    float model_load_progress_ = 0.0f;
    std::string model_load_status_;
    std::string pending_query_;  // Запрос, который ждет загрузки модели
    std::string model_load_pending_query_;  // Запрос, который ждет загрузки модели (для start_model_load_from_profile)
    bool model_load_at_startup_ = false;  // Флаг загрузки при старте
    std::atomic<bool> is_model_loading_{false};  // Флаг загрузки модели (для блокировки запросов)
    
    // File dialog state - prevents multiple dialogs from opening simultaneously
    std::atomic<bool> file_dialog_open_{false};

    // Workspace list cache (to avoid scanning directory every frame)
    mutable std::vector<std::string> workspace_list_cache_;
    mutable uint32_t workspace_list_cache_time_ = 0;  // 0 = cache invalid
    static constexpr uint32_t WORKSPACE_CACHE_DURATION_MS = 2000;  // 2 seconds

    // Methods to create UI components
    void create_ui_components();
    void destroy_ui_components();

    // Model management methods
    void load_selected_model(const std::string& model_path);
    void load_model_with_progress_dialog(const std::string& model_path, const std::string& pending_query, bool at_startup = false);
    void unload_current_model();

    // Model selection methods
    void open_model_selection_dialog();
    void render_model_selection_dialog();
    void render_settings_dialog();
    void render_rag_settings_dialog();
    void render_cloud_services_dialog();
    void render_settings_viewer_dialog();
    void render_grid_snapping_dialog();
    void render_profile_manager_dialog();
    void render_backup_manager_dialog();
#ifdef ENABLE_LLAMA_BENCH
    void render_llama_bench_dialog();
#endif
    void handle_model_file_selection(const std::string& model_path);
    void show_model_reload_confirmation(const std::string& new_model_path);
    void load_model_and_restart_server(const std::string& model_path);

    // Model loading dialog
    void render_model_load_dialog();
    void start_model_load_from_profile(const std::string& pending_query);

    // Conversation file dialog methods
    bool open_conversation_file_dialog(std::string& selected_path);
    bool save_conversation_file_dialog(std::string& selected_path);
    bool try_zenity_conversation_file_dialog(bool save, std::string& selected_path);
    bool try_kdialog_conversation_file_dialog(bool save, std::string& selected_path);
    bool try_python_conversation_file_dialog(bool save, std::string& selected_path);

    // Model file dialog methods
    bool try_open_model_file_dialog(std::string& selected_path);
    bool try_zenity_model_file_dialog(std::string& selected_path);
    bool try_kdialog_model_file_dialog(std::string& selected_path);
    bool try_python_model_file_dialog(std::string& selected_path);

    // Async dialog result processing
    void process_pending_dialog_results();

    // Utility methods
    std::string get_config_directory() const;
    std::string get_cache_directory() const;
    bool ensure_directory_exists(const std::string& path) const;

    // Font rebuild
    bool checkAndRebuildFonts();
    float last_font_size_ = 0.0f;
};

} // namespace ui
} // namespace llama_gui

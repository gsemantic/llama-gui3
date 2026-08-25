#include "main_window.h"
#include "headless_browser_panel.h"
#include "advanced_menu_system.h"
#include "command.h"
#include "settings_dialog.h"
#include "quick_settings_dialog.h"
#include "model_manager.h"
#include "workspace_manager.h"
#include "window_manager.h"
#include "conversation_file_manager.h"
#include "layout_controller.h"
#include "input_handler.h"
#include "core/logger.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <filesystem>
#include <unistd.h>
#include <SDL2/SDL.h>

#include "../external/imgui/backends/imgui_impl_sdl2.h"
#include "../external/imgui/backends/imgui_impl_opengl3.h"
#include "../external/imgui/imgui_internal.h"

namespace llama_gui {
namespace ui {

// Запрос корректной остановки из обработчика сигналов (см. requestExternalStop)
std::atomic<bool> MainWindow::external_stop_flag_{false};

namespace {

// ---- Буфер обмена --------------------------------------------------------
// SDL2 на X11 (2.26.x) периодически возвращает пустую строку из
// SDL_GetClipboardText() (известный баг с владельцем выборки). Пока буфер
// системный работает (xclip читает) — подстраховываемся xclip.

std::string run_clipboard_tool(const char* cmd) {
    std::string out;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return out;
    char buf[4096];
    std::size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);
    pclose(pipe);
    return out;
}

std::string g_clipboard_text;

const char* app_get_clipboard_text(ImGuiContext*) {
    g_clipboard_text.clear();
    bool sdl_empty = true;
    if (char* sdl_text = SDL_GetClipboardText()) {
        if (*sdl_text) {
            g_clipboard_text = sdl_text;
            sdl_empty = false;
        }
        SDL_free(sdl_text);
    }
    if (sdl_empty) {
        g_clipboard_text = run_clipboard_tool("xclip -selection clipboard -o 2>/dev/null");
        std::cout << "[clipboard] SDL_GetClipboardText пуст, xclip вернул "
                  << g_clipboard_text.size() << " байт" << std::endl;
    }
    return g_clipboard_text.c_str();
}

void app_set_clipboard_text(ImGuiContext*, const char* text) {
    if (!text) text = "";
    if (SDL_SetClipboardText(text) != 0) {
        if (FILE* pipe = popen("xclip -selection clipboard 2>/dev/null", "w")) {
            fwrite(text, 1, std::strlen(text), pipe);
            pclose(pipe);
        }
    }
}

} // namespace

MainWindow::MainWindow(StateManager& state_manager, Settings& settings, LlamaInterface& llama_interface)
    : settings_(settings)
    , state_manager_(state_manager)
    , llama_interface_(llama_interface)
    , server_manager_(std::make_unique<ServerManager>(settings_))
    , rag_manager_(nullptr)
    , chat_interface_(std::make_unique<ChatInterface>(state_manager_, settings_, llama_interface_))
    , file_manager_(std::make_unique<FileManager>(state_manager_, settings_))
    , conversation_manager_(std::make_unique<ConversationManager>(state_manager_))
    , settings_dialog_(std::make_unique<SettingsDialog>(settings_, server_manager_.get(), &workspace_manager_, &config_manager_))
    , cloud_services_dialog_(std::make_unique<CloudServicesDialog>(settings_))
    , rag_settings_dialog_(std::make_unique<RagSettingsDialog>(&settings_, this))
    , settings_viewer_dialog_(std::make_unique<SettingsViewerDialog>(settings_))
    , grid_snapping_dialog_(std::make_unique<GridSnappingDialog>())
    , profile_manager_dialog_(std::make_unique<ProfileManagerDialog>(config_manager_))
    , backup_manager_dialog_(std::make_unique<BackupManagerDialog>(config_manager_))
    , quick_settings_dialog_(std::make_unique<QuickSettingsDialog>(settings_))
#ifdef ENABLE_LLAMA_BENCH
    , llama_bench_dialog_(std::make_unique<LlamaBenchDialog>())
#endif
    , command_manager_(std::make_unique<CommandManager>())
    , conversation_file_manager_(std::make_unique<ConversationFileManager>(state_manager_))
    , file_dialog_manager_(std::make_unique<FileDialogManager>())
    , performance_monitor_(std::make_unique<PerformanceMonitor>())
    , layout_controller_()
    , input_handler_()
    , plugin_manager_(std::make_unique<llama_gui::plugin::PluginManager>()) {

    // Initialize advanced menu system
    advanced_menu_system_.initialize(command_manager_.get(), &window_manager_, &workspace_manager_);
    advanced_menu_system_.buildModernMenu();

#ifdef ENABLE_LLAMA_BENCH
    // Передаём server manager в диалог Llama Bench (для остановки/запуска сервера)
    if (llama_bench_dialog_) {
        llama_bench_dialog_->setServerManager(server_manager_);
    }
#endif

    // Привязываем WorkspaceManager к ProfileManagerDialog для редактирования видимости меню
    if (profile_manager_dialog_) {
        profile_manager_dialog_->setWorkspaceManager(&workspace_manager_);
        // После загрузки профиля перезагружаем макет окон, чтобы размеры/позиции не слетали
        profile_manager_dialog_->setProfileLoadCallback([this](const std::string& profile_name) {
            std::cout << "MainWindow: Profile loaded '" << profile_name
                      << "', reloading workspace layout" << std::endl;
            // Перезагружаем последнюю сессию — позиции/размеры берутся из __last_session__.json
            load_workspace("__last_session__");
        });
    }

    // Pass cloud_services_dialog pointer to settings_dialog (for button in server chat settings)
    settings_dialog_->set_cloud_services_dialog(cloud_services_dialog_.get());

    // Set callback to reconnect LlamaInterface after server restart
    settings_dialog_->set_server_started_callback([this]() {
        std::cout << "MainWindow: Scheduling LlamaInterface reconnect..." << std::endl;
        // Schedule reconnect for next frame (non-blocking)
        pending_reconnect_ = true;
    });

    // Set callback for model change - restart server with new model
    settings_dialog_->set_model_changed_callback([this](const std::string& model_path) {
        std::cout << "MainWindow: Model changed to '" << model_path << "', restarting server" << std::endl;
        if (server_manager_) {
            server_manager_->set_model_path(model_path);
            server_manager_->restart_server();
            pending_reconnect_ = true;
        }
    });

    // Set model browse callback for file dialog in settings
    settings_dialog_->setModelBrowseCallback([this](std::function<void(const std::string&)> done) {
        file_dialog_manager_->pick_file("Select Model File", std::move(done), "", "model_files");
    });

    // Set model browse callback for quick settings dialog
    quick_settings_dialog_->setBrowseCallback([this](std::function<void(const std::string&)> done) {
        file_dialog_manager_->pick_file("Select Model File", std::move(done), "", "model_files");
    });

    // File dialogs in FileManager go through FileDialogManager (native or built-in picker)
    if (file_manager_) {
        file_manager_->set_file_dialog_manager(file_dialog_manager_.get());
    }

    // Set server control callbacks for quick settings dialog
    quick_settings_dialog_->setStartServerCallback([this]() {
        if (server_manager_) {
            server_manager_->start_server();
        }
    });
    quick_settings_dialog_->setStopServerCallback([this]() {
        if (server_manager_) {
            server_manager_->stop_server();
        }
    });
    quick_settings_dialog_->setRestartServerCallback([this]() {
        if (server_manager_) {
            server_manager_->set_model_path(settings_.get_model_path());
            server_manager_->restart_server();
            pending_reconnect_ = true;
        }
    });
    quick_settings_dialog_->setModelChangedCallback([this]() {
        if (server_manager_) {
            server_manager_->set_model_path(settings_.get_model_path());
            server_manager_->restart_server();
            pending_reconnect_ = true;
        }
    });

    // Initialize localization system (Russian is the default language)
    auto& loc_manager = getLocalizationManager();
    loc_manager.loadTranslationsFromDirectory("translations");
    loc_manager.loadTranslationsFromDirectory("i18n");
    {
        std::string saved_language = settings_.display().language;
        if (saved_language.empty() || !loc_manager.isLanguageAvailable(saved_language)) {
            saved_language = "ru";
        }
        loc_manager.setCurrentLanguage(saved_language);
    }

    // Set up model selection callback
    chat_interface_->set_model_selection_callback([this]() {
        open_model_selection_dialog();
    });

    // Set up model load request callback
    chat_interface_->set_model_load_request_callback([this](const std::string& pending_query) {
        start_model_load_from_profile(pending_query);
    });

    // Pass model loading flag to ChatInterface
    chat_interface_->set_is_model_loading(&is_model_loading_);

    // Pass model loading progress and status to ChatInterface
    chat_interface_->set_model_load_progress(&model_load_progress_);
    chat_interface_->set_model_load_status(&model_load_status_);

    // File dialogs через встроенный пикер (FileDialogManager)
    chat_interface_->set_file_dialog_manager(file_dialog_manager_.get());

    // Set up attachment sync
    file_manager_->set_attachment_changed_callback([this](const std::vector<std::string>& attachments) {
        chat_interface_->set_attachments(attachments);
    });

    // Set up model load callback from FileManager
    file_manager_->set_model_load_callback([this](const std::string& model_path) {
        load_model_from_path(model_path);
    });

    // Set up file content callback
    file_manager_->set_file_content_callback([this](const std::string& content) {
        chat_interface_->set_input_text(content);
    });

    // Set up callbacks
    state_manager_.set_conversation_change_callback(
        [this](const llama_gui::core::StateEvent& event) {
            on_conversation_changed(event);
        }
    );

    // Adapt UI components
    settings_.adapt_ui_components();

    // Set up language change callback AFTER all components are initialized.
    // IMPORTANT: we must NOT rebuild the menu synchronously here — this callback
    // fires while the menu bar is being rendered (from the language submenu click),
    // and clearing the menu structures mid-frame corrupts the menu and window layout.
    // The rebuild is deferred to the pending_language_change_ block in run(),
    // which executes BEFORE ImGui::NewFrame().
    loc_manager.setLanguageChangeCallback([this](Language new_lang, Language old_lang) {
        // Persist the chosen language so it survives restarts
        settings_.display().language = getLocalizationManager().getCurrentLanguageCode();
        std::string profile = settings_.get_current_profile_name();
        settings_.save_profile(profile.empty() ? "default" : profile);
        pending_language_change_ = true;
    });

    // Меню уже построено ДО применения сохранённого языка (buildModernMenu() выше),
    // поэтому оно могло остаться на языке по умолчанию. Принудительно пересобираем
    // меню на первом кадре, чтобы текст соответствовал выбранному языку.
    pending_language_change_ = true;

    // Initialize UI dirty flags
    force_ui_update_ = false;
    ui_dirty_menu_ = true;

    // Initialize the new UI management system
    initializeNewUISystem();

    // is_initialized_ is false by default, will be set in initialize()
}

MainWindow::~MainWindow() {
    shutdown();
}

bool MainWindow::initialize(int width, int height) {
    auto [screen_width, screen_height] = settings_.get_safe_window_bounds();

    if (width <= 0 || height <= 0) {
        width_ = screen_width;
        height_ = screen_height;
    } else {
        width_ = width;
        height_ = height;
    }

    LOG_INFO("MainWindow инициализация: " + std::to_string(width_) + "x" + std::to_string(height_));

    // === Create SDL2 window ===
#ifdef USE_SDL2
    Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    sdl_window_ = SDL_CreateWindow(
        title_.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width_,
        height_,
        window_flags
    );
    if (!sdl_window_) {
        std::cerr << "❌ Failed to create SDL2 window: " << SDL_GetError() << std::endl;
        if (!audit_ui_mode_) return false;
        // Аудиту рендер не нужен: продолжаем без окна
        LOG_WARNING("UI Audit mode: работаем без SDL-окна");
    } else {
        LOG_INFO("SDL2 окно создано успешно");
    }
    
    // === Create OpenGL context ===
#ifdef USE_OPENGL
    gl_context_ = SDL_GL_CreateContext(sdl_window_);
    if (!gl_context_) {
        std::cerr << "❌ Failed to create OpenGL context: " << SDL_GetError() << std::endl;
        if (!audit_ui_mode_) return false;
        LOG_WARNING("UI Audit mode: работаем без OpenGL-контекста");
    } else {
        LOG_INFO("OpenGL контекст создан успешно");

        // V-Sync из настроек (раньше настройка существовала, но не применялась)
        applied_swap_interval_ = settings_.performance().enable_vsync ? 1 : 0;
        SDL_GL_SetSwapInterval(applied_swap_interval_);
    }
#endif
#endif

    // === Инициализация ConfigManager ===
    // Привязываем внешний Settings (тот же объект что и в main.cpp)
    config_manager_.bindExternalSettings(&settings_);
    if (!config_manager_.initialize()) {
        std::cerr << "⚠ Не удалось инициализировать ConfigManager" << std::endl;
    }

    // Привязываем WorkspaceManager к ProfileManager для интеграции workspace в профили
    config_manager_.getProfileManager().bindWorkspaceManager(&workspace_manager_);

    // Загружаем последнее рабочее пространство (если есть)
    load_workspace("__last_session__");

    // Initialize core components
    state_manager_.initialize(settings_);

    // Initialize RAG manager
    std::string embedding_model_path = settings_.get_embedding_model_path();
    if (audit_ui_mode_) {
        LOG_INFO("UI Audit mode: RAG и эмбеддинг-сервер не запускаются");
    } else if (!embedding_model_path.empty()) {
        // Запускаем выделенный сервер эмбеддингов (bge-m3) при старте,
        // если пользователь не указал внешний URL сервера вручную.
        std::string auto_embedding_url;
        if (settings_.rag().embedding_server_url.empty()) {
            embedding_server_ = std::make_unique<llama_gui::core::EmbeddingServer>();
            embedding_server_->set_model_path(embedding_model_path);
            // Если пользователь настроил свой путь к бинарю llama-server — используем его.
            // Иначе оставляем автоопределённый абсолютный путь (в settings.ini лежит
            // голое имя "llama-server", которого нет в PATH).
            if (settings_.server_runtime().server_binary_path != "llama-server") {
                embedding_server_->set_server_binary_path(settings_.server_runtime().server_binary_path);
            }
            if (embedding_server_->start_server()) {
                auto_embedding_url = embedding_server_->get_server_url();
                LOG_INFO("EmbeddingServer запущен: " + auto_embedding_url + " (модель: " + embedding_model_path + ")");
            } else {
                LOG_ERROR("EmbeddingServer не удалось запустить для модели: " + embedding_model_path);
            }
        }

        rag_manager_ = std::make_unique<llama_gui::core::RagManager>(embedding_model_path);
        if (rag_manager_) {
            rag_manager_->initialize_indexes();

            // Если пользователь не указал внешний URL — используем наш встроенный сервер
            if (!auto_embedding_url.empty()) {
                rag_manager_->set_embedding_server_url(auto_embedding_url);
            }

            // Применяем настройки RAG (URL сервера эмбеддингов, размерность и т.п.)
            rag_manager_->update_from_settings(settings_.rag());

            chat_interface_->set_rag_manager(rag_manager_.get());

            bool rag_enabled = settings_.rag().enable_rag;
            chat_interface_->enable_rag(rag_enabled);

            // Передаём RAG manager в FileManager для обработки вложений
            file_manager_->set_rag_manager(rag_manager_.get());
            file_manager_->enable_rag(rag_enabled);

            rag_interface_ = std::make_unique<RagInterface>();
            rag_interface_->set_rag_manager(rag_manager_.get());
            rag_interface_->set_rag_settings_dialog(rag_settings_dialog_.get());
            rag_interface_->set_chat_interface(chat_interface_.get());
            rag_interface_->set_settings(&settings_);
            rag_interface_->set_file_dialog_manager(file_dialog_manager_.get());
            rag_interface_->set_enabled(rag_enabled);

            // Connect ChatInterface to RagInterface for mini indicator
            chat_interface_->set_rag_interface(rag_interface_.get());
            LOG_INFO("RagManager инициализирован с моделью: " + embedding_model_path);
        }
    } else {
        LOG_INFO("Путь к модели эмбеддингов не указан. RAG будет недоступен до настройки.");
    }

    // Pass WindowManager to ChatInterface for dock support
    chat_interface_->set_window_manager(&window_manager_);

    // Панель headless-браузера (Chromium): рендеринг страниц, отправка в чат.
    // Не путать с «серверным режимом (без GUI)» приложения (флаг --headless).
    headless_browser_panel_ = std::make_unique<HeadlessBrowserPanel>();
    headless_browser_panel_->set_chat_interface(chat_interface_.get());

    // Подсистема агентов: загрузка agent-плагинов (в т.ч. web_render_agent —
    // headless-браузер/Chromium) и интеграция с чатом. Не путать с
    // «серверным режимом (без GUI)» приложения (флаг --headless).
    initialize_agent_system();

    // Set up model progress callback
    // TODO: Implement set_model_progress_callback in ModelManager
    // model_manager_->set_model_progress_callback([this](const std::string& model_name, float progress, const std::string& status) {
    //     std::cout << "Model Load Progress: " << (progress * 100.0f) << "% - " << status << std::endl;
    // });

    // Set up chat template callback
    // TODO: Implement set_chat_template_callback in ModelManager
    // model_manager_->set_chat_template_callback([this](const std::string& model_path, const std::string& chat_template, const std::string& source) {
    //     std::cout << "[MainWindow] Saving chat template to settings (source: " << source << ")" << std::endl;
    //     std::string template_file = "/tmp/llama-gui-chat-template.jinja";
    //     std::ofstream file(template_file);
    //     if (file.is_open()) {
    //         file << chat_template;
    //         file.close();
    //         settings_.grammar().chat_template_file = template_file;
    //     } else {
    //         std::cerr << "[MainWindow] Failed to save chat template to file" << std::endl;
    //         settings_.grammar().chat_template = chat_template;
    //     }
    //     settings_.grammar().use_jinja = true;
    // });

    // Auto-load model from profile (skip if cloud provider is enabled)
    if (audit_ui_mode_) {
        LOG_INFO("UI Audit mode: автозагрузка модели пропущена");
    } else if (settings_.cloud_provider().enabled && !settings_.cloud_provider().model_id.empty()) {
        LOG_INFO("Cloud provider enabled, skipping local model load");
    } else {
        std::string model_path = settings_.get_model_path();
        if (!model_path.empty()) {
            LOG_INFO("Автоматическая загрузка модели из профиля: " + model_path);
            load_model_with_progress_dialog(model_path, "", true);
        } else {
            LOG_INFO("Путь к модели не указан в профиле");
        }
    }

    // Initialize Stage 3 managers
    conversation_file_manager_->setConversationModified(conversation_modified_);
    // TODO: Implement setDeveloperModeEnabled in PerformanceMonitor
    // performance_monitor_->setDeveloperModeEnabled(developer_mode_enabled_);

    // Initialize CommandManager with default commands and window toggle callback
    std::cout << "[DEBUG] Before initializeDefaultCommands()" << std::endl;
    try {
        command_manager_->setToggleWindowCallback([this](const std::string& window_name) {
            window_manager_.toggleWindow(window_name);
            if (window_manager_.isWindowVisible(window_name)) {
                // Открываем диалоговое окно через его собственный метод (если нужно),
                // затем поднимаем окно наверх (порядок отрисовки ImGui зависит от
                // фокуса, а не от порядка Begin()).
                showWindowByName(window_name);
                window_coordinator_.bringToFront(window_name);
            } else {
                hideWindowByName(window_name);
            }
            syncWindowFlagsFromManager();
        });
        command_manager_->initializeDefaultCommands(
            [this]() { open_conversation_file(); },
            // Только запрашиваем выход из цикла. Нельзя вызывать shutdown()
            // прямо здесь: команда исполняется ВНУТРИ кадра рендер-цикла run(),
            // и shutdown() (через cleanup_sdl2 -> SDL_Quit + ImGui::DestroyContext)
            // уничтожил бы SDL/ImGui-бэкенд, тогда как цикл продолжил бы
            // рисовать текущий кадр -> SIGSEGV в IsItemHovered (см. imgui-error
            // "Forgot to shutdown Platform/Renderer backend?"). Реальную выгрузку
            // выполняет ~MainWindow (вызывает shutdown()) после выхода из цикла.
            [this]() { is_running_ = false; },
            [this]() { open_settings(); },
            [this]() {
                file_dialog_manager_->pick_file(
                    "Select Model File",
                    [this](const std::string& model_path) {
                        if (!model_path.empty()) {
                            load_model_from_path(model_path);
                        }
                    },
                    "", "model_files");
            },
            [this](ServerControlCommand::Action action) { on_server_control_command(action); },
            [this](const std::string& help_type) { show_help(help_type); }
        );
        std::cout << "[DEBUG] After initializeDefaultCommands()" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "ERROR in initializeDefaultCommands(): " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "UNKNOWN ERROR in initializeDefaultCommands()" << std::endl;
    }

    // Подключаем файловые колбэки к CommandManager
    command_manager_->setOpenFileCallback([this]() {
        // Встроенный пикер — ОСНОВНОЙ путь; нативный диалог — опциональный ускоритель
        file_dialog_manager_->pick_file(
            "Open Conversation",
            [this](const std::string& path) {
                if (!path.empty()) {
                    conversation_file_manager_->openConversationFile(path);
                }
            },
            "", "json_files");
    });
    command_manager_->setSaveFileCallback([this]() {
        if (conversation_file_manager_->getCurrentConversationPath().empty()) {
            file_dialog_manager_->pick_save("Save Conversation", "conversation.json",
                [this](const std::string& path) {
                    if (!path.empty()) {
                        conversation_file_manager_->saveCurrentConversationAs(path);
                    }
                });
        } else {
            conversation_file_manager_->saveCurrentConversation();
        }
    });
    command_manager_->setSaveFileAsCallback([this](const std::string&) {
        file_dialog_manager_->pick_save("Save Conversation As", "conversation.json",
            [this](const std::string& path) {
                if (!path.empty()) {
                    conversation_file_manager_->saveCurrentConversationAs(path);
                }
            });
    });

    std::cout << "[DEBUG] Before connectWorkspaceCommands()" << std::endl;
    // Connect workspace commands
    try {
        connectWorkspaceCommands();
        std::cout << "[DEBUG] After connectWorkspaceCommands()" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "ERROR in connectWorkspaceCommands(): " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "UNKNOWN ERROR in connectWorkspaceCommands()" << std::endl;
    }

    // Connect additional window toggle commands
    std::cout << "[DEBUG] Before connectAdditionalWindowCommands()" << std::endl;
    try {
        connectAdditionalWindowCommands();
        std::cout << "[DEBUG] After connectAdditionalWindowCommands()" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "ERROR in connectAdditionalWindowCommands(): " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "UNKNOWN ERROR in connectAdditionalWindowCommands()" << std::endl;
    }

    // Connect settings menu commands
    std::cout << "[DEBUG] Before connectSettingsMenuCommands()" << std::endl;
    try {
        connectSettingsMenuCommands();
        std::cout << "[DEBUG] After connectSettingsMenuCommands()" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "ERROR in connectSettingsMenuCommands(): " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "UNKNOWN ERROR in connectSettingsMenuCommands()" << std::endl;
    }

    // Регистрируем вторичные команды (Security/Performance/Logging/Debug/Tools)
    try {
        connectSecondaryCommands();
    } catch (const std::exception& e) {
        std::cerr << "ERROR in connectSecondaryCommands(): " << e.what() << std::endl;
    }

    // Живые галочки пунктов меню (после регистрации команд)
    applyMenuToggleBindings();

    // Register developer commands (Dear ImGui tools)
    try {
        connectDeveloperCommands();
    } catch (const std::exception& e) {
        std::cerr << "ERROR in connectDeveloperCommands(): " << e.what() << std::endl;
    }

    // Sync all window flags after initialization
    syncWindowFlagsFromManager();

    // Инициализация системы плагинов (после готовности всех подсистем)
    try {
        initializePlugins();
    } catch (const std::exception& e) {
        std::cerr << "ERROR initializing plugins: " << e.what() << std::endl;
    }

    is_initialized_ = true;
    LOG_INFO("MainWindow инициализирован успешно");

    return true;
}

void MainWindow::run() {
    if (!is_initialized_) {
        std::cerr << "MainWindow not initialized" << std::endl;
        return;
    }

#ifdef USE_SDL2
    if (!sdl_window_ || !gl_context_) {
        std::cerr << "❌ SDL2 or OpenGL not initialized, cannot run GUI" << std::endl;
        return;
    }
#endif

    is_running_ = true;
    LOG_INFO("Запуск главного цикла GUI");

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Setup Dear ImGui IO
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup Dear ImGui style (reads theme from settings)
    setup_imgui_style();

#ifdef USE_SDL2
    if (sdl_window_ && gl_context_) {
        if (!ImGui_ImplSDL2_InitForOpenGL(sdl_window_, gl_context_)) {
            std::cerr << "❌ Failed to initialize ImGui SDL2 backend!" << std::endl;
            return;
        }
    }
#endif

    // Надёжный буфер обмена: SDL на X11 иногда отдаёт пустую строку —
    // подстраховываемся xclip (ставим ПОСЛЕ инициализации SDL2-бэкенда).
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Platform_GetClipboardTextFn = app_get_clipboard_text;
    platform_io.Platform_SetClipboardTextFn = app_set_clipboard_text;

#ifdef USE_OPENGL
    if (gl_context_) {
        if (!ImGui_ImplOpenGL3_Init("#version 130")) {
            std::cerr << "❌ Failed to initialize ImGui OpenGL backend!" << std::endl;
            return;
        }
    }
#endif

    // Load fonts
    load_fonts_with_cyrillic();

    // Применяем ini-blob из профиля __last_session__ (collapsed/скроллы/колонки),
    // загруженный ещё в initialize() до создания контекста — до первого NewFrame()
    workspace_layout_manager_.applyPendingImguiIni();

    // Main loop
    last_ui_activity_ms_ = SDL_GetTicks();
    while (is_running_) {
        // Запрос остановки извне (SIGTERM/SIGINT из main.cpp) — корректное
        // завершение с сохранением сессии, как при закрытии окна.
        if (external_stop_flag_.load()) {
            std::cout << "Получен внешний запрос остановки: завершаем цикл GUI" << std::endl;
            is_running_ = false;
        }
        // Отложенное восстановление z-order окон (после создания окон первыми кадрами)
        workspace_layout_manager_.tickDeferredApply();
        const Uint32 frame_start_ms = SDL_GetTicks();
#ifdef USE_SDL2
        if (sdl_window_) {
            // Process SDL events: forward to both InputHandler AND ImGui
            SDL_Event event;
            bool got_events = false;
            while (SDL_PollEvent(&event)) {
                got_events = true;
                input_handler_.handleSDLEvent(event);
                if (sdl_window_ && gl_context_) {
                    ImGui_ImplSDL2_ProcessEvent(&event);
                }
                if (event.type == SDL_QUIT) {
                    is_running_ = false;
                }
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                    event.window.windowID == SDL_GetWindowID(sdl_window_)) {
                    is_running_ = false;
                }
            }
            // Любое событие ввода/окна выводит UI из idle-режима на полную частоту
            if (got_events) {
                last_ui_activity_ms_ = SDL_GetTicks();
            }
        }
#endif

        // Process pending language change BEFORE ImGui NewFrame.
        // Rebuilding the menu here is safe: it happens outside of any ImGui
        // rendering pass, so the menu structures and window layout stay intact.
        if (pending_language_change_) {
            advanced_menu_system_.rebuildModernMenu();
            ui_dirty_menu_ = true;
            // Пересборка стёрла привязки check_func — восстанавливаем
            applyMenuToggleBindings();
            // Локализованные заголовки окон изменились (TR(...) в ImGui::Begin),
            // иначе ImGui создал бы окна заново и сбросил их позиции/размеры.
            refreshLocalizedWindowNames();
            workspace_applier_.requestApply();
            pending_language_change_ = false;
        }

        // Check if font size changed and rebuild if needed
        checkAndRebuildFonts();

        // Process pending LlamaInterface reconnect (non-blocking, throttled)
        if (pending_reconnect_) {
            // Облачный режим: запросы идут напрямую к провайдеру (OpenRouter и пр.),
            // локальный бэкенд не требуется — сбрасываем запрос реконнекта.
            const bool cloud_active = settings_.cloud_provider().enabled &&
                                      !settings_.cloud_provider().model_id.empty();
            if (cloud_active) {
                std::cout << "MainWindow: облачный режим активен — реконнект к "
                             "локальному серверу не требуется" << std::endl;
                pending_reconnect_ = false;
                chat_interface_->set_server_ready(true);
            } else {
            static uint32_t last_reconnect_attempt_ = 0;
            static uint32_t reconnect_fail_count_ = 0;
            uint32_t now = SDL_GetTicks();
            if (now - last_reconnect_attempt_ > 2000) {  // Retry every 2 seconds max
                last_reconnect_attempt_ = now;
                // Токен/SSL берём из настроек при каждом переподключении —
                // изменения на вкладке Security подхватываются без перезапуска
                llama_interface_.set_api_key(settings_.server().auth_token);
                llama_interface_.set_ssl_verify(settings_.server().verify_ssl);
                if (llama_interface_.initialize(settings_.get_server_url())) {
                    std::cout << "MainWindow: LlamaInterface connected successfully" << std::endl;
                    pending_reconnect_ = false;
                    reconnect_fail_count_ = 0;
                    chat_interface_->set_server_ready(true);
                } else {
                    // Не спамим лог каждые 2 секунды: первая попытка и далее раз в ~30с
                    ++reconnect_fail_count_;
                    if (reconnect_fail_count_ == 1 || reconnect_fail_count_ % 15 == 0) {
                        std::cout << "MainWindow: Server not ready yet ("
                                  << settings_.get_server_url()
                                  << "), retrying every 2s (attempt "
                                  << reconnect_fail_count_ << ")" << std::endl;
                    }
                }
            }
            }
        }

        // Track model loading progress: poll server health and update progress
        if (is_model_loading_) {
            static uint32_t last_poll_time_ = 0;
            uint32_t now = SDL_GetTicks();
            if (now - last_poll_time_ > 3000) {  // Poll every 3 seconds to avoid blocking UI
                last_poll_time_ = now;
                if (llama_interface_.is_server_healthy()) {
                    model_load_progress_ = 1.0f;
                    model_load_status_ = "Model loaded successfully";
                    is_model_loading_ = false;
                    chat_interface_->set_server_ready(true);
                    // Reconnect LlamaInterface to the now-ready server
                    llama_interface_.initialize(settings_.get_server_url());
                    std::cout << "MainWindow: Server ready, model loaded" << std::endl;
                } else {
                    // Increment progress gradually to show activity
                    if (model_load_progress_ < 0.9f) {
                        model_load_progress_ += 0.1f;
                        model_load_status_ = "Loading model...";
                    } else {
                        model_load_status_ = "Waiting for server...";
                    }
                }
            }
        }

#ifdef USE_OPENGL
        if (gl_context_) {
            ImGui_ImplOpenGL3_NewFrame();
        }
#endif
#ifdef USE_SDL2
        if (sdl_window_ && gl_context_) {
            ImGui_ImplSDL2_NewFrame();
        }
#endif
        ImGui::NewFrame();

        // Render main menu after NewFrame
        if (advanced_menu_system_.isMenuBuilt()) {
            advanced_menu_system_.renderMainMenu();
        }

        // Render UI
        render();

        // Rendering
        ImGui::Render();

#ifdef USE_OPENGL
        glViewport(0, 0, (int)ImGui::GetIO().DisplaySize.x, (int)ImGui::GetIO().DisplaySize.y);
        // Фон главного окна — зависит от темы
        if (settings_.is_dark_theme()) {
            glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        } else {
            glClearColor(0.85f, 0.85f, 0.85f, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif

#ifdef USE_SDL2
        SDL_GL_SwapWindow(sdl_window_);
#endif

        // === Ограничение частоты кадров (target_fps / idle_fps) ===
        // Ранее настройки существовали, но цикл рендерил без ограничений.
        const auto& perf = settings_.performance();

        // Runtime-переключение V-Sync из настроек
#ifdef USE_OPENGL
        if (gl_context_) {
            const int want_interval = perf.enable_vsync ? 1 : 0;
            if (want_interval != applied_swap_interval_) {
                applied_swap_interval_ = want_interval;
                SDL_GL_SetSwapInterval(want_interval);
            }
        }
#endif

        // Занятые состояния требуют полной частоты независимо от простоя мыши:
        // стриминг ответа, загрузка модели, RAG-индексация
        const bool ui_busy = (chat_interface_ && chat_interface_->is_streaming())
                          || is_model_loading_.load()
                          || (rag_interface_ && rag_interface_->is_indexing());

        const Uint32 frame_end_ms = SDL_GetTicks();
        const bool idle_mode = !ui_busy &&
            (frame_end_ms - last_ui_activity_ms_ > static_cast<Uint32>(perf.idle_timeout_ms));
        const int limit_fps = std::max(1, idle_mode ? perf.idle_fps : perf.target_fps);
        const Uint32 frame_budget_ms = static_cast<Uint32>(1000 / limit_fps);
        const Uint32 frame_elapsed_ms = frame_end_ms - frame_start_ms;
        if (frame_elapsed_ms < frame_budget_ms) {
            SDL_Delay(frame_budget_ms - frame_elapsed_ms);
        }
    }

    // Сохраняем workspace ПОКА ImGui контекст ещё жив — иначе позиции/размеры окон недоступны
    save_workspace("__last_session__");

    // Cleanup
#ifdef USE_OPENGL
    ImGui_ImplOpenGL3_Shutdown();
#endif
#ifdef USE_SDL2
    ImGui_ImplSDL2_Shutdown();
#endif
    ImGui::DestroyContext();
}

void MainWindow::shutdown() {
    // save_workspace("__last_session__") теперь вызывается в run() ДО уничтожения ImGui контекста,
    // потому что WorkspaceLayoutManager::save() читает позиции/размеры из ImGui::GetCurrentContext().
    // Вызов здесь (после destroy Context) привёл бы к сохранению устаревших данных WindowManager.

    // Останавливаем выделенный сервер эмбеддингов (bge-m3)
    if (embedding_server_) {
        embedding_server_->stop_server(true);
        embedding_server_.reset();
        LOG_INFO("EmbeddingServer остановлен");
    }

    // Выгружаем плагины до уничтожения UI-подсистем
    if (plugin_manager_) {
        plugin_manager_->shutdown();
    }

    is_running_ = false;

    // Shutdown SDL2
    cleanup_sdl2();

    // Shutdown OpenGL
    cleanup_opengl();

    // Shutdown Dear ImGui
    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
    }
}

void MainWindow::render() {
    if (!is_initialized_) {
        return;
    }

    // Render all UI through the proper pipeline
    render_ui();
}

void MainWindow::initializeNewUISystem() {
    // Регистрация основных функциональных окон в WindowManager
    window_manager_.addWindow("chat", show_chat_, ImVec2(50, 50), ImVec2(800, 600));
    window_manager_.addWindow("conversations", show_conversations_, ImVec2(10, 100), ImVec2(250, 500));
    window_manager_.addWindow("files", show_files_, ImVec2(870, 100), ImVec2(300, 500));
    window_manager_.addWindow("rag", show_rag_, ImVec2(270, 100), ImVec2(400, 500));
    window_manager_.addWindow("headless_browser", show_headless_browser_, ImVec2(540, 100), ImVec2(700, 600));
    window_manager_.addWindow("agents", show_agents_, ImVec2(400, 100), ImVec2(520, 520));

    // Регистрация диалоговых окон (начально скрыты)
    window_manager_.addWindow("settings", show_settings_, ImVec2(200, 100), ImVec2(400, 600));
    window_manager_.addWindow("cloud_services", show_cloud_services_, ImVec2(200, 100), ImVec2(400, 600));
    window_manager_.addWindow("rag_settings", show_rag_settings_, ImVec2(200, 100), ImVec2(400, 600));
    window_manager_.addWindow("settings_viewer", false, ImVec2(200, 100), ImVec2(400, 600));
    window_manager_.addWindow("status_bar", show_status_bar_);
    window_manager_.addWindow("grid_snapping", false, ImVec2(200, 100), ImVec2(400, 600));
    window_manager_.addWindow("profile_manager", show_profile_manager_, ImVec2(200, 100), ImVec2(400, 600));
    window_manager_.addWindow("backup_manager", show_backup_manager_, ImVec2(200, 100), ImVec2(400, 600));

    // Маппинг: WindowManager имя → ImGui::Begin имя (через TR() для локализации)
    window_manager_.setImGuiName("chat", "Chat");
    window_manager_.setImGuiName("conversations", TR("conversations.title"));
    window_manager_.setImGuiName("files", TR("files.title"));
    window_manager_.setImGuiName("rag", "RAG");
    window_manager_.setImGuiName("headless_browser", "Headless-браузер");
    window_manager_.setImGuiName("agents", "Агенты");
    window_manager_.setImGuiName("profile_manager", "Управление профилями");
    window_manager_.setImGuiName("backup_manager", "Резервные копии");
    window_manager_.setImGuiName("cloud_services", "Cloud Services / Облачные сервисы");
    window_manager_.setImGuiName("settings_viewer", "Settings INI Viewer");
    window_manager_.setImGuiName("settings", "Settings");
    window_manager_.setImGuiName("grid_snapping", TR("grid_snapping.title"));
    window_manager_.setImGuiName("rag_settings", TR("rag_settings.title"));

    // Настройка сетки для позиционирования
    window_manager_.getGridSnappingSystem().setEnabled(true);
    window_manager_.getGridSnappingSystem().setGridSize(20);
    grid_snapping_dialog_->setGridSnappingSystem(&window_manager_.getGridSnappingSystem());

    // Кнопка "Snap all windows": примагничиваем все видимые окна к сетке
    grid_snapping_dialog_->setSnapAllCallback([this]() {
        window_manager_.snapAllWindowsToGrid();
        workspace_applier_.requestApply();
    });

    // Инициализация WindowCoordinator
    window_coordinator_.setWindowManager(&window_manager_);
    window_coordinator_.setWorkspaceApplier(&workspace_applier_);
    workspace_applier_.setWindowManager(&window_manager_);
    workspace_layout_manager_.setWindowManager(&window_manager_);
    window_coordinator_.registerWindow("chat", [this]() {
        if (chat_interface_) chat_interface_->render(&show_chat_);
    }, false, "Chat", &show_chat_);
    window_coordinator_.registerWindow("conversations", [this]() {
        if (conversation_manager_) conversation_manager_->render(&show_conversations_);
    }, false, TR("conversations.title"), &show_conversations_);
    window_coordinator_.registerWindow("files", [this]() {
        if (file_manager_) file_manager_->render(&show_files_);
    }, false, TR("files.title"), &show_files_);
    window_coordinator_.registerWindow("rag", [this]() {
        if (rag_interface_) rag_interface_->render_ui(&show_rag_);
    }, false, "RAG", &show_rag_);
    window_coordinator_.registerWindow("headless_browser", [this]() {
        if (headless_browser_panel_) headless_browser_panel_->render(&show_headless_browser_);
    }, false, "Headless-браузер", &show_headless_browser_);
    window_coordinator_.registerWindow("agents", [this]() {
        if (agent_chat_integration_) {
            agent_chat_integration_->get_agent_panel()->render(&show_agents_);
        }
    }, false, "Агенты", &show_agents_);
    window_coordinator_.registerWindow("settings", [this]() {
        if (settings_dialog_) settings_dialog_->render();
    }, true);
    window_coordinator_.registerWindow("cloud_services", [this]() {
        if (cloud_services_dialog_) cloud_services_dialog_->render();
    }, true);
    window_coordinator_.registerWindow("rag_settings", [this]() {
        if (rag_settings_dialog_) {
            rag_settings_dialog_->render();
            // Process pending file dialog for embedding model path
            if (rag_settings_dialog_->has_pending_file_dialog()) {
                rag_settings_dialog_->open_embedding_model_file_dialog();
            }

            // Когда диалог закрылся — применяем новые настройки к RAG менеджеру
            // (срабатывает и на OK, и на Cancel; на Cancel настройки не менялись,
            // повторное применение безвредно и идемпотентно)
            bool visible_now = rag_settings_dialog_->is_visible();
            if (rag_settings_visible_prev_ && !visible_now) {
                if (rag_manager_) {
                    rag_manager_->update_from_settings(settings_.rag());
                }
            }
            rag_settings_visible_prev_ = visible_now;
        }
    }, true);
    window_coordinator_.registerWindow("settings_viewer", [this]() {
        if (settings_viewer_dialog_) settings_viewer_dialog_->render();
    }, true);
    window_coordinator_.registerWindow("grid_snapping", [this]() {
        if (grid_snapping_dialog_) grid_snapping_dialog_->render(&show_grid_snapping_);
    }, false, TR("grid_snapping.title"), &show_grid_snapping_);
    window_coordinator_.registerWindow("profile_manager", [this]() {
        if (profile_manager_dialog_) profile_manager_dialog_->render();
    }, true);
    window_coordinator_.registerWindow("backup_manager", [this]() {
        if (backup_manager_dialog_) backup_manager_dialog_->render();
    }, false, "Резервные копии");

    // Standalone dialogs (managed by DialogManager)
    window_coordinator_.registerWindow("_workspace_save", [this]() {
        if (show_workspace_save_dialog_) render_workspace_save_dialog();
    }, true);
    window_coordinator_.registerWindow("_workspace_load", [this]() {
        if (show_workspace_load_dialog_) render_workspace_load_dialog();
    }, true);

    // Register keyboard shortcuts with InputHandler
    input_handler_.initialize();

    // Window toggle shortcuts
    input_handler_.registerWindowToggleShortcut("chat", SDLK_F1, KMOD_NONE);
    input_handler_.registerWindowToggleShortcut("conversations", SDLK_F2, KMOD_NONE);
    input_handler_.registerWindowToggleShortcut("files", SDLK_F3, KMOD_NONE);
    input_handler_.registerWindowToggleShortcut("rag", SDLK_F4, KMOD_NONE);
    input_handler_.registerWindowToggleShortcut("settings", SDLK_F5, KMOD_NONE);

    // Action shortcuts
    input_handler_.registerShortcut("save_workspace", SDLK_s, KMOD_CTRL,
        [this]() { save_current_conversation(); });
    input_handler_.registerShortcut("open_settings", SDLK_COMMA, KMOD_CTRL,
        [this]() { open_settings(); });
    input_handler_.registerShortcut("toggle_status_bar", SDLK_F12, KMOD_NONE,
        [this]() {
            window_manager_.toggleWindow("status_bar");
        });
    input_handler_.registerShortcut("reload_fonts", SDLK_EQUALS, KMOD_CTRL,
        [this]() { reload_fonts(); });
    input_handler_.registerShortcut("toggle_fullscreen", SDLK_F11, KMOD_NONE,
        [this]() { toggle_fullscreen(); });

    // Set up shortcut callback to toggle windows
    input_handler_.setShortcutCallback([this](const std::string& window_name) {
        window_manager_.toggleWindow(window_name);
        syncWindowFlagsFromManager();
    });

    // Set up key down callback for general key handling
    input_handler_.setKeyDownCallback([this](SDL_Keycode key, SDL_Keymod mod) {
        // Escape closes focused dialog or resets view
        if (key == SDLK_ESCAPE) {
            if (dialog_manager_.hasVisibleDialogs()) {
                dialog_manager_.closeAllDialogs();
            }
        }
    });

    // Применяем позиции из загруженного workspace (один кадр)
    workspace_applier_.requestApply();
}

void MainWindow::syncWindowFlagsFromManager() {
    show_chat_ = window_manager_.isWindowVisible("chat");
    show_conversations_ = window_manager_.isWindowVisible("conversations");
    show_files_ = window_manager_.isWindowVisible("files");
    show_rag_ = window_manager_.isWindowVisible("rag");
    show_settings_ = window_manager_.isWindowVisible("settings");
    show_cloud_services_ = window_manager_.isWindowVisible("cloud_services");
    show_rag_settings_ = window_manager_.isWindowVisible("rag_settings");
    show_profile_manager_ = window_manager_.isWindowVisible("profile_manager");
    show_backup_manager_ = window_manager_.isWindowVisible("backup_manager");
    show_grid_snapping_ = window_manager_.isWindowVisible("grid_snapping");
    show_status_bar_ = window_manager_.isWindowVisible("status_bar");
    show_headless_browser_ = window_manager_.isWindowVisible("headless_browser");
    show_agents_ = window_manager_.isWindowVisible("agents");
}

void MainWindow::refreshLocalizedWindowNames() {
    // Сначала сохраняем текущие позиции/размеры локализованных окон — они ещё
    // зарегистрированы в WindowManager/WindowCoordinator под СТАРЫМИ ImGui-именами.
    // Затем обновляем ImGui-имена на актуальные переводы и пере-применяем макет,
    // чтобы окно не "прыгало" в позицию по умолчанию (ImGui считает окно с новым
    // именем новым окном и сбрасывает его геометрию).
    auto capture = [this](const std::string& wm_name) {
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (!g) return;
        std::string old_name = window_manager_.getImGuiName(wm_name);
        for (int i = 0; i < g->Windows.Size; i++) {
            ImGuiWindow* w = g->Windows[i];
            if (!w || w->Hidden) continue;
            std::string wname = w->Name;
            auto hash_pos = wname.find("##");
            if (hash_pos != std::string::npos) wname = wname.substr(0, hash_pos);
            if (wname != old_name) continue;
            window_manager_.setWindowPositionRaw(wm_name, w->Pos);
            window_manager_.setWindowSizeRaw(wm_name, w->Size);
            break;
        }
    };

    // Окна, чьи ImGui-имена локализованы через TR() (см. initializeNewUISystem)
    const char* localized_windows[] = {
        "conversations", "files", "grid_snapping", "rag_settings"
    };
    for (const char* wm_name : localized_windows) {
        capture(wm_name);
    }

    window_manager_.setImGuiName("conversations", TR("conversations.title"));
    window_manager_.setImGuiName("files", TR("files.title"));
    window_manager_.setImGuiName("grid_snapping", TR("grid_snapping.title"));
    window_manager_.setImGuiName("rag_settings", TR("rag_settings.title"));

    window_coordinator_.updateWindowImguiName("conversations", TR("conversations.title"));
    window_coordinator_.updateWindowImguiName("files", TR("files.title"));
    window_coordinator_.updateWindowImguiName("grid_snapping", TR("grid_snapping.title"));
}

void MainWindow::showWindowByName(const std::string& window_name) {
    // Диалоговые окна управляются собственными флагами видимости —
    // при переключении через Window menu нужно открыть диалог явно.
    if (window_name == "settings") {
        if (settings_dialog_) settings_dialog_->show();
    } else if (window_name == "cloud_services") {
        if (cloud_services_dialog_) cloud_services_dialog_->open();
    } else if (window_name == "rag_settings") {
        if (rag_settings_dialog_) rag_settings_dialog_->set_visible(true);
    } else if (window_name == "settings_viewer") {
        if (settings_viewer_dialog_) settings_viewer_dialog_->show();
    } else if (window_name == "grid_snapping") {
        if (grid_snapping_dialog_) grid_snapping_dialog_->show();
    } else if (window_name == "profile_manager") {
        if (profile_manager_dialog_) profile_manager_dialog_->setOpen(true);
    } else if (window_name == "backup_manager") {
        if (backup_manager_dialog_) backup_manager_dialog_->setOpen(true);
    }
}

void MainWindow::hideWindowByName(const std::string& window_name) {
    if (window_name == "settings") {
        if (settings_dialog_) settings_dialog_->hide();
    } else if (window_name == "cloud_services") {
        if (cloud_services_dialog_) cloud_services_dialog_->close();
    } else if (window_name == "rag_settings") {
        if (rag_settings_dialog_) rag_settings_dialog_->set_visible(false);
    } else if (window_name == "settings_viewer") {
        if (settings_viewer_dialog_) settings_viewer_dialog_->hide();
    } else if (window_name == "profile_manager") {
        if (profile_manager_dialog_) profile_manager_dialog_->setOpen(false);
    } else if (window_name == "backup_manager") {
        if (backup_manager_dialog_) backup_manager_dialog_->setOpen(false);
    }
}

void MainWindow::show_settings_viewer() {
    // Show settings viewer dialog
    std::cout << "MainWindow: Opening Settings Viewer" << std::endl;
    show_settings_viewer_ = true;
}

void MainWindow::applyMenuToggleBindings() {
    // «Проверка SSL» — чекбокс отражает реальное состояние настройки.
    // check_func вызывается каждый кадр из updateMenuStates().
    if (AdvancedMenu* sec_menu = advanced_menu_system_.getMenuByKey("Security")) {
        for (auto& item : sec_menu->items) {
            if (item.command == "toggle_verify_ssl") {
                item.check_func = [this]() { return settings_.server().verify_ssl; };
            }
        }
    }
}

void MainWindow::registerCommand(const std::string& name, std::unique_ptr<Command> command) {
    if (!command_manager_) {
        std::cerr << "CommandManager not initialized" << std::endl;
        return;
    }
    command_manager_->registerCommand(name, std::move(command));
}

void MainWindow::set_title(const std::string& title) {
    title_ = title;
}

void MainWindow::set_size(int width, int height) {
    width_ = width;
    height_ = height;
}

void MainWindow::set_position(int x, int y) {
    // TODO: Implement window positioning
}

void MainWindow::on_conversation_changed(const llama_gui::core::StateEvent& event) {
}

void MainWindow::on_server_state_changed() {
}

void MainWindow::on_settings_changed() {
}

void MainWindow::on_state_changed(const std::string& key) {
}

void MainWindow::open_file(const std::string& file_path) {
    conversation_file_manager_->openFile();
}

void MainWindow::save_conversation(const std::string& conversation_id, const std::string& file_path) {
    conversation_file_manager_->saveCurrentConversationAs(file_path);
}

void MainWindow::export_conversations(const std::string& file_path) {
    conversation_file_manager_->exportConversations(file_path);
}

void MainWindow::open_conversation_file() {
    conversation_file_manager_->openConversationFile();
}

void MainWindow::save_current_conversation() {
    conversation_file_manager_->saveCurrentConversation();
}

void MainWindow::save_current_conversation_as(const std::string& file_path) {
    conversation_file_manager_->saveCurrentConversationAs(file_path);
}

void MainWindow::open_settings() {
    settings_dialog_->render();
}

void MainWindow::show_help(const std::string& help_type) {
    show_help_dialog();
}

void MainWindow::on_server_control_command(ServerControlCommand::Action action) {
    switch (action) {
        case ServerControlCommand::Action::Start:
            if (server_manager_) {
                server_manager_->set_model_path(settings_.get_model_path());
                server_manager_->start_server();
                show_info("Server", "Сервер запущен");
            }
            break;
        case ServerControlCommand::Action::Stop:
            if (server_manager_) {
                server_manager_->stop_server();
                show_info("Server", "Сервер остановлен");
            }
            break;
        case ServerControlCommand::Action::Restart:
            if (server_manager_) {
                server_manager_->set_model_path(settings_.get_model_path());
                server_manager_->restart_server();
                show_info("Server", "Сервер перезапущен");
            }
            break;
        default:
            break;
    }
}

void MainWindow::save_workspace(const std::string& name) {
    workspace_layout_manager_.save(name);
}

void MainWindow::load_workspace(const std::string& name) {
    workspace_layout_manager_.load(name);

    // Синхронизируем флаги видимости с WindowManager
    syncWindowFlagsFromManager();

    // Позиции применяются через WorkspaceApplier в render_ui()
    workspace_applier_.requestApply();
}

void MainWindow::delete_workspace(const std::string& name) {
    workspace_layout_manager_.remove(name);
}

std::vector<std::string> MainWindow::get_workspace_list() const {
    return workspace_layout_manager_.list();
}

void MainWindow::show_keyboard_shortcuts() {
    std::cout << "MainWindow: Showing keyboard shortcuts" << std::endl;
    show_keyboard_shortcuts_ = true;
}

void MainWindow::show_error(const std::string& title, const std::string& message) {
    dialog_manager_.showError(title, message);
}

void MainWindow::show_warning(const std::string& title, const std::string& message) {
    dialog_manager_.showWarning(title, message);
}

void MainWindow::show_info(const std::string& title, const std::string& message) {
    dialog_manager_.showInfo(title, message);
}

void MainWindow::show_profile_manager() {
    if (profile_manager_dialog_) {
        profile_manager_dialog_->setOpen(true);
        window_coordinator_.bringToFront("profile_manager");
    }
}

void MainWindow::save_current_profile() {
    std::string current = config_manager_.getCurrentProfileName();
    if (current.empty()) {
        if (profile_manager_dialog_) {
            profile_manager_dialog_->showCreateDialog();
        }
    } else {
        if (config_manager_.saveProfile(current)) {
            show_info("Профили", "Профиль сохранён: " + current);
        } else {
            show_info("Ошибка", "Ошибка сохранения профиля");
        }
    }
}

void MainWindow::load_profile(const std::string& profile_name) {
    if (config_manager_.loadProfile(profile_name)) {
        show_info("Профили", "Профиль загружен: " + profile_name);
        // Перезагружаем макет окон, чтобы размеры/позиции восстановились
        load_workspace("__last_session__");
    } else {
        show_info("Ошибка", "Ошибка загрузки профиля");
    }
}

void MainWindow::create_backup() {
    std::string backup = config_manager_.createBackup();
    if (!backup.empty()) {
        show_info("Резервная копия", "Создана резервная копия: " + backup);
    } else {
        show_info("Ошибка", "Ошибка создания резервной копии");
    }
}

void MainWindow::restore_from_backup(const std::string& backup_path) {
    if (config_manager_.restoreFromBackup(backup_path)) {
        show_info("Резервная копия", "Настройки восстановлены");
        // Перезагружаем макет окон после восстановления настроек
        load_workspace("__last_session__");
    } else {
        show_info("Ошибка", "Ошибка восстановления");
    }
}

void MainWindow::handle_model_file_selection(const std::string& model_path) {
    std::cout << "MainWindow: Model file selected: " << model_path << std::endl;
    load_model_from_path(model_path);
}

void MainWindow::show_model_reload_confirmation(const std::string& new_model_path) {
    dialog_manager_.showConfirmation(
        "Перезагрузка модели",
        "Загрузить новую модель: " + new_model_path + "?",
        [this, new_model_path](bool confirmed) {
            if (confirmed) {
                load_model_from_path(new_model_path);
            }
        }
    );
}

void MainWindow::load_model_and_restart_server(const std::string& model_path) {
    if (server_manager_) {
        // Update settings and server manager with new model path before restart
        settings_.set_model_path(model_path);
        server_manager_->set_model_path(model_path);
        server_manager_->restart_server();
        show_info("Server", "Модель загружена с перезапуском сервера");
    } else {
        show_warning("Server", "Сервер не инициализирован");
    }
}

void MainWindow::start_model_load_from_profile(const std::string& pending_query) {
    std::string model_path = settings_.get_model_path();
    if (model_path.empty()) {
        show_warning("Profile", "Путь к модели не указан в профиле");
        return;
    }
    
    show_model_load_dialog_ = true;
    model_load_progress_ = 0.0f;
    model_load_status_ = "Initializing...";
    is_model_loading_ = true;
    
    if (!pending_query.empty()) {
        model_load_pending_query_ = pending_query;
    }
}

void MainWindow::process_pending_dialog_results() {
    if (pending_dialog_result_) {
        switch (pending_dialog_result_->type) {
            case PendingDialogResult::DialogResultType::ModelLoad:
                if (pending_dialog_result_->confirmed) {
                    load_model_from_path(pending_dialog_result_->model_path);
                }
                break;
            case PendingDialogResult::DialogResultType::FileSave:
                if (pending_dialog_result_->confirmed && !pending_dialog_result_->file_path.empty()) {
                    conversation_file_manager_->saveCurrentConversationAs(
                        pending_dialog_result_->file_path
                    );
                }
                break;
            default:
                break;
        }
        pending_dialog_result_.reset();
    }
}

std::string MainWindow::get_config_directory() const {
    return ".";
}

std::string MainWindow::get_cache_directory() const {
    return ".";
}

bool MainWindow::ensure_directory_exists(const std::string& path) const {
    return true;
}

bool MainWindow::init_sdl2() {
#ifdef USE_SDL2
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << std::endl;
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool MainWindow::init_opengl() {
#ifdef USE_OPENGL
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    return true;
#else
    return false;
#endif
}

void MainWindow::setup_imgui_style() {
    if (settings_.is_dark_theme()) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
    }

    // Смягчаем геометрию: скруглённые углы окон и элементов
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
}

void MainWindow::create_ui_components() {
    std::cout << "MainWindow: Creating UI components" << std::endl;
}

void MainWindow::load_model_with_progress_dialog(const std::string& model_path, const std::string& pending_query, bool at_startup) {
    // Отдельный диалог загрузки показываем только при ручной загрузке,
    // при автозагрузке индикатор в окне чата достаточен
    if (!at_startup) {
        show_model_load_dialog_ = true;
    }
    model_load_progress_ = 0.0f;
    model_load_status_ = "Starting server...";
    is_model_loading_ = true;

    if (!pending_query.empty()) {
        pending_query_ = pending_query;
    }

    // Set model path for server and start the server
    if (server_manager_) {
        server_manager_->set_model_path(model_path);
        server_manager_->set_server_url(settings_.get_server_url());
        server_manager_->start_server();
        model_load_status_ = "Loading model...";
    }
}

void MainWindow::render_ui() {
    // Отрисовка визуальной сетки (если включена)
    ImVec2 display_size = ImGui::GetIO().DisplaySize;
    window_manager_.getGridSnappingSystem().renderGridOverlay(
        static_cast<int>(display_size.x),
        static_cast<int>(display_size.y));

    // Используем WindowCoordinator для рендеринга всех окон и диалогов
    window_coordinator_.renderAll();

    // Рендерим диалоги DialogManager
    dialog_manager_.render();

    // Рендерим FileDialogManager (встроенный пикер + доставка результатов)
    if (file_dialog_manager_) {
        file_dialog_manager_->render();
    }

    // Quick settings dialog (не управляется WindowManager)
    if (quick_settings_dialog_) {
        quick_settings_dialog_->render();
    }

    // Model dialogs (standalone, managed by their own flags)
    render_model_selection_dialog();
    render_model_load_dialog();

#ifdef ENABLE_LLAMA_BENCH
    // Llama Bench dialog (standalone)
    render_llama_bench_dialog();
#endif

    // Developer tools (Dear ImGui окна: Metrics, Style Editor, Font Selector, Debug Log)
    renderDeveloperTools();

    // Render main layout and status bar
    render_main_layout();
    if (window_manager_.isWindowVisible("status_bar")) {
        render_status_bar();
    }

    // Рендер окон/виджетов плагинов (внутри активного ImGui-контекста)
    if (plugin_manager_) {
        plugin_manager_->render_plugins();
    }
}

void MainWindow::initializePlugins() {
    if (!plugin_manager_) return;

    llama_gui::plugin::PluginSubsystems subsystems;
    subsystems.command_manager = command_manager_.get();
    subsystems.window_manager = &window_manager_;
    subsystems.window_coordinator = &window_coordinator_;
    subsystems.menu_system = &advanced_menu_system_;
    subsystems.dialog_manager = &dialog_manager_;
    subsystems.state_manager = &state_manager_;
    subsystems.settings = &settings_;
    subsystems.llama_interface = &llama_interface_;
    subsystems.chat_interface = chat_interface_.get();
    subsystems.rag_manager = rag_manager_.get();
    subsystems.config_dir = get_config_directory();
    subsystems.data_dir = get_cache_directory();
    subsystems.plugins_dir = "plugins";

    plugin_manager_->initialize(subsystems);
}

namespace {
// Каталог исполняемого файла (для поиска плагинов агентов рядом с бинарником).
std::string agent_executable_dir() {
#ifdef __linux__
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        std::string path(buf);
        const auto pos = path.find_last_of('/');
        if (pos != std::string::npos) return path.substr(0, pos);
    }
#endif
    return ".";
}
}  // namespace

void MainWindow::initialize_agent_system() {
    agent_registry_.set_context(&agent_context_);

    // Загрузка agent-плагинов (в т.ч. web_render_agent — headless-браузер/Chromium)
    // из каталогов, зеркальных поиску LLaMA-плагинов: cwd/plugins/agents и
    // <каталог exe>/plugins/agents.
    const std::vector<std::string> candidate_dirs = {
        "plugins/agents",
        agent_executable_dir() + "/plugins/agents",
    };

    int loaded = 0;
    for (const auto& dir : candidate_dirs) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) continue;
        loaded += agent_plugin_loader_.load_plugins_from_directory(dir, &agent_registry_);
    }

    // Инициализация всех загруженных агентов контекстом выполнения.
    agent_registry_.initialize_all(&agent_context_);

    // Интеграция с чатом: команды вида /agent <name> <action> [params].
    agent_chat_integration_ = std::make_unique<AgentChatIntegration>();
    if (agent_chat_integration_->initialize(&agent_registry_, &agent_context_)) {
        chat_interface_->set_agent_command_handler(
            [this](const std::string& command) {
                if (!agent_chat_integration_ || !agent_chat_integration_->is_available()) {
                    return false;
                }
                return agent_chat_integration_->handle_chat_command(
                    command,
                    [this](const ChatAgentResult& result) {
                        chat_interface_->add_assistant_message(
                            agent_chat_integration_->format_for_chat(result));
                    });
            });
    }

    std::cout << "[MainWindow] Agent system: loaded " << loaded
              << " agent plugin(s), registry has "
              << agent_registry_.list_agents().size() << " agent(s)" << std::endl;
}

void MainWindow::cleanup_opengl() {
#ifdef USE_OPENGL
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (ebo_) glDeleteBuffers(1, &ebo_);
#endif
}

void MainWindow::reload_fonts() {
    ImGuiIO& io = ImGui::GetIO();

    // Clear fonts before reloading
    io.Fonts->Clear();

    // Load new fonts (Clear() is already called, so just load and build)
    load_fonts_with_cyrillic();

    // Font texture will be rebuilt automatically on next NewFrame()
    // The fonts are already built in load_fonts_with_cyrillic()
}

void MainWindow::open_embedding_model_picker(const std::function<void(const std::string&)>& on_result) {
    file_dialog_manager_->pick_file("Выберите модель эмбеддинга", on_result, "", "embedding_model_files");
}

void MainWindow::toggle_fullscreen() {
#ifdef USE_SDL2
    if (!sdl_window_) return;

    is_fullscreen_ = !is_fullscreen_;

    if (is_fullscreen_) {
        // Save current window position/size before going fullscreen
        SDL_GetWindowPosition(sdl_window_, &pre_fullscreen_x_, &pre_fullscreen_y_);
        SDL_GetWindowSize(sdl_window_, &pre_fullscreen_w_, &pre_fullscreen_h_);
        SDL_SetWindowFullscreen(sdl_window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
        std::cout << "MainWindow: Entering fullscreen" << std::endl;
    } else {
        SDL_SetWindowFullscreen(sdl_window_, 0);
        SDL_SetWindowPosition(sdl_window_, pre_fullscreen_x_, pre_fullscreen_y_);
        SDL_SetWindowSize(sdl_window_, pre_fullscreen_w_, pre_fullscreen_h_);
        std::cout << "MainWindow: Exiting fullscreen" << std::endl;
    }
#endif
}

void MainWindow::renderDeveloperTools() {
    // Dear ImGui встроенные отладочные окна
    if (show_metrics_window_) {
        ImGui::ShowMetricsWindow(&show_metrics_window_);
    }

    if (show_debug_log_window_) {
        ImGui::ShowDebugLogWindow(&show_debug_log_window_);
    }

    if (show_style_editor_window_) {
        ImGui::Begin("Style Editor", &show_style_editor_window_);
        ImGui::ShowStyleEditor();
        ImGui::End();
    }

    if (show_font_selector_window_) {
        ImGui::Begin("Font Selector", &show_font_selector_window_);
        ImGui::ShowFontSelector("Font");
        ImGui::End();
    }
}

#ifdef ENABLE_LLAMA_BENCH
void MainWindow::openLlamaBenchDialog() {
    std::cout << "MainWindow: Opening Llama Bench dialog" << std::endl;
    if (llama_bench_dialog_) {
        llama_bench_dialog_->setVisible(true);
    }
}

void MainWindow::render_llama_bench_dialog() {
    if (llama_bench_dialog_) {
        llama_bench_dialog_->render();
    }
}
#endif

} // namespace ui
} // namespace llama_gui

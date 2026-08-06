#include "../include/ui/localization_manager.h"

namespace llama_gui {
namespace ui {

// =========================================================================
// Переводы для меню (Menus)
// =========================================================================

void LocalizationManager::initializeMenuTranslations() {
    // Main Menus
    addTranslation("menu.file", "Файл", "File");
    addTranslation("menu.edit", "Правка", "Edit");
    addTranslation("menu.model", "Модель", "Model");
    addTranslation("menu.view", "Вид", "View");
    addTranslation("menu.window", "Окно", "Window");
    addTranslation("menu.help", "Справка", "Help");
    addTranslation("menu.agents", "Агенты", "Agents");
    addTranslation("menu.tools", "Инструменты", "Tools");
    addTranslation("menu.debug", "Отладка", "Debug");
    addTranslation("menu.performance", "Производительность", "Performance");
    addTranslation("menu.security", "Безопасность", "Security");
    addTranslation("menu.logging", "Логирование", "Logging");

    // File Menu Items
    addTranslation("menu.file.new_chat", "Новый чат", "New Chat");
    addTranslation("menu.file.open", "Открыть", "Open");
    addTranslation("menu.file.save", "Сохранить", "Save");
    addTranslation("menu.file.save_as", "Сохранить как", "Save As");
    addTranslation("menu.file.exit", "Выход", "Exit");

    // Edit Menu Items
    addTranslation("menu.edit.settings", "Настройки", "Settings");
    addTranslation("menu.edit.preferences", "Параметры", "Preferences");
    addTranslation("menu.edit.keyboard_shortcuts", "Горячие клавиши", "Keyboard Shortcuts");

    // Model Menu Items
    addTranslation("menu.model.select_model", "Выбрать модель", "Select Model");
    addTranslation("menu.model.model_directory", "Папка моделей", "Model Directory");
    addTranslation("menu.model.server", "Сервер", "Server");
    addTranslation("menu.model.server_settings", "Настройки сервера", "Server Settings");
    addTranslation("menu.model.server.start", "Запустить сервер", "Start Server");
    addTranslation("menu.model.server.stop", "Остановить сервер", "Stop Server");
    addTranslation("menu.model.server.restart", "Перезапустить сервер", "Restart Server");
    addTranslation("menu.model.server.status", "Статус сервера", "Server Status");

    // View Menu Items
    addTranslation("menu.view.performance_overlay", "Информация о производительности", "Performance Overlay");
    addTranslation("menu.view.status_bar", "Строка состояния", "Status Bar");

    // Window Menu Items
    addTranslation("menu.window.conversations", "Беседы", "Conversations");
    addTranslation("menu.window.files", "Файлы", "Files");
    addTranslation("menu.window.chat", "Чат", "Chat");
    addTranslation("menu.window.rag", "RAG", "RAG");
    addTranslation("menu.window.agents", "Агенты", "Agents");
    addTranslation("menu.window.workspace", "Рабочая область", "Workspace");
    addTranslation("menu.window.workspace.save", "Сохранить рабочую область", "Save Workspace");
    addTranslation("menu.window.workspace.load", "Загрузить рабочую область", "Load Workspace");
    addTranslation("menu.window.workspace.reset", "Сбросить рабочую область", "Reset Workspace");
    addTranslation("menu.window.workspace.user", "Пользователь", "User");
    addTranslation("menu.window.workspace.developer", "Разработчик", "Developer");
    addTranslation("menu.window.workspace.admin", "Администратор", "Admin");

    // Help Menu Items
    addTranslation("menu.help.documentation", "Документация", "Documentation");
    addTranslation("menu.help.keyboard_shortcuts", "Горячие клавиши", "Keyboard Shortcuts");
    addTranslation("menu.help.about", "О программе", "About");
    addTranslation("menu.help.check_updates", "Проверить обновления", "Check for Updates");

    // Agents Menu Items
    addTranslation("menu.agents.panel", "Панель агентов", "Agents Panel");
    addTranslation("menu.agents.status", "Статус агентов", "Agent Status");
    addTranslation("menu.agents.list", "Список агентов", "List Agents");
    addTranslation("menu.agents.rag_search", "RAG поиск", "RAG Search");
    addTranslation("menu.agents.web_search", "Веб-поиск", "Web Search");
    addTranslation("menu.agents.code", "Генерация кода", "Generate Code");
    addTranslation("menu.agents.summarize", "Суммаризация", "Summarize");

    // Tools Menu Items (Developer)
    addTranslation("menu.tools.plugins", "Плагины", "Plugins");
    addTranslation("menu.tools.extensions", "Расширения", "Extensions");
    addTranslation("menu.tools.console", "Консоль", "Console");

    // Debug Menu Items (Developer)
    addTranslation("menu.debug.metrics", "Метрики/Отладка", "Metrics/Debugger");
    addTranslation("menu.debug.style_editor", "Редактор стилей", "Style Editor");
    addTranslation("menu.debug.font_selector", "Выбор шрифта", "Font Selector");
    addTranslation("menu.debug.debug_log", "Журнал отладки", "Debug Log");
    addTranslation("menu.debug.item_picker", "Выбор элементов", "Item Picker");
    addTranslation("menu.debug.debug_mode", "Режим отладки", "Debug Mode");
    addTranslation("menu.debug.show_group_rects", "Показать группы", "Show Group Rects");
    addTranslation("menu.debug.command_manager_state", "Состояние Command Manager", "Command Manager State");
    addTranslation("menu.debug.window_manager_state", "Состояние Window Manager", "Window Manager State");
    addTranslation("menu.debug.logger_info", "Информация Logger", "Logger Info");
    addTranslation("menu.debug.export_debug", "Экспорт отладки", "Export Debug Info");

    // Performance Menu Items (Admin)
    addTranslation("menu.performance.overlay", "Оверлей производительности", "Performance Overlay");
    addTranslation("menu.performance.settings", "Настройки производительности", "Performance Settings");
    addTranslation("menu.performance.vsync", "V-Sync", "V-Sync");
    addTranslation("menu.performance.fps_limit", "Ограничение FPS", "FPS Limit");
    addTranslation("menu.performance.smart_redraw", "Умная перерисовка", "Smart Redraw");

    // Security Menu Items (Admin)
    addTranslation("menu.security.ssl", "Настройки SSL/TLS", "SSL/TLS Settings");
    addTranslation("menu.security.auth_token", "Токен авторизации", "Auth Token");
    addTranslation("menu.security.verify_ssl", "Проверка SSL", "Verify SSL");
    addTranslation("menu.security.file_validation", "Проверка файлов", "File Validation");
    addTranslation("menu.security.audit_log", "Журнал аудита", "Audit Log");

    // Logging Menu Items (Admin)
    addTranslation("menu.logging.settings", "Настройки логирования", "Logging Settings");
    addTranslation("menu.logging.view_logs", "Просмотр логов", "View Logs");
    addTranslation("menu.logging.log_level", "Уровень логов", "Log Level");
    addTranslation("menu.logging.log_to_file", "Лог в файл", "Log to File");
    addTranslation("menu.logging.flush_logs", "Сброс логов", "Flush Logs");
    addTranslation("menu.logging.export_logs", "Экспорт логов", "Export Logs");

    // Settings Menu Items - Profiles and Backups
    addTranslation("menu.settings.profiles", "Профили", "Profiles");
    addTranslation("menu.settings.profiles.manage", "Управление профилями", "Manage Profiles");
    addTranslation("menu.settings.profiles.save_current", "Сохранить текущий профиль", "Save Current Profile");
    addTranslation("menu.settings.backups", "Резервные копии", "Backups");
}

} // namespace ui
} // namespace llama_gui

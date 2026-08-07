#include "../include/ui/localization_manager.h"

namespace llama_gui {
namespace ui {

// =========================================================================
// Переводы для диалогов настроек (Settings Dialog)
// =========================================================================

void LocalizationManager::initializeSettingsTranslations() {
    // Settings Dialog
    addTranslation("settings.title", "Настройки", "Settings");
    addTranslation("settings.quick", "Быстрые настройки", "Quick Settings");
    addTranslation("settings.advanced", "Расширенные настройки", "Advanced Settings");
    addTranslation("settings.mode", "Режим настроек", "Settings Mode");
    addTranslation("settings.frequently_used", "Часто используемые", "Frequently used settings");
    addTranslation("settings.detailed", "Детальные настройки", "Detailed configuration by category");

    // Settings tabs - Quick
    addTranslation("settings.tab.server", "Сервер", "Server");
    addTranslation("settings.tab.chat", "Чат", "Chat");
    addTranslation("settings.tab.models", "Модели", "Models");
    addTranslation("settings.tab.ui", "Интерфейс", "UI");

    // Settings categories - Advanced
    addTranslation("settings.category.gpu_hardware", "GPU и оборудование", "GPU & Hardware");
    addTranslation("settings.category.sampling_generation", "Сэмплирование и генерация", "Sampling & Generation");
    addTranslation("settings.category.model_server", "Модель и сервер", "Model & Server");
    addTranslation("settings.category.system", "Система", "System");

    // Settings tabs - Advanced
    addTranslation("settings.tab.gpu", "GPU", "GPU");
    addTranslation("settings.tab.cache", "Кэш", "Cache");
    addTranslation("settings.tab.sampling", "Сэмплирование", "Sampling");
    addTranslation("settings.tab.sampling_advanced", "Доп. сэмплирование", "Sampling Advanced");
    addTranslation("settings.tab.context", "Контекст", "Context");
    addTranslation("settings.tab.rope", "RoPE", "RoPE");
    addTranslation("settings.tab.model_loading", "Загрузка модели", "Model Loading");
    addTranslation("settings.tab.batch", "Пакетная обработка", "Batch");
    addTranslation("settings.tab.server_runtime", "Выполнение сервера", "Server Runtime");
    addTranslation("settings.tab.grammar", "Грамматика", "Grammar");
    addTranslation("settings.tab.control_vectors", "Векторы управления", "Control Vectors");
    addTranslation("settings.tab.logging", "Логирование", "Logging");
    addTranslation("settings.tab.performance", "Производительность", "Performance");
    addTranslation("settings.tab.advanced", "Дополнительно", "Advanced");
    addTranslation("settings.tab.output", "Вывод", "Output");
    addTranslation("settings.tab.tensor_override", "Переопределение тензоров", "Tensor Override");

    // Settings labels
    addTranslation("settings.label.host", "Хост", "Host");
    addTranslation("settings.label.port", "Порт", "Port");
    addTranslation("settings.label.api_url", "API URL", "API URL");
    addTranslation("settings.label.server_control", "Управление сервером", "Server Control");
    addTranslation("settings.label.server_status", "Статус сервера", "Server Status");
    addTranslation("settings.label.system_prompt", "Системный промпт", "System Prompt");
    addTranslation("settings.label.max_tokens", "Макс. токенов", "Max Tokens");
    addTranslation("settings.label.temperature", "Температура", "Temperature");
    addTranslation("settings.label.top_p", "Top P", "Top P");
    addTranslation("settings.label.top_k", "Top K", "Top K");
    addTranslation("settings.label.repeat_penalty", "Штраф за повтор", "Repeat Penalty");
    addTranslation("settings.label.cpu_threads", "Потоки CPU", "CPU Threads");
    addTranslation("settings.label.context_size", "Размер контекста", "Context Size");
    addTranslation("settings.label.gpu_layers", "Слои GPU", "GPU Layers");
    addTranslation("settings.label.model_path", "Путь к модели", "Model Path");
    addTranslation("settings.label.model_directory", "Папка моделей", "Model Directory");
    addTranslation("settings.label.embedding_model", "Модель эмбеддингов", "Embedding Model");
    addTranslation("settings.label.theme", "Тема", "Theme");
    addTranslation("settings.label.font_size", "Размер шрифта", "Font Size");
    addTranslation("settings.label.window_size", "Размер окна", "Window Size");
    addTranslation("settings.label.vsync", "V-Sync", "V-Sync");
    addTranslation("settings.label.fps_limit", "Ограничение FPS", "FPS Limit");

    // Settings buttons
    addTranslation("settings.button.start_server", "Запустить сервер", "Start Server");
    addTranslation("settings.button.stop_server", "Остановить сервер", "Stop Server");
    addTranslation("settings.button.restart_server", "Перезапустить", "Restart");
    addTranslation("settings.button.browse", "Обзор...", "Browse...");
    addTranslation("settings.button.set_directory", "Указать папку", "Set Directory");

    // Language
    addTranslation("settings.language", "Язык", "Language");
    addTranslation("settings.language.interface", "Язык интерфейса", "Interface Language");

    // UI settings
    addTranslation("settings.ui.theme", "Тема", "Theme");
    addTranslation("settings.ui.theme.dark", "Тёмная", "Dark");
    addTranslation("settings.ui.theme.light", "Светлая", "Light");
    addTranslation("settings.ui.theme.auto", "Авто", "Auto");
    addTranslation("settings.ui.theme.appearance", "Тема оформления", "Appearance Theme");
    addTranslation("settings.ui.font", "Шрифт", "Font");
    addTranslation("settings.ui.window", "Окно", "Window");
    addTranslation("settings.ui.window.width", "Ширина", "Width");
    addTranslation("settings.ui.window.height", "Высота", "Height");
    addTranslation("settings.ui.window.maximized", "Развернуто", "Maximized");
    addTranslation("settings.ui.window.auto_resize", "Автоматический размер", "Auto Resize");
    addTranslation("settings.ui.window.auto_resize.help", "Автоматически подстраивать размер окна под разрешение монитора", "Automatically adjust the window size to the monitor resolution");
    addTranslation("settings.ui.window.center", "Центрировать окно", "Center Window");
    addTranslation("settings.ui.window.min_width", "Мин. ширина", "Min Width");
    addTranslation("settings.ui.window.min_height", "Мин. высота", "Min Height");
    addTranslation("settings.ui.performance", "Производительность интерфейса", "Interface Performance");
    addTranslation("settings.ui.animations", "Анимации", "Animations");
    addTranslation("settings.ui.vsync.help", "Вертикальная синхронизация — убирает разрывы кадров", "Vertical sync — eliminates screen tearing");
    addTranslation("settings.ui.idle_fps", "FPS в простое", "Idle FPS");
    addTranslation("settings.ui.idle_fps.help", "Частота кадров, когда окно не активно", "Frame rate when the window is not active");
    addTranslation("settings.ui.idle_timeout", "Таймаут простоя (мс)", "Idle Timeout (ms)");
    addTranslation("settings.ui.idle_timeout.help", "Время бездействия до переключения в режим пониженного FPS", "Idle time before switching to reduced FPS mode");
    addTranslation("settings.ui.smart_redraw", "Умная перерисовка", "Smart Redraw");
    addTranslation("settings.ui.smart_redraw.help", "Перерисовывать только при изменениях (экономит CPU)", "Redraw only on changes (saves CPU)");
    addTranslation("settings.ui.performance_overlay", "Показать оверлей производительности", "Show Performance Overlay");

    // File settings
    addTranslation("settings.files.default_paths", "Пути по умолчанию", "Default Paths");
    addTranslation("settings.files.save_path", "Путь сохранения", "Save Path");
    addTranslation("settings.files.auto_save_path", "Путь автосохранения", "Auto-save Path");
    addTranslation("settings.files.auto_save", "Автосохранение", "Auto-save");
    addTranslation("settings.files.auto_save_enabled", "Включить автосохранение", "Enable Auto-save");
    addTranslation("settings.files.auto_save_interval", "Интервал (сек)", "Interval (sec)");

    // Security settings
    addTranslation("settings.security.ssl", "SSL / TLS", "SSL / TLS");
    addTranslation("settings.security.verify_ssl", "Проверять SSL-сертификаты", "Verify SSL Certificates");
    addTranslation("settings.security.verify_ssl.help", "Отключите только если используете самоподписанные сертификаты", "Disable only if you use self-signed certificates");
    addTranslation("settings.security.token", "Токен доступа", "Access Token");
    addTranslation("settings.security.token.help", "Токен для аутентификации на сервере (если включена)", "Token for server authentication (if enabled)");

    // Profiles
    addTranslation("settings.profiles.title", "Профили настроек", "Settings Profiles");
    addTranslation("settings.profiles.select", "Выберите профиль", "Select profile");
    addTranslation("settings.profiles.load", "Загрузить", "Load");
    addTranslation("settings.profiles.save", "Сохранить", "Save");
    addTranslation("settings.profiles.save_as", "Сохранить как", "Save As");
    addTranslation("settings.profiles.delete", "Удалить", "Delete");
    addTranslation("settings.profiles.new_name", "Имя нового профиля", "New profile name");

    // Settings viewer (INI viewer dialog)
    addTranslation("settings.viewer.title", "Просмотр настроек INI", "Settings INI Viewer");
    addTranslation("settings.viewer.filter", "Фильтр:", "Filter:");
    addTranslation("settings.viewer.search", "Поиск", "Search");
    addTranslation("settings.viewer.clear", "Сброс", "Clear");
    addTranslation("settings.viewer.modified_only", "Только изменённые", "Modified only");
    addTranslation("settings.viewer.showing", "Показано: %zu / %zu (Изменено: %zu)", "Showing: %zu / %zu (Modified: %zu)");
    addTranslation("settings.viewer.all_sections", "Все секции", "All sections");
    addTranslation("settings.viewer.no_match", "Нет настроек, соответствующих фильтрам", "No settings match the current filters");
    addTranslation("settings.viewer.col.section", "Секция", "Section");
    addTranslation("settings.viewer.col.key", "Ключ", "Key");
    addTranslation("settings.viewer.col.value", "Значение", "Value");
    addTranslation("settings.viewer.col.status", "Статус", "Status");
    addTranslation("settings.viewer.edit_tooltip", "Двойной клик для редактирования", "Double-click to edit");
    addTranslation("settings.viewer.empty", "[пусто]", "[empty]");
    addTranslation("settings.viewer.modified", "Изменено", "Modified");
    addTranslation("settings.viewer.actions", "Действия:", "Actions:");
    addTranslation("settings.viewer.save", "Сохранить", "Save");
    addTranslation("settings.viewer.revert", "Отменить", "Revert");
    addTranslation("settings.viewer.reload", "Откатить", "Reload");
    addTranslation("settings.viewer.apply", "Применить", "Apply");
    addTranslation("settings.viewer.close", "Закрыть", "Close");
    addTranslation("settings.viewer.tooltip.save", "Сохранить изменения в INI файл", "Save changes to INI file");
    addTranslation("settings.viewer.tooltip.revert", "Отменить все несохранённые изменения", "Discard all unsaved changes");
    addTranslation("settings.viewer.tooltip.reload", "Откатить к последнему сохранённому состоянию (перезагрузить из файла)", "Reload settings from INI file (discard unsaved changes)");
    addTranslation("settings.viewer.tooltip.apply", "Применить текущие значения INI к настройкам приложения", "Apply current INI values to application settings");
    addTranslation("settings.viewer.value", "Значение:", "Value:");
    addTranslation("settings.viewer.cut", "Вырезать", "Cut");
    addTranslation("settings.viewer.copy", "Копировать", "Copy");
    addTranslation("settings.viewer.paste", "Вставить", "Paste");
    addTranslation("settings.viewer.ok", "OK", "OK");
    addTranslation("settings.viewer.cancel", "Отмена", "Cancel");
    addTranslation("settings.viewer.status.reverted", "Все изменения отменены", "All changes reverted");
    addTranslation("settings.viewer.status.load_failed", "Не удалось загрузить INI файл: ", "Failed to load INI file: ");
    addTranslation("settings.viewer.status.loaded", "Загружено: ", "Loaded: ");
    addTranslation("settings.viewer.status.save_failed", "Не удалось сохранить INI файл: ", "Failed to save INI file: ");
    addTranslation("settings.viewer.status.saved", "Сохранено: ", "Saved: ");
    addTranslation("settings.viewer.status.applied", "Настройки применены", "Settings applied");
    addTranslation("settings.viewer.status.applied_sync", "Настройки применены и синхронизированы", "Settings applied and synchronized");
}

} // namespace ui
} // namespace llama_gui

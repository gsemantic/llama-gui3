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

    // Profiles
    addTranslation("settings.profiles.title", "Профили настроек", "Settings Profiles");
    addTranslation("settings.profiles.select", "Выберите профиль", "Select profile");
    addTranslation("settings.profiles.load", "Загрузить", "Load");
    addTranslation("settings.profiles.save", "Сохранить", "Save");
    addTranslation("settings.profiles.save_as", "Сохранить как", "Save As");
    addTranslation("settings.profiles.delete", "Удалить", "Delete");
    addTranslation("settings.profiles.new_name", "Имя нового профиля", "New profile name");
}

} // namespace ui
} // namespace llama_gui

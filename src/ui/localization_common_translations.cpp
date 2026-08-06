#include "../include/ui/localization_manager.h"

namespace llama_gui {
namespace ui {

// =========================================================================
// Переводы для чата и общих элементов (Chat & Common)
// =========================================================================

void LocalizationManager::initializeCommonTranslations() {
    // Chat interface
    addTranslation("chat.title", "Беседа: %s", "Conversation: %s");
    addTranslation("chat.no_active_conversation", "Нет активной беседы. Начните новый чат, чтобы начать.", "No active conversation. Start a new chat to begin.");
    addTranslation("chat.send", "Отправить", "Send");
    addTranslation("chat.attach", "Прикрепить", "Attach");
    addTranslation("chat.select_model", "Выбрать модель", "Select Model");
    addTranslation("chat.clear", "Очистить", "Clear");
    addTranslation("chat.auto_scroll", "Автопрокрутка", "Auto-scroll");
    addTranslation("chat.generating", "Генерируется...", "Generating...");
    addTranslation("chat.processing_rag", "📄 Чтение документа...", "📄 Reading document...");
    addTranslation("chat.stop", "Остановить", "Stop");
    addTranslation("chat.copy_message", "Копировать всё сообщение", "Copy Entire Message");

    // Context menu items
    addTranslation("context.cut", "Вырезать", "Cut");
    addTranslation("context.copy", "Копировать", "Copy");
    addTranslation("context.paste", "Вставить", "Paste");
    addTranslation("context.clear", "Очистить", "Clear");

    // Messages
    addTranslation("message.user", "Пользователь", "User");
    addTranslation("message.assistant", "Ассистент", "Assistant");
    addTranslation("message.system", "Система", "System");
    addTranslation("message.processing", "Обрабатываю запрос...", "Processing request...");
    addTranslation("message.generating", "Ассистент (генерирует):", "Assistant (generating):");

    // Buttons
    addTranslation("button.yes", "Да", "Yes");
    addTranslation("button.no", "Нет", "No");
    addTranslation("button.cancel", "Отмена", "Cancel");
    addTranslation("button.ok", "ОК", "OK");
    addTranslation("button.save", "Сохранить", "Save");
    addTranslation("button.load", "Загрузить", "Load");
    addTranslation("button.delete", "Удалить", "Delete");
    addTranslation("button.edit", "Редактировать", "Edit");
    addTranslation("button.add", "Добавить", "Add");
    addTranslation("button.remove", "Удалить", "Remove");
    addTranslation("button.apply", "Применить", "Apply");
    addTranslation("button.reset", "Сброс", "Reset");

    // Status messages
    addTranslation("status.model_loaded", "Модель загружена: %s", "Model loaded: %s");
    addTranslation("status.model_load_failed", "Не удалось загрузить модель: %s", "Failed to load model: %s");
    addTranslation("status.server_started", "Сервер запущен", "Server started");
    addTranslation("status.server_stopped", "Сервер остановлен", "Server stopped");
    addTranslation("status.server_restarted", "Сервер перезапущен", "Server restarted");
    addTranslation("status.server_starting", "Сервер запускается...", "Server is starting...");
    addTranslation("status.server_stopping", "Сервер останавливается...", "Server is stopping...");
    addTranslation("status.server_restarting", "Сервер перезапускается...", "Server is restarting...");
    addTranslation("status.server_ready", "Сервер готов к работе", "Server is ready and running");
    addTranslation("status.server_not_ready", "Сервер не запущен", "Server is not running");
    addTranslation("status.server_status", "Статус сервера", "Server Status");
    addTranslation("status.details", "Детали:", "Details:");
    addTranslation("status.workspace_saved", "Рабочая область сохранена: %s", "Workspace saved: %s");
    addTranslation("status.workspace_loaded", "Рабочая область загружена: %s", "Workspace loaded: %s");
    addTranslation("status.workspace_reset", "Рабочая область сброшена", "Workspace reset");

    // Error messages
    addTranslation("error.model_load_failed", "Ошибка загрузки модели", "Model Load Error");
    addTranslation("error.server_restart_failed", "Не удалось перезапустить сервер", "Server Restart Failed");
    addTranslation("error.no_response", "Ошибка: Нет ответа от модели", "Error: No response from model");
    addTranslation("error.streaming_failed", "Ошибка: Сбой потоковой передачи - %s", "Error: Streaming failed - %s");
    addTranslation("error.failed_to_start_streaming", "Ошибка: Не удалось запустить потоковую операцию", "Error: Failed to start streaming operation");

    // Info messages
    addTranslation("info.model_selection", "Выбор модели", "Model Selection");
    addTranslation("info.opening_model_dialog", "Открытие диалога выбора модели...", "Opening model file selection dialog...");
    addTranslation("info.model_directory", "Папка моделей", "Model Directory");
    addTranslation("info.opening_directory_dialog", "Открытие диалога выбора папки...", "Opening directory selection dialog...");
    addTranslation("info.model_directory_set", "Папка моделей успешно установлена: %s", "Model directory successfully set to: %s");
    addTranslation("info.model_loaded_success", "Модель успешно загружена и сервер перезапущен: %s", "Successfully loaded model and restarted server: %s");
    addTranslation("info.conversation_exported", "Беседа экспортирована в: %s", "Conversation exported to: %s");
    addTranslation("info.file_saved", "Файл сохранён в: %s", "File saved to: %s");
    addTranslation("info.theme_changed", "Тема изменена", "Theme Changed");
    addTranslation("info.theme_applied", "Применена тёмная тема", "Dark theme applied");
    addTranslation("info.theme_applied_light", "Применена светлая тема", "Light theme applied");
    addTranslation("info.theme_applied_classic", "Применена классическая тема", "Classic theme applied");

    // Warnings
    addTranslation("warning.model_selection_cancelled", "Выбор модели отменён", "Model selection cancelled");
    addTranslation("warning.message_empty", "Предупреждение: Содержимое сообщения пусто после очистки", "WARNING: Message content is empty after cleaning");

    // Dialog titles
    addTranslation("dialog.confirm_model_reload", "Подтвердить перезагрузку модели", "Confirm Model Reload");
    addTranslation("dialog.different_model_loaded", "Загружена другая модель.", "A different model is currently loaded.");
    addTranslation("dialog.reload_with_new_model", "Хотите перезагрузить сервер с новой моделью?", "Do you want to reload the server with the new model?");
    addTranslation("dialog.current_model", "Текущая модель: %s", "Current model: %s");
    addTranslation("dialog.new_model", "Новая модель: %s", "New model: %s");
    addTranslation("dialog.yes_reload_server", "Да, перезагрузить сервер", "Yes, Reload Server");

    // Performance metrics
    addTranslation("performance.completed", "✓ Завершено: %d токенов | %.1f ток/с | %dс", "✓ Completed: %d tokens | %.1f tok/s | %ds");
    addTranslation("performance.generating", "Генерация: ~%d токенов | %.1f ток/с | %dс", "Generating: ~%d tokens | %.1f tok/s | %ds");
    addTranslation("performance.context_remaining", "Осталось контекста: %d / %d", "Context: %d / %d");
    addTranslation("performance.context_remaining_inline", " | Осталось контекста: %d / %d", " | Context: %d / %d");

    // Generation statistics (detailed view after completion)
    addTranslation("performance.generation_stats", "Статистика генерации:", "Generation Statistics:");
    addTranslation("performance.time", "Время", "Time");
    addTranslation("performance.seconds", "с", "s");
    addTranslation("performance.tokens", "Токены", "Tokens");
    addTranslation("performance.speed", "Скорость", "Speed");
    addTranslation("performance.tokens_per_second", "ток/с", "tok/s");

    // General
    addTranslation("general.loading", "Загрузка...", "Loading...");
    addTranslation("general.ready", "Готово", "Ready");
    addTranslation("general.error", "Ошибка", "Error");
    addTranslation("general.warning", "Предупреждение", "Warning");
    addTranslation("general.success", "Успешно", "Success");
    addTranslation("general.confirm", "Подтвердить", "Confirm");
    addTranslation("general.close", "Закрыть", "Close");

    // Workspace
    addTranslation("workspace.title", "Рабочая область", "Workspace");
    addTranslation("workspace.current", "Текущая:", "Current:");
    addTranslation("workspace.user", "Пользователь", "User");
    addTranslation("workspace.developer", "Разработчик", "Developer");
    addTranslation("workspace.admin", "Администратор", "Admin");
    addTranslation("workspace.switch", "Переключить", "Switch");
    
    // Active Mode (режимы доступа)
    addTranslation("mode.active", "Активное состояние", "Active Mode");
    addTranslation("mode.user", "Пользователь", "User");
    addTranslation("mode.developer", "Разработчик", "Developer");
    addTranslation("mode.admin", "Администратор", "Admin");
    addTranslation("mode.user_desc", "Базовый режим с ограниченным доступом к функциям", "Basic mode with limited access to features");
    addTranslation("mode.developer_desc", "Режим разработчика с инструментами отладки", "Developer mode with debugging tools");
    addTranslation("mode.admin_desc", "Режим администратора с полным доступом", "Admin mode with full access");

    // Language selector
    addTranslation("language.selector.label", "Язык:", "Language:");
    addTranslation("language.selector.icon", "🌐", "🌐");
    addTranslation("language.russian", "Русский", "Russian");
    addTranslation("language.english", "English", "English");
}

} // namespace ui
} // namespace llama_gui

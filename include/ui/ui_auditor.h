#pragma once

// =========================================================================
// UiAuditor — статический аудит консистентности UI без запуска рендера.
//
// Проверяет согласованность трёх реестров:
//   1. AdvancedMenuSystem (пункты меню и их ссылки на команды)
//   2. CommandManager     (зарегистрированные команды, заглушки, хоткеи)
//   3. WindowManager      (окна для window-toggle пунктов меню)
//
// Находит: битые ссылки на команды, команды-заглушки, дубликаты пунктов,
// "мёртвые" пункты без действия, команды-сироты вне меню, битые хоткеи.
//
// Запуск в приложении: флаг --audit-ui (см. main.cpp / MainWindow::runUiAudit).
// =========================================================================

#include <string>
#include <vector>

namespace llama_gui {
namespace ui {

class CommandManager;
class AdvancedMenuSystem;
class WindowManager;
struct AdvancedMenuItem;

struct UiAuditFinding {
    enum class Severity {
        Error,    // сломанная функциональность: битая ссылка, пункт без действия
        Warning,  // работает частично/неправильно: заглушка, дубль, неизвестное окно
        Info      // потенциальная проблема: команда-сирота, расхождение хоткеев
    };

    Severity severity;
    std::string category;   // "menu", "command", "shortcut", "window"
    std::string location;   // путь в меню: "Settings > Quick Settings > Server"
    std::string message;

    const char* severity_tag() const;
};

struct UiAuditReport {
    std::vector<UiAuditFinding> findings;

    // Статистика обхода
    size_t total_menus = 0;
    size_t total_items = 0;         // включая подменю, без разделителей
    size_t total_commands = 0;      // зарегистрировано в CommandManager
    size_t total_shortcuts = 0;     // зарегистрировано хоткеев
    size_t total_windows = 0;       // зарегистрировано окон в WindowManager
    size_t menu_stub_items = 0;     // пунктов меню, ссылающихся на заглушки
    size_t orphan_commands = 0;     // команд вне меню и хоткеев

    size_t errors() const;
    size_t warnings() const;
    size_t infos() const;

    // Формированный текстовый отчёт (для stdout / файла)
    std::string toText(const std::string& title = "UI AUDIT") const;
};

class UiAuditor {
public:
    // Все указатели опциональны: отсутствующий реестр просто не проверяется
    UiAuditor(const CommandManager* command_manager,
              const AdvancedMenuSystem* menu_system,
              const WindowManager* window_manager);

    UiAuditReport run() const;

private:
    // Рекурсивный обход пунктов меню (включая подменю)
    void walkItems(const std::string& parent_path,
                   const std::vector<AdvancedMenuItem>& items,
                   UiAuditReport& report,
                   std::vector<std::pair<std::string, std::string>>& command_usage) const;

    const CommandManager* command_manager_;
    const AdvancedMenuSystem* menu_system_;
    const WindowManager* window_manager_;
};

} // namespace ui
} // namespace llama_gui

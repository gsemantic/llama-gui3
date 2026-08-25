#include "ui_auditor.h"
#include "advanced_menu_system.h"
#include "window_manager.h"
#include "../../include/ui/command_manager.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace llama_gui {
namespace ui {

namespace {

const char* severity_tag(UiAuditFinding::Severity s) {
    switch (s) {
        case UiAuditFinding::Severity::Error:   return "ERROR  ";
        case UiAuditFinding::Severity::Warning: return "WARN   ";
        case UiAuditFinding::Severity::Info:    return "INFO   ";
    }
    return "???    ";
}

} // namespace

// =========================================================================
// UiAuditFinding / UiAuditReport
// =========================================================================

const char* UiAuditFinding::severity_tag() const {
    // Свободная функция выше недоступна как член — дублируем логику здесь
    switch (severity) {
        case Severity::Error:   return "ERROR  ";
        case Severity::Warning: return "WARN   ";
        case Severity::Info:    return "INFO   ";
    }
    return "???    ";
}

size_t UiAuditReport::errors() const {
    return static_cast<size_t>(std::count_if(findings.begin(), findings.end(),
        [](const UiAuditFinding& f) { return f.severity == UiAuditFinding::Severity::Error; }));
}

size_t UiAuditReport::warnings() const {
    return static_cast<size_t>(std::count_if(findings.begin(), findings.end(),
        [](const UiAuditFinding& f) { return f.severity == UiAuditFinding::Severity::Warning; }));
}

size_t UiAuditReport::infos() const {
    return static_cast<size_t>(std::count_if(findings.begin(), findings.end(),
        [](const UiAuditFinding& f) { return f.severity == UiAuditFinding::Severity::Info; }));
}

std::string UiAuditReport::toText(const std::string& title) const {
    std::ostringstream out;

    out << "\n======================================================\n";
    out << "  " << title << "\n";
    out << "======================================================\n";

    // Сводка
    out << "Объекты: меню=" << total_menus
        << " пунктов=" << total_items
        << " команд=" << total_commands
        << " хоткеев=" << total_shortcuts
        << " окон=" << total_windows << "\n";
    out << "Итоги: ошибок=" << errors()
        << " предупреждений=" << warnings()
        << " инфо=" << infos() << "\n";
    out << "Заглушек в меню=" << menu_stub_items
        << " команд-сирот=" << orphan_commands << "\n";

    if (findings.empty()) {
        out << "Проблем не обнаружено.\n";
    }

    // Группировка по категориям для читаемости
    static const std::vector<std::string> order = { "menu", "command", "shortcut", "window" };
    for (const auto& category : order) {
        bool header_printed = false;
        for (const auto& f : findings) {
            if (f.category != category) continue;
            if (!header_printed) {
                out << "\n--- [" << category << "] ---\n";
                header_printed = true;
            }
            out << severity_tag(f.severity) << " " << f.message;
            if (!f.location.empty()) {
                out << "  @ " << f.location;
            }
            out << "\n";
        }
    }

    // Находки вне известных категорий (на будущее)
    for (const auto& f : findings) {
        bool known = std::find(order.begin(), order.end(), f.category) != order.end();
        if (!known) {
            out << severity_tag(f.severity) << " [" << f.category << "] "
                << f.message << "  @ " << f.location << "\n";
        }
    }

    out << "\n======================================================\n";
    return out.str();
}

// =========================================================================
// UiAuditor
// =========================================================================

UiAuditor::UiAuditor(const CommandManager* command_manager,
                     const AdvancedMenuSystem* menu_system,
                     const WindowManager* window_manager)
    : command_manager_(command_manager)
    , menu_system_(menu_system)
    , window_manager_(window_manager) {}

void UiAuditor::walkItems(
        const std::string& parent_path,
        const std::vector<AdvancedMenuItem>& items,
        UiAuditReport& report,
        std::vector<std::pair<std::string, std::string>>& command_usage) const {

    std::set<std::string> names_in_scope;

    for (const auto& item : items) {
        if (item.type == AdvancedMenuItemType::Separator) continue;
        report.total_items++;

        const std::string path =
            parent_path.empty() ? item.name : parent_path + " > " + item.name;

        // Дубликат имени внутри одного родительского контейнера
        if (!names_in_scope.insert(item.name).second) {
            report.findings.push_back({UiAuditFinding::Severity::Warning, "menu",
                path, "Дублирующийся пункт меню \"" + item.name + "\""});
        }

        if (item.type == AdvancedMenuItemType::Submenu) {
            walkItems(path, item.submenu_items, report, command_usage);
            continue;
        }

        // Пункт с действием-переключателем окна
        if (item.is_window_toggle) {
            if (item.window_name.empty()) {
                report.findings.push_back({UiAuditFinding::Severity::Error, "window",
                    path, "Переключатель окна без имени окна"});
            } else if (window_manager_) {
                const auto windows = window_manager_->getAllWindowNames();
                if (std::find(windows.begin(), windows.end(), item.window_name) == windows.end()) {
                    report.findings.push_back({UiAuditFinding::Severity::Warning, "window",
                        path, "Окно \"" + item.window_name + "\" не зарегистрировано в WindowManager"});
                }
            }
            continue;
        }

        // Обычный пункт: должна быть команда ИЛИ callback ИЛИ toggle_func
        if (item.command.empty() && !item.callback && !item.toggle_func) {
            report.findings.push_back({UiAuditFinding::Severity::Error, "menu",
                path, "Пункт меню без действия (нет команды и callback)"});
            continue;
        }

        if (!item.command.empty()) {
            command_usage.emplace_back(item.command, path);

            if (command_manager_ && !command_manager_->isCommandRegistered(item.command)) {
                report.findings.push_back({UiAuditFinding::Severity::Error, "menu",
                    path, "Битая ссылка: команда \"" + item.command + "\" не зарегистрирована"});
            } else if (command_manager_ && command_manager_->isCommandStub(item.command)) {
                report.menu_stub_items++;
                report.findings.push_back({UiAuditFinding::Severity::Warning, "menu",
                    path, "Пункт ссылается на заглушку \"" + item.command + "\""});
            }
        }

        // Расхождение хоткея пункта меню с глобальным реестром
        if (!item.shortcut.empty() && command_manager_) {
            const std::string registered_cmd =
                command_manager_->getCommandForShortcut(item.shortcut);
            if (!registered_cmd.empty() && registered_cmd != item.command) {
                report.findings.push_back({UiAuditFinding::Severity::Warning, "shortcut",
                    path, "Хоткей \"" + item.shortcut + "\" в реестре назначен \""
                          + registered_cmd + "\", а пункт ссылается на \"" + item.command + "\""});
            }
        }
    }
}

UiAuditReport UiAuditor::run() const {
    UiAuditReport report;

    // --- Реестр команд ---
    std::set<std::string> used_commands;
    std::vector<std::pair<std::string, std::string>> command_usage;

    if (command_manager_) {
        const auto names = command_manager_->getAllCommandNames();
        report.total_commands = names.size();
        for (const auto& n : names) used_commands.insert(n);
    }

    // --- Меню ---
    if (menu_system_) {
        const auto menu_keys = menu_system_->getAllMenuNames();
        report.total_menus = menu_keys.size();

        for (const auto& key : menu_keys) {
            // Плагины добавляют меню по имени без menu_key — пробуем оба поиска
            const AdvancedMenu* menu = menu_system_->getMenuByKey(key);
            if (!menu) menu = menu_system_->getMenu(key);
            if (!menu) {
                report.findings.push_back({UiAuditFinding::Severity::Error, "menu",
                    key, "Меню заявлено в списке, но не найдено (" + key + ")"});
                continue;
            }
            walkItems(menu->menu_key.empty() ? menu->name : menu->menu_key,
                      menu->items, report, command_usage);
        }
    }

    for (const auto& [cmd, loc] : command_usage) {
        (void)loc;
        used_commands.erase(cmd);
    }

    // --- Хоткеи ---
    if (command_manager_) {
        const auto shortcuts = command_manager_->getAvailableShortcuts();
        report.total_shortcuts = shortcuts.size();
        std::map<std::string, int> shortcut_owner_count;
        for (const auto& sc : shortcuts) {
            const std::string cmd = command_manager_->getCommandForShortcut(sc);
            shortcut_owner_count[sc]++;
            if (!cmd.empty()) used_commands.erase(cmd);
            if (!cmd.empty() && !command_manager_->isCommandRegistered(cmd)) {
                report.findings.push_back({UiAuditFinding::Severity::Error, "shortcut",
                    "", "Хоткей \"" + sc + "\" указывает на незарегистрированную команду \"" + cmd + "\""});
            }
        }
        for (const auto& [sc, count] : shortcut_owner_count) {
            if (count > 1) {
                report.findings.push_back({UiAuditFinding::Severity::Warning, "shortcut",
                    "", "Хоткей \"" + sc + "\" зарегистрирован " + std::to_string(count) + " раз"});
            }
        }
    }

    // --- Окна ---
    if (window_manager_) {
        report.total_windows = window_manager_->getAllWindowNames().size();
    }

    // --- Команды-сироты ---
    report.orphan_commands = used_commands.size();
    for (const auto& cmd : used_commands) {
        report.findings.push_back({UiAuditFinding::Severity::Info, "command",
            "", "Команда \"" + cmd + "\" не доступна ни из меню, ни по хоткею"});
    }

    return report;
}

} // namespace ui
} // namespace llama_gui

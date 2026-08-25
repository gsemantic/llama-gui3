#include "main_window.h"
#include "ui_auditor.h"
#include "advanced_menu_system.h"
#include "window_manager.h"
#include "../../include/ui/command_manager.h"

#include <iostream>

namespace llama_gui {
namespace ui {

int MainWindow::runUiAudit() {
    UiAuditor auditor(command_manager_.get(), &advanced_menu_system_, &window_manager_);
    UiAuditReport report = auditor.run();

    std::cout << report.toText("UI AUDIT (--audit-ui)") << std::endl;

    // Подсказка по дальнейшим действиям
    if (report.errors() > 0) {
        std::cout << "Найдено ошибок: " << report.errors()
                  << ". Исправьте битые ссылки и мёртвые пункты меню." << std::endl;
    } else {
        std::cout << "Критических ошибок не найдено." << std::endl;
    }

    return static_cast<int>(report.errors());
}

} // namespace ui
} // namespace llama_gui

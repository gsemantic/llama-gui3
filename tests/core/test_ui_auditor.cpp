// =========================================================================
// Тесты UiAuditor: детекция битых ссылок, заглушек, дублей, сирот
// на синтетическом реестре команд/меню (без ImGui-рендера и SDL).
// Регрессионная защита: новые классы проблем в меню не должны проходить.
// =========================================================================

#include "../test_framework.h"
#include "ui/ui_auditor.h"
#include "ui/advanced_menu_system.h"
#include "ui/command_manager.h"

#include <memory>
#include <set>

using namespace llama_gui::ui;

namespace {

// Собирает меню из пар (имя пункта, команда); пустая команда = пункт без действия
std::unique_ptr<AdvancedMenu> makeMenu(
        const std::string& key,
        const std::vector<std::pair<std::string, std::string>>& items) {
    auto menu = std::make_unique<AdvancedMenu>();
    menu->menu_key = key;
    menu->name = key;
    for (const auto& [name, cmd] : items) {
        AdvancedMenuItem item;
        item.name = name;
        item.command = cmd;
        item.type = AdvancedMenuItemType::Item;
        menu->items.push_back(item);
    }
    return menu;
}

} // namespace

void test_audit_clean_registry() {
    CommandManager cm;
    AdvancedMenuSystem ms;
    ms.initialize(&cm, nullptr, nullptr);

    cm.registerCommand("cmd_a", CommandFactory::createFunctionalCommand(
        "cmd_a", []() {}, "test", "", nullptr));
    ms.addMenu(makeMenu("File", {{"Do A", "cmd_a"}}));

    UiAuditReport report = UiAuditor(&cm, &ms, nullptr).run();
    TEST_ASSERT_EQUAL(report.errors(), 0);
    TEST_ASSERT_EQUAL(report.warnings(), 0);
    TEST_ASSERT_EQUAL(report.total_menus, 1);
    TEST_ASSERT_EQUAL(report.total_items, 1);
}

void test_audit_dead_command_reference() {
    CommandManager cm;
    AdvancedMenuSystem ms;
    ms.initialize(&cm, nullptr, nullptr);

    // Пункт ссылается на незарегистрированную команду
    ms.addMenu(makeMenu("File", {{"Ghost", "no_such_cmd"}}));

    UiAuditReport report = UiAuditor(&cm, &ms, nullptr).run();
    TEST_ASSERT(report.errors() >= 1);
    bool found = false;
    for (const auto& f : report.findings) {
        if (f.message.find("no_such_cmd") != std::string::npos &&
            f.severity == UiAuditFinding::Severity::Error) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
}

void test_audit_stub_reference_is_warning() {
    CommandManager cm;
    AdvancedMenuSystem ms;
    ms.initialize(&cm, nullptr, nullptr);

    cm.registerCommand("stub_cmd", CommandFactory::createFunctionalCommand(
        "stub_cmd", []() {}, "stub", "", nullptr));
    cm.markCommandAsStub("stub_cmd");
    ms.addMenu(makeMenu("Tools", {{"Stub Item", "stub_cmd"}}));

    UiAuditReport report = UiAuditor(&cm, &ms, nullptr).run();
    TEST_ASSERT_EQUAL(report.errors(), 0);
    TEST_ASSERT_EQUAL(report.warnings(), 1);
    TEST_ASSERT_EQUAL(report.menu_stub_items, 1);
}

void test_audit_duplicate_item_names() {
    CommandManager cm;
    AdvancedMenuSystem ms;
    ms.initialize(&cm, nullptr, nullptr);

    cm.registerCommand("x1", CommandFactory::createFunctionalCommand(
        "x1", []() {}, "", "", nullptr));
    cm.registerCommand("x2", CommandFactory::createFunctionalCommand(
        "x2", []() {}, "", "", nullptr));
    // Два пункта с одинаковым именем в одном меню
    ms.addMenu(makeMenu("Agents", {{"About", "x1"}, {"About", "x2"}}));

    UiAuditReport report = UiAuditor(&cm, &ms, nullptr).run();
    bool dup_found = false;
    for (const auto& f : report.findings) {
        if (f.message.find("Дублирующийся пункт") != std::string::npos &&
            f.location.find("About") != std::string::npos) {
            dup_found = true;
        }
    }
    TEST_ASSERT_TRUE(dup_found);
}

void test_audit_dead_menu_item() {
    CommandManager cm;
    AdvancedMenuSystem ms;
    ms.initialize(&cm, nullptr, nullptr);

    // Пункт без команды и без callback — мёртвый
    ms.addMenu(makeMenu("Misc", {{"Dead Item", ""}}));

    UiAuditReport report = UiAuditor(&cm, &ms, nullptr).run();
    TEST_ASSERT(report.errors() >= 1);
    bool found = false;
    for (const auto& f : report.findings) {
        if (f.message.find("без действия") != std::string::npos) found = true;
    }
    TEST_ASSERT_TRUE(found);
}

void test_audit_orphan_commands() {
    CommandManager cm;
    AdvancedMenuSystem ms;
    ms.initialize(&cm, nullptr, nullptr);

    cm.registerCommand("used_cmd", CommandFactory::createFunctionalCommand(
        "used_cmd", []() {}, "", "", nullptr));
    cm.registerCommand("orphan_cmd", CommandFactory::createFunctionalCommand(
        "orphan_cmd", []() {}, "", "", nullptr));
    ms.addMenu(makeMenu("File", {{"Used", "used_cmd"}}));

    UiAuditReport report = UiAuditor(&cm, &ms, nullptr).run();
    TEST_ASSERT_EQUAL(report.orphan_commands, 1);
    bool found = false;
    for (const auto& f : report.findings) {
        if (f.message.find("orphan_cmd") != std::string::npos &&
            f.severity == UiAuditFinding::Severity::Info) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
}

void test_audit_shortcut_mismatch() {
    CommandManager cm;
    AdvancedMenuSystem ms;
    ms.initialize(&cm, nullptr, nullptr);

    cm.registerCommand("menu_cmd", CommandFactory::createFunctionalCommand(
        "menu_cmd", []() {}, "", "", nullptr));
    cm.registerCommand("real_cmd", CommandFactory::createFunctionalCommand(
        "real_cmd", []() {}, "", "", nullptr));
    // Реестр: хоткей принадлежит real_cmd; пункт меню претендует на него же,
    // но ссылается на menu_cmd — расхождение должно быть обнаружено.
    cm.registerShortcut("Ctrl+Shift+1", "real_cmd");

    auto menu = makeMenu("File", {{"Item", "menu_cmd"}});
    menu->items[0].shortcut = "Ctrl+Shift+1";
    ms.addMenu(std::move(menu));

    UiAuditReport report = UiAuditor(&cm, &ms, nullptr).run();
    bool found = false;
    for (const auto& f : report.findings) {
        if (f.category == "shortcut" &&
            f.message.find("в реестре назначен") != std::string::npos) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
}

void test_audit_report_text_format() {
    CommandManager cm;
    AdvancedMenuSystem ms;
    ms.initialize(&cm, nullptr, nullptr);
    ms.addMenu(makeMenu("File", {{"Ghost", "missing"}}));

    UiAuditReport report = UiAuditor(&cm, &ms, nullptr).run();
    std::string text = report.toText("UNIT TEST");
    TEST_ASSERT(text.find("UNIT TEST") != std::string::npos);
    TEST_ASSERT(text.find("ERROR") != std::string::npos);
    TEST_ASSERT(text.find("missing") != std::string::npos);
}

// Регистрация в раннере
static const bool _reg_clean = []() {
    test::TestRunner::instance().addTest("UiAuditor.clean_registry", test_audit_clean_registry);
    test::TestRunner::instance().addTest("UiAuditor.dead_reference", test_audit_dead_command_reference);
    test::TestRunner::instance().addTest("UiAuditor.stub_warning", test_audit_stub_reference_is_warning);
    test::TestRunner::instance().addTest("UiAuditor.duplicate_names", test_audit_duplicate_item_names);
    test::TestRunner::instance().addTest("UiAuditor.dead_item", test_audit_dead_menu_item);
    test::TestRunner::instance().addTest("UiAuditor.orphans", test_audit_orphan_commands);
    test::TestRunner::instance().addTest("UiAuditor.shortcut_mismatch", test_audit_shortcut_mismatch);
    test::TestRunner::instance().addTest("UiAuditor.report_text", test_audit_report_text_format);
    return true;
}();

int main() {
    return test::TestRunner::instance().run();
}

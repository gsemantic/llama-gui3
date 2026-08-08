// test_menu_restore.cpp — регрессионный тест восстановления пунктов меню плагинов.
//
// Плагины (hello_plugin и news_rewriter) добавляют пункты в существующее меню
// "Agents". Приложение перестраивает меню (rebuildModernMenu при смене языка),
// пункты плагинов при этом пропадают — PluginManager::render_plugins() должен
// идемпотентно восстанавливать их. Проверяем:
//   - после init в меню "Agents" есть пункты приложения + пункты плагинов;
//   - после "перестройки" меню пункты плагинов исчезают;
//   - после render_plugins() они возвращаются;
//   - повторные вызовы render_plugins() (кадры) НЕ растят меню (нет дубликатов),
//     в т.ч. когда в одно меню добавляют несколько плагинов;
//   - в меню нет двух пунктов с одинаковыми (имя, команда).

#include "plugins/plugin_manager.h"
#include "ui/advanced_menu_system.h"
#include "ui/command_manager.h"
#include "ui/window_manager.h"
#include "ui/workspace_manager.h"

#include <cstdio>
#include <set>
#include <string>
#include <utility>

#ifndef PLUGIN_TEST_DIR
#define PLUGIN_TEST_DIR "plugins"
#endif

using namespace llama_gui;

static int failures = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

// Собираем меню "Agents" так же, как createAgentsMenu() в приложении.
static void build_agents_menu(ui::AdvancedMenuSystem& menu_system) {
    auto agents = std::make_unique<ui::AdvancedMenu>();
    agents->name = "Agents";
    agents->menu_key = "Agents";
    agents->items.push_back(ui::AdvancedMenuItemFactory::createCommandItem(
        "Agents Panel", "toggle_window_agents"));
    agents->items.push_back(ui::AdvancedMenuItemFactory::createCommandItem(
        "Agent Status", "agents_status"));
    agents->items.push_back(ui::AdvancedMenuItemFactory::createCommandItem(
        "List Agents", "agents_list"));
    agents->items.push_back(ui::AdvancedMenuItemFactory::createSeparator());
    agents->items.push_back(ui::AdvancedMenuItemFactory::createCommandItem(
        "RAG Search", "rag"));
    agents->items.push_back(ui::AdvancedMenuItemFactory::createCommandItem(
        "Web Search", "search"));
    agents->items.push_back(ui::AdvancedMenuItemFactory::createCommandItem(
        "Generate Code", "code"));
    agents->items.push_back(ui::AdvancedMenuItemFactory::createCommandItem(
        "Summarize", "summarize"));
    menu_system.addMenu(std::move(agents));
}

static int count_items(const ui::AdvancedMenu* menu) {
    return menu ? static_cast<int>(menu->items.size()) : -1;
}

int main() {
    ui::CommandManager command_manager;
    ui::WindowManager window_manager;
    ui::WorkspaceManager workspace_manager;
    ui::AdvancedMenuSystem menu_system;
    menu_system.initialize(&command_manager, &window_manager, &workspace_manager);

    // Меню приложения (только "Agents" — достаточно для проверки восстановления).
    build_agents_menu(menu_system);
    const int app_items = count_items(menu_system.getMenuByKey("Agents"));

    // Плагины с реальными подсистемами меню/команд/окон.
    plugin::PluginSubsystems subsystems;
    subsystems.command_manager = &command_manager;
    subsystems.window_manager = &window_manager;
    subsystems.menu_system = &menu_system;
    subsystems.config_dir = ".";
    subsystems.data_dir = ".";
    subsystems.plugins_dir = PLUGIN_TEST_DIR;

    plugin::PluginManager manager;
    if (!manager.initialize(subsystems)) {
        std::fprintf(stderr, "FAIL: PluginManager::initialize() вернул false\n");
        return 1;
    }

    // После init: пункты приложения + пункты hello_plugin + news_rewriter.
    const ui::AdvancedMenu* agents = menu_system.getMenuByKey("Agents");
    check(agents != nullptr, "меню 'Agents' должно существовать");
    const int after_init = count_items(agents);
    check(after_init > app_items, "после init в меню 'Agents' должны быть пункты плагинов");

    // Симуляция перестройки меню приложением: пункты плагинов пропадают.
    menu_system.removeMenu("Agents");
    build_agents_menu(menu_system);
    check(count_items(menu_system.getMenuByKey("Agents")) == app_items,
          "после перестройки меню пункты плагинов должны исчезнуть");

    // Первый кадр: render_plugins() восстанавливает пункты плагинов.
    manager.render_plugins();
    const int restored = count_items(menu_system.getMenuByKey("Agents"));
    check(restored == after_init,
          "после render_plugins() пункты плагинов должны восстановиться в полном объёме");

    // Последующие кадры: меню не должно расти (нет дубликатов).
    bool stable = true;
    for (int i = 0; i < 10; ++i) {
        manager.render_plugins();
        if (count_items(menu_system.getMenuByKey("Agents")) != restored) stable = false;
    }
    check(stable, "повторные render_plugins() не должны менять размер меню (нет дубликатов)");

    // Никаких двух пунктов с одинаковыми (имя, команда) — иначе ImGui ID collision.
    bool no_dup_pairs = true;
    {
        const ui::AdvancedMenu* m = menu_system.getMenuByKey("Agents");
        std::set<std::pair<std::string, std::string>> seen;
        for (const auto& item : m->items) {
            if (item.type == ui::AdvancedMenuItemType::Separator) continue;
            auto key = std::make_pair(item.name, item.command);
            if (!seen.insert(key).second) no_dup_pairs = false;
        }
    }
    check(no_dup_pairs, "в меню нет дубликатов пунктов (имя, команда)");

    manager.shutdown();

    if (failures == 0) {
        std::printf("MENU RESTORE TEST OK (app_items=%d, total=%d)\n", app_items, restored);
        return 0;
    }
    std::fprintf(stderr, "MENU RESTORE TEST FAILED: %d ошибок\n", failures);
    return 1;
}

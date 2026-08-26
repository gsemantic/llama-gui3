// test_menu_layout.cpp — тесты MenuLayoutManager: модель, JSON round-trip,
// группы и порядок пунктов пользовательской раскладки меню.

#include "ui/menu_layout_manager.h"

#include <cassert>
#include <cstdio>
#include <iostream>

using namespace llama_gui::ui;

int main() {
    const std::string path = "/tmp/opencode/llama_gui_test_menu_layout.json";
    std::remove(path.c_str());

    {
        MenuLayoutManager m;
        m.setConfigPath(path);
        assert(!m.load()); // Файла нет — не ошибка (первый запуск)
        m.setActiveWorkspace("User");

        // Видимость
        m.setItemHidden("File", "cmd.open", true);
        assert(m.isItemHidden("File", "cmd.open"));
        assert(!m.isItemHidden("File", "cmd.save"));
        m.setMenuHidden("Developer", false);

        // Группы и порядок
        std::string gid = m.createGroup("File", "My tools");
        assert(m.moveItemToGroup("File", gid, "cmd.save"));
        assert(m.moveItemToGroup("File", gid, "toggle_window_chat"));
        assert(!m.moveItemToGroup("File", "group_999", "cmd.x")); // Нет группы

        MenuLayoutEntry e1;
        e1.is_group = false;
        e1.key = "cmd.quit";
        MenuLayoutEntry e2;
        e2.is_group = true;
        e2.key = gid;
        m.setEntries("File", {e1, e2});
        assert(m.save());

        // Перенос в группу другого меню невозможен (id уникальны глобально)
        std::string gid2 = m.createGroup("View", "Second");
        assert(m.moveItemToGroup("File", gid2, "cmd.save") == false);
    }

    // Round-trip
    MenuLayoutManager m2;
    m2.setConfigPath(path);
    assert(m2.load());
    m2.setActiveWorkspace("User");

    assert(m2.isItemHidden("File", "cmd.open"));
    assert(!m2.isItemHidden("File", "cmd.save"));
    assert(!m2.isMenuHidden("Developer"));

    const MenuLayoutForMenu* layout = m2.findMenuLayout("File");
    assert(layout != nullptr);
    assert(layout->entries.size() == 2);
    assert(!layout->entries[0].is_group && layout->entries[0].key == "cmd.quit");
    assert(layout->entries[1].is_group);
    assert(layout->groups.size() == 1);
    assert(layout->groups[0].title == "My tools");
    assert(layout->groups[0].item_keys.size() == 2);

    // Извлечение из группы возвращает пункт на верхний уровень
    assert(m2.removeItemFromGroup("File", layout->groups[0].id, "cmd.save"));
    assert(m2.findMenuLayout("File")->groups[0].item_keys.size() == 1);
    assert(m2.findMenuLayout("File")->entries.size() == 3);

    // Изоляция workspace
    m2.setActiveWorkspace("Developer");
    assert(!m2.isItemHidden("File", "cmd.open"));
    assert(m2.findMenuLayout("File") == nullptr);
    m2.setActiveWorkspace("User");
    assert(m2.isItemHidden("File", "cmd.open"));

    // Экспорт / импорт
    std::string exported = m2.serializeToJson();
    MenuLayoutManager m3;
    assert(m3.deserializeFromJson(exported));
    m3.setActiveWorkspace("User");
    assert(m3.isItemHidden("File", "cmd.open"));
    assert(m3.findMenuLayout("File") != nullptr);
    assert(m3.deserializeFromJson("{not json") == false); // Битый JSON

    // Сброс
    m2.resetMenu("File");
    assert(m2.findMenuLayout("File") == nullptr);
    m2.setActiveWorkspace("Developer");
    m2.resetAll();
    m2.setActiveWorkspace("User");
    assert(m2.findMenuLayout("View") == nullptr);

    std::remove(path.c_str());
    std::cout << "MENU LAYOUT TEST OK" << std::endl;
    return 0;
}

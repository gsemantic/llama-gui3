#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace llama_gui {
namespace ui {

class AdvancedMenuSystem;

/**
 * @brief Диалог настройки раскладки главного меню
 *
 * Позволяет пользователю скрывать меню и их пункты, менять порядок,
 * собирать пункты в собственные подменю (группы). Изменения применяются
 * к главному меню сразу (живой предпросмотр) и сохраняются автоматически.
 *
 * Редактор работает с верхним уровнем каждого меню; модель авторитетна
 * на время сеанса правки и коммитится в MenuLayoutManager при каждом
 * изменении.
 */
class MenuLayoutEditorDialog {
public:
    void setMenuSystem(AdvancedMenuSystem* menu_system) { menu_system_ = menu_system; }

    /**
     * @brief Отрисовать диалог
     * @param open Флаг открытия окна (ImGui-конвенция)
     */
    void render(bool* open);

    void show() { show_ = true; }
    void hide() { show_ = false; }
    bool isVisible() const { return show_; }

private:
    // Узел верхнего уровня модели: пункт меню или позиция группы
    struct EditorNode {
        bool is_group = false;
        std::string key;      // Ключ пункта (для Item)
        int group_index = -1; // Индекс в groups_ (для Group)
    };

    // Группа (кастомное подменю) в модели редактора
    struct EditorGroup {
        std::string id;
        std::string title;
        std::vector<std::string> item_keys;
    };

    void selectMenu(const std::string& menu_key);
    void rebuildModel();
    void commit();

    AdvancedMenuSystem* menu_system_ = nullptr;
    bool show_ = false;

    std::string current_menu_key_;
    std::vector<EditorNode> nodes_;      // Отображаемый порядок верхнего уровня
    std::vector<EditorGroup> groups_;    // Группы текущего меню
    std::unordered_map<std::string, std::string> item_names_; // Ключ → имя пункта
    std::unordered_set<std::string> existing_keys_;

    int target_group_ = -1;              // Целевая группа для кнопки "в группу"
    char new_group_title_[128] = "";     // Поле ввода заголовка новой группы
};

} // namespace ui
} // namespace llama_gui

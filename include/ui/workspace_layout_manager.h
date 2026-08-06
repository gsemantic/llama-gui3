#pragma once

#include <string>
#include <vector>

namespace llama_gui {
namespace ui {

class WindowManager;

/**
 * @brief Менеджер профилей рабочих пространств (расположение окон).
 *
 * Отвечает за сохранение/загрузку/удаление профилей — наборов позиций,
 * размеров и видимости окон. Каждый профиль — JSON-файл в .config/llama-gui/workspaces/.
 */
class WorkspaceLayoutManager {
public:
    void setWindowManager(WindowManager* wm) { window_manager_ = wm; }

    /** Сохранить текущее расположение окон в профиль. */
    bool save(const std::string& name);

    /** Загрузить профиль — применить расположение окон. */
    bool load(const std::string& name);

    /** Удалить профиль. */
    bool remove(const std::string& name);

    /** Переименовать профиль. */
    bool rename(const std::string& old_name, const std::string& new_name);

    /** Список всех профилей на диске. */
    std::vector<std::string> list() const;

    /** Существует ли профиль с таким именем. */
    bool exists(const std::string& name) const;

    /** Имя текущего активного профиля. */
    std::string currentName() const { return current_name_; }

private:
    WindowManager* window_manager_ = nullptr;
    std::string current_name_;
    std::string workspaces_dir_ = ".config/llama-gui/workspaces";
};

} // namespace ui
} // namespace llama_gui

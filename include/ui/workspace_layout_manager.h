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

    /**
     * @brief Применить отложенный ini-blob ImGui (позиции/collapsed/колонки).
     *
     * Профиль загружается ДО создания ImGui-контекста, поэтому ini-blob из JSON
     * применяется позже — вызывайте один раз сразу после CreateContext(),
     * до первого NewFrame(). Если blob'а нет — ничего не делает.
     */
    void applyPendingImguiIni();

    /**
     * @brief Восстановить z-order окон (какое окно поверх какого).
     *
     * Вызывается КАЖДЫЙ кадр из run(); применяется один раз после 2-го кадра
     * (окна должны быть созданы первыми Begin()). Дёшево: два сравнения.
     */
    void tickDeferredApply();

private:
    WindowManager* window_manager_ = nullptr;
    std::string current_name_;
    std::string workspaces_dir_ = ".config/llama-gui/workspaces";
    std::string pending_imgui_ini_;   // ini-blob ImGui, ожидающий создания контекста
    std::vector<std::string> pending_z_order_;  // порядок окон (задний → передний)
};

} // namespace ui
} // namespace llama_gui

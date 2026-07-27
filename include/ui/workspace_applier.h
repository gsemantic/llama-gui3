#pragma once

#include <string>
#include "imgui.h"

namespace llama_gui {
namespace ui {

class WindowManager;

/**
 * @brief Хранит позиции/размеры окон из workspace для одноразового применения.
 *
 * Координатор вызывает getPositions() после Begin() для каждого окна
 * и напрямую корректирует w->Pos / w->Size. Флаг сбрасывается
 * после успешного применения ко всем окнам.
 */
class WorkspaceApplier {
public:
    void setWindowManager(WindowManager* manager) { window_manager_ = manager; }

    /** Mark that workspace positions should be applied. */
    void requestApply();

    /** Are there pending positions to apply? */
    bool hasPendingPositions() const { return pending_; }

    /** Get saved position for a window by name. Returns false if not found. */
    bool getPosition(const std::string& name, ImVec2& out_pos, ImVec2& out_size) const;

    /** Call after all windows have been processed. Resets the flag. */
    void confirmApplied() { pending_ = false; }

private:
    WindowManager* window_manager_ = nullptr;
    bool pending_ = false;
};

} // namespace ui
} // namespace llama_gui

#include "workspace_applier.h"
#include "window_manager.h"

namespace llama_gui {
namespace ui {

void WorkspaceApplier::requestApply() {
    pending_ = true;
}

bool WorkspaceApplier::getPosition(const std::string& name, ImVec2& out_pos, ImVec2& out_size) const {
    if (!window_manager_ || !pending_) return false;

    WindowState state = window_manager_->getWindowState(name);
    if (state.size.x <= 0 || state.size.y <= 0) return false;

    out_pos = state.position;
    out_size = state.size;
    return true;
}

} // namespace ui
} // namespace llama_gui

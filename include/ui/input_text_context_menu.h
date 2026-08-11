#pragma once

#include "imgui.h"

namespace llama_gui {
namespace ui {

// Right-click context menu (Cut / Copy / Paste / Select All) for the input field
// submitted immediately before this call.
//
// Call right after any ImGui::InputText()/InputTextMultiline() so that
// ImGui::GetItemID() returns the field. The chosen action is executed by
// re-activating the field and emulating the corresponding Ctrl+<key> chord via
// ImGui's input queue, so the field's own selection/cursor/undo/filter and
// read-only handling apply. No access to the field's buffer is needed.
void InputTextContextMenu();

}  // namespace ui
}  // namespace llama_gui

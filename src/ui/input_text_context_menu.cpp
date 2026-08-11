#include "../include/ui/input_text_context_menu.h"

#include "../external/imgui/imgui_internal.h"

#include "../include/ui/localization_manager.h"

namespace llama_gui {
namespace ui {

namespace {

// An action picked from the context menu is executed in two steps:
//   1. NavNextActivateId re-activates the field next frame. ImGuiActivateFlags
//      TryToPreserveState makes InputText keep its cursor/selection/undo stack.
//   2. One frame later the field is active and not "just activated", so its
//      built-in shortcut handling performs the Ctrl+<key> chord that we queue.
struct PendingAction {
    ImGuiID field_id = 0;
    ImGuiKey key = ImGuiKey_None;
    bool keys_queued = false;  // step 2 done, releases still pending
};

PendingAction g_pending;

void ScheduleAction(ImGuiID field_id, ImGuiKey key) {
    ImGuiContext& g = *GImGui;
    g.NavNextActivateId = field_id;
    g.NavNextActivateFlags = ImGuiActivateFlags_TryToPreserveState;
    g_pending = PendingAction{field_id, key, false};
}

}  // namespace

void InputTextContextMenu() {
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = g.IO;

    // Step 2 finished: the emulated chord fired this frame while the field was
    // active, so queue the key releases (they are processed at next NewFrame).
    if (g_pending.field_id != 0 && g_pending.keys_queued) {
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddKeyEvent(g_pending.key, false);
        g_pending = PendingAction{};
    }

    ImGuiID field_id = ImGui::GetItemID();

    // Step 1: the field was (re-)activated via NavActivateId on the previous
    // frame and is now active. Queue the Ctrl+<key> press so InputText's
    // built-in shortcut processing runs it on the next frame.
    if (g_pending.field_id == field_id && !g_pending.keys_queued) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(g_pending.key, true);
        g_pending.keys_queued = true;
    }

    // Passing a null str_id uses the field's own ID as the popup ID, so fields
    // in the same window don't collide.
    if (ImGui::BeginPopupContextItem(nullptr)) {
        bool has_selection = false;
        if (ImGuiInputTextState* state = ImGui::GetInputTextState(field_id))
            has_selection = state->HasSelection();

        if (ImGui::MenuItem(TRF("context.cut", "Cut"), "Ctrl+X", false, has_selection)) {
            ScheduleAction(field_id, ImGuiKey_X);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem(TRF("context.copy", "Copy"), "Ctrl+C", false, has_selection)) {
            ScheduleAction(field_id, ImGuiKey_C);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem(TRF("context.paste", "Paste"), "Ctrl+V")) {
            ScheduleAction(field_id, ImGuiKey_V);
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(TRF("context.select_all", "Select All"), "Ctrl+A")) {
            ScheduleAction(field_id, ImGuiKey_A);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace ui
}  // namespace llama_gui

#include "../include/ui/input_text_context_menu.h"

#include "../external/imgui/imgui_internal.h"

#include "../include/ui/localization_manager.h"

namespace llama_gui {
namespace ui {

namespace {

// An action picked from the context menu is executed in two steps:
//   1. NavNextActivateId re-activates the field next frame. ImGuiActivateFlags
//      TryToPreserveState makes InputText keep its cursor/selection/undo stack.
//   2. The field is active and its built-in shortcut handling performs the
//      Ctrl+<key> chord that we push into ImGui's input queue.
//
// ImGui trickles queued input events (ConfigInputTrickleEventQueue, on by
// default): a leftover event (e.g. the mouse-up of the menu click still sitting
// in the queue) can push the delivery of our synthetic down to a later frame.
// The release must therefore wait until the press really landed in
// io.KeysData[], otherwise releasing early would cancel the press before
// InputText ever sees it.
struct PendingAction {
    ImGuiID field_id = 0;
    ImGuiKey key = ImGuiKey_None;
    float press_time = 0.0f;     // g.Time when the press was queued
    bool keys_queued = false;    // Ctrl+<key> press has been queued
};

PendingAction g_pending;

void ScheduleAction(ImGuiID field_id, ImGuiKey key) {
    ImGuiContext& g = *GImGui;
    g.NavNextActivateId = field_id;
    g.NavNextActivateFlags = ImGuiActivateFlags_TryToPreserveState;
    g_pending = PendingAction{field_id, key, 0.0f, false};
}

}  // namespace

void InputTextContextMenu() {
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = g.IO;

    // Step 2 finished? The emulated chord fired while the field was active.
    // Queue the key releases only once the chord was actually consumed: the
    // key is down AND InputText's shortcut route for Ctrl+<key> was granted
    // this frame (i.e. the paste/cut/... performed). The route is attributed
    // by UpdateKeyRoutingTable() one frame after InputText submits it, so the
    // press landing [just-activated frame + 1 frame] is not enough — wait for
    // the grant.
    if (g_pending.field_id != 0 && g_pending.keys_queued) {
        const bool key_down = io.KeysData[g_pending.key - ImGuiKey_NamedKey_BEGIN].Down;
        const bool route_granted = ImGui::TestShortcutRouting(ImGuiMod_Ctrl | g_pending.key, g_pending.field_id);
        const float held_time = g.Time - g_pending.press_time;
        if (key_down && route_granted) {
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            io.AddKeyEvent(g_pending.key, false);
            g_pending = PendingAction{};
        } else if (held_time > 0.5f) {
            // Safety net: never keep a synthetic key held indefinitely.
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            io.AddKeyEvent(g_pending.key, false);
            g_pending = PendingAction{};
        }
        // else: hold. The press or the routing grant hasn't landed yet.
    }

    ImGuiID field_id = ImGui::GetItemID();
    if (field_id == 0)
        return;

    // Step 1: the field was (re-)activated via NavActivateId on the previous
    // frame and is now active. Queue the Ctrl+<key> press, but NOT on the
    // "just activated" frame: InputTextEx skips its shortcut processing then,
    // so the Ctrl+<key> route is only (re-)submitted the following frame and
    // granted one frame later. Pressing any earlier makes the press expire
    // (DownDuration != 0) before the route arrives.
    if (g_pending.field_id == field_id && !g_pending.keys_queued &&
        g.ActiveId == field_id && !g.ActiveIdIsJustActivated) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(g_pending.key, true);
        g_pending.press_time = g.Time;
        g_pending.keys_queued = true;
    }

    // Unique popup name per field (its own ImGuiID) so fields in the same
    // window don't collide. OpenPopup/BeginPopup follow the same pattern as
    // the chat input menu (IsItemHovered + IsMouseClicked(Right)).
    char popup_name[48];
    ImFormatString(popup_name, IM_ARRAYSIZE(popup_name), "##input_ctx_%08X", field_id);

    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup(popup_name);

    if (ImGui::BeginPopup(popup_name)) {
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

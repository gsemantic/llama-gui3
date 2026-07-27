#include "../include/ui/layout_controller.h"
#include <iostream>

namespace llama_gui {
namespace ui {

LayoutController::LayoutController()
    : left_panel_width_(250.0f)
    , right_panel_width_(300.0f)
    , bottom_panel_height_(200.0f)
    , show_menu_bar_(true)
    , show_status_bar_(true)
    , compact_mode_(false)
    , ui_dirty_main_layout_(true)
    , ui_dirty_left_panel_(true)
    , ui_dirty_right_panel_(true)
    , ui_dirty_bottom_panel_(true)
    , ui_dirty_status_bar_(true)
    , ui_dirty_chat_(true)
    , ui_dirty_conversations_(true)
    , ui_dirty_files_(true)
    , ui_dirty_menu_(true)
{
}

void LayoutController::renderMainLayout() {
    if (!main_layout_callback_) {
        return;
    }

    if (ui_dirty_main_layout_) {
        ui_dirty_main_layout_ = false;
    }

    main_layout_callback_();
}

void LayoutController::renderStatusBar() {
    if (!status_bar_callback_) {
        return;
    }

    if (ui_dirty_status_bar_) {
        ui_dirty_status_bar_ = false;
    }

    status_bar_callback_();
}

void LayoutController::setLeftPanelWidth(float width) {
    left_panel_width_ = width;
}

void LayoutController::setRightPanelWidth(float width) {
    right_panel_width_ = width;
}

void LayoutController::setBottomPanelHeight(float height) {
    bottom_panel_height_ = height;
}

} // namespace ui
} // namespace llama_gui

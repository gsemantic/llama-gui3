#pragma once

#include <string>
#include <functional>
#include <unordered_map>

#ifdef USE_IMGUI
#include "../external/imgui/imgui.h"
#endif

namespace llama_gui {
namespace ui {

/**
 * @brief Контроллер для управления макетом пользовательского интерфейса
 * 
 * Отвечает за:
 * - Рендеринг основного макета (main layout)
 * - Рендеринг статус-бара
 * - Управление флагами видимости панелей
 * - Обновление dirty flags
 */
class LayoutController {
public:
    LayoutController();
    ~LayoutController() = default;

    // =========================================================================
    // Управление макетом
    // =========================================================================

    /**
     * @brief Рендеринг основного макета
     */
    void renderMainLayout();

    /**
     * @brief Рендеринг статус-бара
     */
    void renderStatusBar();

    /**
     * @brief Настройка параметров макета
     */
    void setLeftPanelWidth(float width);
    void setRightPanelWidth(float width);
    void setBottomPanelHeight(float height);
    
    float getLeftPanelWidth() const { return left_panel_width_; }
    float getRightPanelWidth() const { return right_panel_width_; }
    float getBottomPanelHeight() const { return bottom_panel_height_; }

    // =========================================================================
    // Управление флагами видимости
    // =========================================================================

    /**
     * @brief Установить флаг видимости меню
     */
    void setShowMenuBar(bool show) { show_menu_bar_ = show; }

    /**
     * @brief Установить флаг видимости статус-бара
     */
    void setShowStatusBar(bool show) { show_status_bar_ = show; }

    /**
     * @brief Установить флаг компактного режима
     */
    void setCompactMode(bool compact) { compact_mode_ = compact; }

    bool isShowMenuBar() const { return show_menu_bar_; }
    bool isShowStatusBar() const { return show_status_bar_; }
    bool isCompactMode() const { return compact_mode_; }

    // =========================================================================
    // Dirty flags
    // =========================================================================

    /**
     * @brief Установить dirty flag для основного макета
     */
    void setDirtyMainLayout(bool dirty) { ui_dirty_main_layout_ = dirty; }

    /**
     * @brief Установить dirty flag для левой панели
     */
    void setDirtyLeftPanel(bool dirty) { ui_dirty_left_panel_ = dirty; }

    /**
     * @brief Установить dirty flag для правой панели
     */
    void setDirtyRightPanel(bool dirty) { ui_dirty_right_panel_ = dirty; }

    /**
     * @brief Установить dirty flag для нижней панели
     */
    void setDirtyBottomPanel(bool dirty) { ui_dirty_bottom_panel_ = dirty; }

    /**
     * @brief Установить dirty flag для статус-бара
     */
    void setDirtyStatusBar(bool dirty) { ui_dirty_status_bar_ = dirty; }

    /**
     * @brief Установить dirty flag для чата
     */
    void setDirtyChat(bool dirty) { ui_dirty_chat_ = dirty; }

    /**
     * @brief Установить dirty flag для бесед
     */
    void setDirtyConversations(bool dirty) { ui_dirty_conversations_ = dirty; }

    /**
     * @brief Установить dirty flag для файлов
     */
    void setDirtyFiles(bool dirty) { ui_dirty_files_ = dirty; }

    /**
     * @brief Установить dirty flag для меню
     */
    void setDirtyMenu(bool dirty) { ui_dirty_menu_ = dirty; }

    bool isDirtyMainLayout() const { return ui_dirty_main_layout_; }
    bool isDirtyLeftPanel() const { return ui_dirty_left_panel_; }
    bool isDirtyRightPanel() const { return ui_dirty_right_panel_; }
    bool isDirtyBottomPanel() const { return ui_dirty_bottom_panel_; }
    bool isDirtyStatusBar() const { return ui_dirty_status_bar_; }
    bool isDirtyChat() const { return ui_dirty_chat_; }
    bool isDirtyConversations() const { return ui_dirty_conversations_; }
    bool isDirtyFiles() const { return ui_dirty_files_; }
    bool isDirtyMenu() const { return ui_dirty_menu_; }

    // =========================================================================
    // Callbacks
    // =========================================================================

    using LayoutRenderCallback = std::function<void()>;
    using StatusBarRenderCallback = std::function<void()>;
    using PanelRenderCallback = std::function<void()>;

    void setMainLayoutRenderCallback(LayoutRenderCallback callback) {
        main_layout_callback_ = callback;
    }

    void setStatusBarRenderCallback(StatusBarRenderCallback callback) {
        status_bar_callback_ = callback;
    }

    void setLeftPanelRenderCallback(PanelRenderCallback callback) {
        left_panel_callback_ = callback;
    }

    void setRightPanelRenderCallback(PanelRenderCallback callback) {
        right_panel_callback_ = callback;
    }

    void setBottomPanelRenderCallback(PanelRenderCallback callback) {
        bottom_panel_callback_ = callback;
    }

    void setChatRenderCallback(PanelRenderCallback callback) {
        chat_callback_ = callback;
    }

    void setConversationsRenderCallback(PanelRenderCallback callback) {
        conversations_callback_ = callback;
    }

    void setFilesRenderCallback(PanelRenderCallback callback) {
        files_callback_ = callback;
    }

    void setMenuRenderCallback(PanelRenderCallback callback) {
        menu_callback_ = callback;
    }

private:
    // Layout parameters
    float left_panel_width_ = 250.0f;
    float right_panel_width_ = 300.0f;
    float bottom_panel_height_ = 200.0f;

    // Visibility flags
    bool show_menu_bar_ = true;
    bool show_status_bar_ = true;
    bool compact_mode_ = false;

    // Dirty flags for smart redraw
    bool ui_dirty_main_layout_ = true;
    bool ui_dirty_left_panel_ = true;
    bool ui_dirty_right_panel_ = true;
    bool ui_dirty_bottom_panel_ = true;
    bool ui_dirty_status_bar_ = true;
    bool ui_dirty_chat_ = true;
    bool ui_dirty_conversations_ = true;
    bool ui_dirty_files_ = true;
    bool ui_dirty_menu_ = true;

    // Callbacks
    LayoutRenderCallback main_layout_callback_;
    StatusBarRenderCallback status_bar_callback_;
    PanelRenderCallback left_panel_callback_;
    PanelRenderCallback right_panel_callback_;
    PanelRenderCallback bottom_panel_callback_;
    PanelRenderCallback chat_callback_;
    PanelRenderCallback conversations_callback_;
    PanelRenderCallback files_callback_;
    PanelRenderCallback menu_callback_;
};

} // namespace ui
} // namespace llama_gui

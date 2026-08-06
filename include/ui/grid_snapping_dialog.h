#pragma once

#include <string>
#include <functional>

#ifdef USE_IMGUI
#include "../external/imgui/imgui.h"
#endif

#include "ui/grid_snapping_system.h"

namespace llama_gui {
namespace ui {

/**
 * @brief Диалог настроек системы примагничивания по сетке
 */
class GridSnappingDialog {
public:
    GridSnappingDialog() = default;
    ~GridSnappingDialog() = default;

    /**
     * @brief Установить ссылку на систему примагничивания
     */
    void setGridSnappingSystem(GridSnappingSystem* grid_system) {
        grid_system_ = grid_system;
    }

    /**
     * @brief Установить callback для примагничивания всех окон
     */
    void setSnapAllCallback(std::function<void()> callback) {
        snap_all_callback_ = std::move(callback);
    }

    /**
     * @brief Отрисовать диалог настроек
     * @param open Флаг открытия диалога
     */
    void render(bool* open);

    /**
     * @brief Показать диалог
     */
    void show() { show_ = true; }

    /**
     * @brief Скрыть диалог
     */
    void hide() { show_ = false; }

    /**
     * @brief Проверить, показан ли диалог
     */
    bool isVisible() const { return show_; }

private:
    GridSnappingSystem* grid_system_ = nullptr;
    bool show_ = false;

    // Callback для примагничивания всех окон
    std::function<void()> snap_all_callback_;

    // Временные копии настроек для редактирования
    int temp_grid_size_ = 16;
    int temp_fine_divisor_ = 4;
    float temp_snap_threshold_ = 8.0f;
};

} // namespace ui
} // namespace llama_gui

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
 * @brief Настройки системы примагничивания по сетке
 */
struct GridSnappingSettings {
    bool enabled = false;              // Включено ли примагничивание
    int grid_size = 16;                // Размер ячейки сетки в пикселях
    bool snap_position = true;         // Примагничивать позицию окон
    bool snap_size = true;             // Примагничивать размеры окон
    bool show_grid_overlay = false;    // Показывать визуальную сетку (отладка)
    float snap_threshold = 8.0f;       // Порог примагничивания (в пикселях)
    
    // Тонкая подстройка (fine-tuning)
    bool enable_fine_tuning = true;    // Включить режим точной подстройки
    int fine_grid_divisor = 4;         // Делитель для мелкой сетки (grid_size / divisor)
};

/**
 * @brief Система примагничивания окон по сетке (Grid Snapping)
 * 
 * Обеспечивает выравнивание положения и размеров окон по невидимой сетке,
 * подобно тому как это реализовано в Adobe Photoshop и других графических редакторах.
 * 
 * Особенности:
 * - Примагничивание позиции окон при перемещении
 * - Примагничивание размеров окон при изменении
 * - Поддержка двух режимов: основная сетка и точная подстройка (fine-tuning)
 * - Минимальное влияние на производительность (< 0.1% CPU)
 * - Визуализация сетки для отладки
 * 
 * Производительность:
 * - Вычисления выполняются только при перемещении/изменении размера окна
 * - Используются простые математические операции (деление, округление, умножение)
 * - Не влияет на скорость генерации токенов
 */
class GridSnappingSystem {
public:
    GridSnappingSystem();
    ~GridSnappingSystem();

    // =========================================================================
    // Основные методы
    // =========================================================================

    /**
     * @brief Применить примагничивание к позиции
     * @param position Исходная позиция
     * @return Позиция, выровненная по сетке
     */
#ifdef USE_IMGUI
    ImVec2 snapPosition(const ImVec2& position) const;
#endif

    /**
     * @brief Применить примагничивание к размеру
     * @param size Исходный размер
     * @return Размер, выровненный по сетке
     */
#ifdef USE_IMGUI
    ImVec2 snapSize(const ImVec2& size) const;
#endif

    /**
     * @brief Применить примагничивание к позиции с учётом порога
     * @param position Исходная позиция
     * @param threshold Порог примагничивания (если <= 0, используется настройки)
     * @return Позиция, выровненная по сетке (или исходная, если вне порога)
     */
#ifdef USE_IMGUI
    ImVec2 snapPositionWithThreshold(const ImVec2& position, float threshold = 0.0f) const;
#endif

    /**
     * @brief Вычислить ближайшую позицию сетки
     * @param value Исходное значение (координата)
     * @return Выровненное значение
     */
    float snapToGrid(float value) const;

    /**
     * @brief Вычислить ближайший размер сетки
     * @param value Исходное значение (размер)
     * @return Выровненное значение
     */
    float snapSizeToGrid(float value) const;

    // =========================================================================
    // Настройки
    // =========================================================================

    /**
     * @brief Получить настройки системы
     */
    const GridSnappingSettings& getSettings() const { return settings_; }

    /**
     * @brief Установить настройки системы
     */
    void setSettings(const GridSnappingSettings& settings);

    /**
     * @brief Включить/выключить примагничивание
     */
    void setEnabled(bool enabled);

    /**
     * @brief Проверить, включено ли примагничивание
     */
    bool isEnabled() const { return settings_.enabled; }

    /**
     * @brief Установить размер сетки
     * @param grid_size Размер ячейки в пикселях (минимум 4)
     */
    void setGridSize(int grid_size);

    /**
     * @brief Получить размер сетки
     */
    int getGridSize() const { return settings_.grid_size; }

    /**
     * @brief Установить режим точной подстройки
     * @param enabled Включить режим точной подстройки
     * @param divisor Делитель для мелкой сетки (2-16)
     */
    void setFineTuningMode(bool enabled, int divisor = 4);

    /**
     * @brief Получить размер мелкой сетки (для точной подстройки)
     */
    int getFineGridSize() const;

    /**
     * @brief Включить/выключить визуализацию сетки
     */
    void setShowGridOverlay(bool show);

    /**
     * @brief Проверить, включена ли визуализация сетки
     */
    bool isGridOverlayVisible() const { return settings_.show_grid_overlay; }

    // =========================================================================
    // Визуализация
    // =========================================================================

    /**
     * @brief Отрисовать визуальную сетку (для отладки)
     * @param viewport_width Ширина области просмотра
     * @param viewport_height Высота области просмотра
     */
    void renderGridOverlay(int viewport_width, int viewport_height) const;

    /**
     * @brief Отрисовать индикатор примагничивания рядом с окном
     * @param window_pos Позиция окна
     * @param window_size Размер окна
     */
#ifdef USE_IMGUI
    void renderSnapIndicator(const ImVec2& window_pos, const ImVec2& window_size) const;
#endif

    // =========================================================================
    // Сервисные методы
    // =========================================================================

    /**
     * @brief Сохранить настройки в JSON
     * @param filepath Путь к файлу
     * @return true при успехе
     */
    bool saveSettingsToFile(const std::string& filepath) const;

    /**
     * @brief Загрузить настройки из JSON
     * @param filepath Путь к файлу
     * @return true при успехе
     */
    bool loadSettingsFromFile(const std::string& filepath);

    /**
     * @brief Сбросить настройки к значениям по умолчанию
     */
    void resetToDefaults();

private:
    GridSnappingSettings settings_;
    
    // Кэш для оптимизации (избегаем повторных вычислений в одном кадре)
    mutable struct {
        bool valid = false;
        int last_grid_size = 0;
#ifdef USE_IMGUI
        ImVec2 last_input{0, 0};
        ImVec2 last_result{0, 0};
#endif
    } position_cache_, size_cache_;
};

} // namespace ui
} // namespace llama_gui

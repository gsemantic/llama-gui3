#pragma once

#include "bench_tab_base.h"
#include <string>
#include <vector>

namespace llama_gui {
namespace ui {

/**
 * @class HistoryTab
 * @brief Вкладка для просмотра истории запусков бенчмарка
 *
 * Показывает историю всех запусков с возможностью фильтрации.
 */
class HistoryTab : public BenchTabBase {
public:
    HistoryTab();
    ~HistoryTab() override = default;

    // Запрет копирования
    HistoryTab(const HistoryTab&) = delete;
    HistoryTab& operator=(const HistoryTab&) = delete;

    // =========================================================================
    // Методы интерфейса
    // =========================================================================

    void render() override;
    const char* getTitle() const override;
    const char* getIcon() const override;

    // =========================================================================
    // Обработчики событий
    // =========================================================================

    void refreshResults();

private:
    // =========================================================================
    // Рендеринг компонентов
    // =========================================================================

    void renderResultsTable();

private:
    // История запусков
    std::vector<std::string> history_;
};

} // namespace ui
} // namespace llama_gui

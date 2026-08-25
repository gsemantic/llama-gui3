#pragma once

#include "bench_tab_base.h"
#include <string>
#include <vector>

namespace llama_gui {
namespace bench {
class LlamaBenchResults;
}
namespace ui {

/**
 * @class HistoryTab
 * @brief Вкладка для просмотра истории запусков бенчмарка
 *
 * Показывает историю всех запусков с возможностью фильтрации.
 * Данные берутся из реального хранилища LlamaBenchResults
 * (bench_results/history.json).
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

    /** Источник данных (не владеющий указатель; должен переживать вкладку). */
    void setResultsSource(bench::LlamaBenchResults* source);

    /** Перезагрузить историю с диска (history.json). */
    void refreshResults();

private:
    // =========================================================================
    // Рендеринг компонентов
    // =========================================================================

    void renderResultsTable();

private:
    bench::LlamaBenchResults* results_source_ = nullptr;
};

} // namespace ui
} // namespace llama_gui

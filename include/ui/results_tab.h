#pragma once

#include "bench_tab_base.h"
#include <string>
#include <vector>

namespace llama_gui {
namespace ui {

/**
 * @class ResultsTab
 * @brief Вкладка для просмотра и экспорта результатов бенчмарка
 *
 * Показывает результаты тестов с возможностью фильтрации и экспорта.
 */
class ResultsTab : public BenchTabBase {
public:
    ResultsTab();
    ~ResultsTab() override = default;

    // Запрет копирования
    ResultsTab(const ResultsTab&) = delete;
    ResultsTab& operator=(const ResultsTab&) = delete;

    // =========================================================================
    // Методы интерфейса
    // =========================================================================

    void render() override;
    const char* getTitle() const override;
    const char* getIcon() const override;

    // =========================================================================
    // Обработчики событий
    // =========================================================================

    void onExportJson();
    void onExportCsv();
    void onExportMarkdown();
    void onAnalyzeWithModel();

private:
    // =========================================================================
    // Рендеринг компонентов
    // =========================================================================

    void renderResultsTable();

private:
    // Результаты
    std::vector<std::string> results_;
    int results_filter_idx_ = 0;
};

} // namespace ui
} // namespace llama_gui

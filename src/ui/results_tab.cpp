#include "results_tab.h"
#include "core/logger.h"

namespace llama_gui {
namespace ui {

ResultsTab::ResultsTab()
{
    results_.push_back("Benchmark result 1");
    results_.push_back("Benchmark result 2");
    results_.push_back("Benchmark result 3");
}

const char* ResultsTab::getTitle() const
{
    return "Results";
}

const char* ResultsTab::getIcon() const
{
    return "📊";
}

void ResultsTab::render()
{
    renderWithSpacing();
    renderInfoMessage("Benchmark Results");
    renderSeparator();
    renderWithSpacing();

    renderResultsTable();
    renderWithSpacing();

    // Кнопки экспорта
    if (ImGui::Button("Export JSON")) {
        onExportJson();
    }
    ImGui::SameLine();
    if (ImGui::Button("Export CSV")) {
        onExportCsv();
    }
    ImGui::SameLine();
    if (ImGui::Button("Export Markdown")) {
        onExportMarkdown();
    }
    ImGui::SameLine();
    if (ImGui::Button("Analyze with Model")) {
        onAnalyzeWithModel();
    }
}

void ResultsTab::renderResultsTable()
{
    // TODO: Реализовать таблицу результатов
    if (ImGui::BeginTable("ResultsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Date");
        ImGui::TableSetupColumn("Profile");
        ImGui::TableSetupColumn("Prompt TPS");
        ImGui::TableSetupColumn("Gen TPS");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();

        // TODO: Заполнить таблицу результатами
        ImGui::EndTable();
    }
}

void ResultsTab::onExportJson()
{
    LOG_INFO("ResultsTab: Exporting JSON");
}

void ResultsTab::onExportCsv()
{
    LOG_INFO("ResultsTab: Exporting CSV");
}

void ResultsTab::onExportMarkdown()
{
    LOG_INFO("ResultsTab: Exporting Markdown");
}

void ResultsTab::onAnalyzeWithModel()
{
    // TODO: Реализовать анализ с моделью
    LOG_INFO("ResultsTab: Analyzing with model");
}

} // namespace ui
} // namespace llama_gui

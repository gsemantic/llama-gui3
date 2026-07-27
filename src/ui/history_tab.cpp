#include "history_tab.h"
#include "core/logger.h"

namespace llama_gui {
namespace ui {

HistoryTab::HistoryTab()
{
    history_.push_back("Benchmark run 1");
    history_.push_back("Benchmark run 2");
    history_.push_back("Benchmark run 3");
}

const char* HistoryTab::getTitle() const
{
    return "History";
}

const char* HistoryTab::getIcon() const
{
    return "🕐";
}

void HistoryTab::render()
{
    renderWithSpacing();
    renderInfoMessage("Benchmark History");
    renderSeparator();
    renderWithSpacing();

    // TODO: Показать статистику
    ImGui::Text("Total runs: %zu", history_.size());
    ImGui::Text("Comparisons: %zu", 0);  // TODO

    renderWithSpacing();
    renderSeparator();
    renderWithSpacing();

    // История запусков
    if (ImGui::BeginTable("HistoryTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Date");
        ImGui::TableSetupColumn("Profile");
        ImGui::TableSetupColumn("Prompt TPS");
        ImGui::TableSetupColumn("Gen TPS");
        ImGui::TableSetupColumn("Status");

        ImGui::TableHeadersRow();

        // TODO: Заполнить таблицу историей
        for (const auto& item : history_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Recent");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", item.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("0.0");
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("0.0");
            ImGui::TableSetColumnIndex(4);
            ImGui::TextDisabled("Ready");
        }

        ImGui::EndTable();
    }
}

void HistoryTab::refreshResults()
{
    // TODO: Обновить историю
    LOG_INFO("HistoryTab: Refreshing results");
}

void HistoryTab::renderResultsTable()
{
    // TODO: Реализовать таблицу истории
}

} // namespace ui
} // namespace llama_gui

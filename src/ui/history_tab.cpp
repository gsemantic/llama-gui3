#include "history_tab.h"
#include "core/logger.h"
#include "../include/bench/llama_bench_results.h"
#include "../include/bench/bench_types.h"
#include "../external/imgui/imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace llama_gui {
namespace ui {

namespace {

std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
    return buf;
}

ImVec4 statusColor(bench::BenchStatus status) {
    switch (status) {
        case bench::BenchStatus::Completed: return ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
        case bench::BenchStatus::Failed:    return ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        case bench::BenchStatus::Running:   return ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
        default:                            return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    }
}

} // namespace

HistoryTab::HistoryTab() = default;

const char* HistoryTab::getTitle() const
{
    return "History";
}

const char* HistoryTab::getIcon() const
{
    return "🕐";
}

void HistoryTab::setResultsSource(bench::LlamaBenchResults* source)
{
    results_source_ = source;
}

void HistoryTab::render()
{
    renderWithSpacing();
    renderInfoMessage("Benchmark History");
    renderSeparator();
    renderWithSpacing();

    const size_t total_runs = results_source_ ? results_source_->getResults().size() : 0;
    const size_t total_comparisons =
        results_source_ ? results_source_->getComparisons().size() : 0;

    ImGui::Text("Total runs: %zu", total_runs);
    ImGui::Text("Comparisons: %zu", total_comparisons);

    renderWithSpacing();
    renderSeparator();
    renderWithSpacing();

    if (total_runs == 0) {
        ImGui::TextDisabled("История пуста: запустите бенчмарк во вкладке Run Benchmark.");
        return;
    }

    renderResultsTable();
}

void HistoryTab::refreshResults()
{
    if (results_source_) {
        if (results_source_->loadFromHistory()) {
            LOG_INFO("HistoryTab: история перезагружена из history.json");
        }
    }
}

void HistoryTab::renderResultsTable()
{
    if (!results_source_) return;

    const auto& results = results_source_->getResults();

    // Свежие записи сверху; длинную историю ограничиваем последними 100
    const size_t max_rows = 100;
    const size_t skip = results.size() > max_rows ? results.size() - max_rows : 0;

    if (ImGui::BeginTable("HistoryTable", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Prompt TPS", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Gen TPS", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableHeadersRow();

        for (size_t i = results.size(); i > skip; --i) {
            const auto& r = results[i - 1];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(formatTimestamp(r.start_time).c_str());
            ImGui::TableSetColumnIndex(1);
            const std::string& profile = !r.profile_name.empty()
                ? r.profile_name : r.model_name;
            ImGui::TextUnformatted(profile.c_str());
            if (!r.model_path.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", r.model_path.c_str());
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.2f", r.prompt_tokens_per_sec);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.2f", r.gen_tokens_per_sec);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(statusColor(r.status), "%s",
                               bench::statusToString(r.status).c_str());
            if (!r.error_message.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", r.error_message.c_str());
            }
        }

        ImGui::EndTable();
    }
}

} // namespace ui
} // namespace llama_gui

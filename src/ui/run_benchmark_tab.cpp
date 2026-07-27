#include "run_benchmark_tab.h"
#include "core/logger.h"

namespace llama_gui {
namespace ui {

RunBenchmarkTab::RunBenchmarkTab()
{
    available_profiles_.push_back("Default");
    available_profiles_.push_back("Fast");
    available_profiles_.push_back("Balanced");
    available_profiles_.push_back("Quality");
    profile_selected_.resize(available_profiles_.size(), false);
    profile_selected_[0] = true;  // Выбрать первый профиль по умолчанию
}

const char* RunBenchmarkTab::getTitle() const
{
    return "Run Benchmark";
}

const char* RunBenchmarkTab::getIcon() const
{
    return "▶";
}

void RunBenchmarkTab::render()
{
    renderWithSpacing();
    renderInfoMessage("Run benchmark for a single model/profile");
    renderSeparator();
    renderWithSpacing();

    // Выбор профиля
    renderProfileSelection();
    renderWithSpacing();

    // Параметры теста
    renderTestParameters();
    renderWithSpacing();

    // Индикатор прогресса если выполняется
    if (isRunning()) {
        renderProgressIndicator();
        renderWithSpacing();
    }

    // Кнопки действий
    renderActionButtons();
}

void RunBenchmarkTab::renderProfileSelection()
{
    ImGui::Text("Select Profile:");
    ImGui::Spacing();

    // Фильтр
    char filter_buf[128] = "";
    if (ImGui::InputTextWithHint("##ProfileFilter", "Search profiles...", filter_buf, IM_ARRAYSIZE(filter_buf))) {
        // TODO: Реализовать фильтрацию
    }

    ImGui::Spacing();

    // Список профилей
    if (ImGui::BeginListBox("##ProfilesList", ImVec2(-FLT_MIN, 150))) {
        for (size_t i = 0; i < available_profiles_.size(); ++i) {
            const std::string& profile = available_profiles_[i];
            bool selected = profile_selected_[i];

            if (ImGui::Selectable(profile.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                // Одиночный выбор
                for (auto& s : profile_selected_) {
                    s = false;
                }
                profile_selected_[i] = true;
                onProfileSelected(profile, true);
            }
        }
        ImGui::EndListBox();
    }

    ImGui::TextDisabled("Hold Ctrl to select multiple profiles");
}

void RunBenchmarkTab::renderTestParameters()
{
    ImGui::Text("Test Parameters:");
    renderSeparator();
    renderWithSpacing();

    if (ImGui::BeginTable("ParamsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Prompt tokens
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Prompt Tokens");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##NPrompt", &n_prompt_);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("Number of tokens in the prompt");

        // Generation tokens
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Generation Tokens");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##NGen", &n_gen_);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("Number of tokens to generate");

        // Batch size
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Batch Size");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##BatchSize", &batch_size_);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("Batch size for processing");

        // Threads
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Threads");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##Threads", &threads_);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("Number of CPU threads");

        // GPU layers
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("GPU Layers");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##NGPULayers", &n_gpu_layers_);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("Number of layers to offload to GPU");

        ImGui::EndTable();
    }
}

void RunBenchmarkTab::renderProgressIndicator()
{
    ImGui::Text("Status: %s", getCurrentStatus().c_str());

    if (getProgress() >= 0 && getProgress() <= 100) {
        char progress_buf[64];
        snprintf(progress_buf, IM_ARRAYSIZE(progress_buf), "%d%%", getProgress());
        ImGui::ProgressBar(static_cast<float>(getProgress()) / 100.0f, ImVec2(-FLT_MIN, 20), progress_buf);
    }
}

void RunBenchmarkTab::renderActionButtons()
{
    renderSeparator();
    renderWithSpacing();

    if (isRunning()) {
        // Кнопка отмены
        if (ImGui::Button("Cancel", ImVec2(150, 0))) {
            onCancelBenchmark();
        }
    } else {
        // Кнопка запуска
        if (ImGui::Button("Start Benchmark", ImVec2(150, 0))) {
            onStartBenchmark();
        }
    }
}

void RunBenchmarkTab::onStartBenchmark()
{
    // TODO: Реализовать запуск бенчмарка
    LOG_INFO("RunBenchmarkTab: Starting benchmark");
}

void RunBenchmarkTab::onCancelBenchmark()
{
    // TODO: Реализовать отмену бенчмарка
    LOG_INFO("RunBenchmarkTab: Cancelling benchmark");
}

void RunBenchmarkTab::updateProfileList()
{
    // TODO: Загрузить список профилей из директории
}

void RunBenchmarkTab::onProfileSelected(const std::string& profile, bool selected)
{
    // TODO: Обработать выбор профиля
}

void RunBenchmarkTab::drawColoredText(const char* text, uint32_t color)
{
    // TODO: Реализовать отрисовку цветного текста
    ImGui::Text("%s", text);
}

} // namespace ui
} // namespace llama_gui

#include "compare_profiles_tab.h"
#include "core/logger.h"

namespace llama_gui {
namespace ui {

CompareProfilesTab::CompareProfilesTab()
{
    available_profiles_.push_back("Default");
    available_profiles_.push_back("Fast");
    available_profiles_.push_back("Balanced");
    available_profiles_.push_back("Quality");
    profile_selected_.resize(available_profiles_.size(), false);
}

const char* CompareProfilesTab::getTitle() const
{
    return "Compare Profiles";
}

const char* CompareProfilesTab::getIcon() const
{
    return "⚖️";
}

void CompareProfilesTab::render()
{
    renderWithSpacing();
    renderInfoMessage("Compare multiple profiles against each other");
    renderSeparator();
    renderWithSpacing();

    // Важное сообщение о том, что параметры берутся из профилей
    renderInfoMessage("Parameters are taken from EACH profile");
    renderWithSpacing();

    // Модальное окно с информацией
    if (ImGui::Button("?")) {
        // TODO: Открыть модальное окно с информацией
    }
    renderWithSpacing();

    renderWarningMessage("Select profiles to compare (hold Ctrl for multiple):");
    renderWithSpacing();

    renderProfileSelection();
    renderWithSpacing();

    // Показываем параметры только для информации (не редактируемые)
    renderTestParametersInfo();
    renderWithSpacing();

    if (isRunning()) {
        renderProgressIndicator();
        renderWithSpacing();
    }

    renderActionButtons();
}

void CompareProfilesTab::renderProfileSelection()
{
    // TODO: Реализовать выбор профилей
    ImGui::Text("Profiles selection placeholder");
}

void CompareProfilesTab::renderTestParametersInfo()
{
    // TODO: Показать параметры из профилей
    ImGui::Text("Test parameters info placeholder");
}

void CompareProfilesTab::renderProgressIndicator()
{
    ImGui::Text("Status: %s", getCurrentStatus().c_str());

    if (getProgress() >= 0 && getProgress() <= 100) {
        char progress_buf[64];
        snprintf(progress_buf, IM_ARRAYSIZE(progress_buf), "%d%%", getProgress());
        ImGui::ProgressBar(static_cast<float>(getProgress()) / 100.0f, ImVec2(-FLT_MIN, 20), progress_buf);
    }
}

void CompareProfilesTab::renderActionButtons()
{
    renderSeparator();
    renderWithSpacing();

    if (isRunning()) {
        if (ImGui::Button("Cancel", ImVec2(150, 0))) {
            // TODO: Реализовать отмену
        }
    } else {
        if (ImGui::Button("Start Comparison", ImVec2(150, 0))) {
            // TODO: Запустить сравнение
        }
    }
}

void CompareProfilesTab::onProfileSelected(const std::string& profile, bool selected)
{
    LOG_INFO("CompareProfilesTab: Profile selected: " + profile + ", selected: " + std::to_string(selected));
}

void CompareProfilesTab::onRefreshProfiles()
{
    // TODO: Обновить список профилей
}

} // namespace ui
} // namespace llama_gui

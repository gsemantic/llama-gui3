#pragma once

#include "bench_tab_base.h"
#include <string>
#include <vector>

namespace llama_gui {
namespace ui {

/**
 * @class CompareProfilesTab
 * @brief Вкладка для сравнения нескольких профилей
 *
 * Позволяет выбрать несколько профилей и запустить их сравнение.
 */
class CompareProfilesTab : public BenchTabBase {
public:
    CompareProfilesTab();
    ~CompareProfilesTab() override = default;

    // Запрет копирования
    CompareProfilesTab(const CompareProfilesTab&) = delete;
    CompareProfilesTab& operator=(const CompareProfilesTab&) = delete;

    // =========================================================================
    // Методы интерфейса
    // =========================================================================

    void render() override;
    const char* getTitle() const override;
    const char* getIcon() const override;

    // =========================================================================
    // Обработчики событий
    // =========================================================================

    void onProfileSelected(const std::string& profile, bool selected);
    void onRefreshProfiles();

private:
    // =========================================================================
    // Рендеринг компонентов
    // =========================================================================

    void renderProfileSelection();
    void renderTestParametersInfo();
    void renderProgressIndicator();
    void renderActionButtons();

private:
    // Профили
    std::vector<std::string> available_profiles_;
    std::vector<char> profile_selected_;
    std::string profiles_filter_;

    // UI состояние
    bool show_error_modal_ = false;
    std::string error_message_;
};

} // namespace ui
} // namespace llama_gui

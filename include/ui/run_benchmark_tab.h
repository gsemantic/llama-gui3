#pragma once

#include "bench_tab_base.h"
#include <string>
#include <vector>

namespace llama_gui {
namespace ui {

/**
 * @class RunBenchmarkTab
 * @brief Вкладка для запуска одиночного теста
 *
 * Позволяет выбрать профиль и запустить бенчмарк с параметрами.
 */
class RunBenchmarkTab : public BenchTabBase {
public:
    RunBenchmarkTab();
    ~RunBenchmarkTab() override = default;

    // Запрет копирования
    RunBenchmarkTab(const RunBenchmarkTab&) = delete;
    RunBenchmarkTab& operator=(const RunBenchmarkTab&) = delete;

    // =========================================================================
    // Методы интерфейса
    // =========================================================================

    void render() override;
    const char* getTitle() const override;
    const char* getIcon() const override;

    // =========================================================================
    // Обработчики событий
    // =========================================================================

    void onStartBenchmark();
    void onCancelBenchmark();

    // =========================================================================
    // Обновление состояния
    // =========================================================================

    void updateProfileList();
    void onProfileSelected(const std::string& profile, bool selected);

private:
    // =========================================================================
    // Рендеринг компонентов
    // =========================================================================

    void renderProfileSelection();
    void renderTestParameters();
    void renderProgressIndicator();
    void renderActionButtons();

    // =========================================================================
    // Вспомогательные методы
    // =========================================================================

    void drawColoredText(const char* text, uint32_t color);

private:
    // Профили
    std::vector<std::string> available_profiles_;
    std::vector<char> profile_selected_;
    std::string profiles_filter_;
    int active_profile_idx_ = -1;

    // Параметры теста
    int n_prompt_ = 512;
    int n_gen_ = 128;
    int batch_size_ = 2048;
    int threads_ = 2;
    int n_gpu_layers_ = 0;
    int repetitions_ = 3;
    std::string context_depths_ = "0,4096,8192";

    // UI состояния
    bool show_error_modal_ = false;
    std::string error_message_;
};

} // namespace ui
} // namespace llama_gui

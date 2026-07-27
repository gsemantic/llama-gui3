#include "../include/ui/llama_bench_dialog.h"
#include "../include/bench/bench_common.h"
#include "../include/bench/profile_adapter.h"
#include "../include/core/config_manager.h"
#include "../include/core/server_manager.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <thread>

namespace llama_gui {
namespace ui {

/**
 * @brief Внутренняя реализация LlamaBenchDialog
 */
struct LlamaBenchDialog::Impl {
    bool visible = false;
    bool initialized = false;

    // Модуль llama-bench
    llama_gui::bench::LlamaBenchModule bench_module;

    // Сервер-менеджер (ссылка из MainWindow)
    std::shared_ptr<llama_gui::core::ServerManager> server_manager;
    bool server_was_running = false;  // Был ли сервер запущен до бенчмарка

    // Профили
    std::string profiles_directory;
    std::vector<std::string> available_profiles;
    std::vector<char> profile_selected;
    std::string profiles_filter;

    // Параметры теста
    int n_prompt = 512;
    int n_gen = 128;
    int batch_size = 2048;
    int threads = 2;
    int n_gpu_layers = 0;
    int repetitions = 3;
    std::string context_depths = "0,4096,8192";

    // Состояние выполнения
    bool running = false;
    int progress = 0;
    std::string current_status;

    // Вкладки
    int active_tab = 0;

    // Классы вкладок
    std::unique_ptr<RunBenchmarkTab> run_tab;
    std::unique_ptr<CompareProfilesTab> compare_tab;
    std::unique_ptr<ResultsTab> results_tab;
    std::unique_ptr<HistoryTab> history_tab;

    // Результаты
    bool show_results = false;
    int results_filter_idx = 0;

    // Экспорт
    std::string last_export_path;

    // UI состояния
    bool show_error_modal = false;
    std::string error_message;
};

// ============================================================================
// Конструктор/деструктор
// ============================================================================

LlamaBenchDialog::LlamaBenchDialog()
    : pimpl_(std::make_unique<Impl>())
{
    // Создать вкладки
    pimpl_->run_tab = std::make_unique<RunBenchmarkTab>();
    pimpl_->compare_tab = std::make_unique<CompareProfilesTab>();
    pimpl_->results_tab = std::make_unique<ResultsTab>();
    pimpl_->history_tab = std::make_unique<HistoryTab>();
}

LlamaBenchDialog::~LlamaBenchDialog() {
    shutdown();
}

// ============================================================================
// Управление видимостью
// ============================================================================

void LlamaBenchDialog::setVisible(bool visible) {
    pimpl_->visible = visible;
}

bool LlamaBenchDialog::isVisible() const {
    return pimpl_->visible;
}

void LlamaBenchDialog::toggle() {
    pimpl_->visible = !pimpl_->visible;
}

// ============================================================================
// Управление сервером
// ============================================================================

void LlamaBenchDialog::setServerManager(std::shared_ptr<llama_gui::core::ServerManager> server_manager) {
    pimpl_->server_manager = server_manager;
}

bool LlamaBenchDialog::stopServerForBenchmark() {
    if (!pimpl_->server_manager) {
        return false;
    }

    // Проверить запущен ли сервер
    if (pimpl_->server_manager->is_server_running()) {
        pimpl_->server_was_running = true;

        // Остановить сервер
        std::cout << "Llama Bench: Stopping server for benchmark..." << std::endl;
        pimpl_->server_manager->stop_server(true);  // blocking = true

        // Подождать пока сервер действительно остановится (до 5 секунд)
        int wait_count = 0;
        while (pimpl_->server_manager->is_server_running() && wait_count < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_count++;
        }

        if (pimpl_->server_manager->is_server_running()) {
            std::cerr << "Llama Bench: Warning - server did not stop cleanly" << std::endl;
        } else {
            std::cout << "Llama Bench: Server stopped successfully" << std::endl;
        }
        return true;
    }

    pimpl_->server_was_running = false;
    return false;
}

bool LlamaBenchDialog::restartServerAfterBenchmark() {
    if (!pimpl_->server_manager || !pimpl_->server_was_running) {
        return false;
    }

    // Запустить сервер снова
    pimpl_->server_manager->start_server();

    std::cout << "Llama Bench: Server restarted after benchmark" << std::endl;
    pimpl_->server_was_running = false;
    return true;
}

// ============================================================================
// Инициализация
// ============================================================================

bool LlamaBenchDialog::initialize(const std::string& llama_bench_path,
                                  const std::string& profiles_dir) {
    if (pimpl_->initialized) {
        return true;
    }

    pimpl_->profiles_directory = profiles_dir;

    // Найти путь к llama-bench если не указан
    std::string bench_path = llama_bench_path;
    if (bench_path.empty()) {
        bench_path = findLlamaBenchPath();
    }

    if (bench_path.empty()) {
        pimpl_->error_message = "llama-bench executable not found";
        pimpl_->show_error_modal = true;
        return false;
    }

    // Инициализировать модуль
    if (!pimpl_->bench_module.initialize(bench_path)) {
        pimpl_->error_message = "Failed to initialize llama-bench module";
        pimpl_->show_error_modal = true;
        return false;
    }

    // Настроить колбэки
    pimpl_->bench_module.setStatusCallback([this](const std::string& status) {
        pimpl_->current_status = status;
    });

    pimpl_->bench_module.setProgressCallback([this](int percent, const std::string& status) {
        pimpl_->progress = percent;
        pimpl_->current_status = status;
    });

    // Callback для перезапуска сервера после завершения бенчмарка
    pimpl_->bench_module.setBenchmarkCompleteCallback([this]() {
        restartServerAfterBenchmark();
    });

    // Загрузить профили
    updateProfileList();

    pimpl_->initialized = true;

    return true;
}

void LlamaBenchDialog::shutdown() {
    pimpl_->bench_module.shutdown();
    pimpl_->initialized = false;
}

// ============================================================================
// Сброс состояния
// ============================================================================

void LlamaBenchDialog::reset() {
    pimpl_->running = false;
    pimpl_->progress = 0;
    pimpl_->current_status.clear();
    pimpl_->active_tab = 0;
}

// ============================================================================
// Рендеринг
// ============================================================================

void LlamaBenchDialog::render() {
    if (!pimpl_->visible) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Llama Bench - Model Comparison", &pimpl_->visible)) {
        ImGui::End();
        return;
    }

    // Вкладки
    const char* tabs[] = { "Run Benchmark", "Compare Profiles", "Results", "History" };

    ImGui::BeginTabBar("##LlamaBenchTabs");

    if (ImGui::BeginTabItem(tabs[0])) {
        pimpl_->active_tab = 0;
        pimpl_->run_tab->updateState(pimpl_->running, pimpl_->progress, pimpl_->current_status);
        pimpl_->run_tab->render();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(tabs[1])) {
        pimpl_->active_tab = 1;
        pimpl_->compare_tab->updateState(pimpl_->running, pimpl_->progress, pimpl_->current_status);
        pimpl_->compare_tab->render();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(tabs[2])) {
        pimpl_->active_tab = 2;
        pimpl_->results_tab->updateState(pimpl_->running, pimpl_->progress, pimpl_->current_status);
        pimpl_->results_tab->render();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(tabs[3])) {
        pimpl_->active_tab = 3;
        pimpl_->history_tab->updateState(pimpl_->running, pimpl_->progress, pimpl_->current_status);
        pimpl_->history_tab->render();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();

    // Статус бар
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginGroup();
    ImGui::TextDisabled("Status: %s", pimpl_->current_status.empty() ? "Ready" : pimpl_->current_status.c_str());
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::TextDisabled("Profiles: %zu", pimpl_->available_profiles.size());
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::TextDisabled("Results: %zu", pimpl_->bench_module.getResults().getTotalResults());
    ImGui::EndGroup();

    ImGui::End();

    // Модальное окно ошибки
    if (pimpl_->show_error_modal) {
        ImGui::OpenPopup("Error");
        pimpl_->show_error_modal = false;
    }

    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Error: %s", pimpl_->error_message.c_str());
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void LlamaBenchDialog::updateTabState() {
    // Обновить состояние всех вкладок
    pimpl_->run_tab->updateState(pimpl_->running, pimpl_->progress, pimpl_->current_status);
    pimpl_->compare_tab->updateState(pimpl_->running, pimpl_->progress, pimpl_->current_status);
    pimpl_->results_tab->updateState(pimpl_->running, pimpl_->progress, pimpl_->current_status);
    pimpl_->history_tab->updateState(pimpl_->running, pimpl_->progress, pimpl_->current_status);
}

// ============================================================================
// Вспомогательные методы
// ============================================================================

void LlamaBenchDialog::updateProfileList() {
    pimpl_->available_profiles.clear();
    pimpl_->profile_selected.clear();

    // Загрузить профили из директории
    auto profiles = llama_gui::bench::BenchCommon::listFilesWithExtension(
        pimpl_->profiles_directory, ".json");

    for (const auto& path : profiles) {
        std::string name = llama_gui::bench::BenchCommon::extractFileNameWithoutExt(path);
        pimpl_->available_profiles.push_back(name);
        pimpl_->profile_selected.push_back(false);
    }
}

void LlamaBenchDialog::refreshResults() {
    // Обновить отображение результатов
}

std::string LlamaBenchDialog::formatDuration(double seconds) const {
    return llama_gui::bench::BenchCommon::formatDuration(seconds);
}

std::string LlamaBenchDialog::formatSpeed(double tps) const {
    return llama_gui::bench::BenchCommon::formatSpeed(tps);
}

std::string LlamaBenchDialog::formatTimeMs(double ms) const {
    return llama_gui::bench::BenchCommon::formatTimeMs(ms);
}

void LlamaBenchDialog::drawColoredText(const char* text, uint32_t color) {
    // Извлечь RGBA из цвета
    float r = ((color >> 0) & 0xFF) / 255.0f;
    float g = ((color >> 8) & 0xFF) / 255.0f;
    float b = ((color >> 16) & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;

    ImGui::TextColored(ImVec4(r, g, b, a), "%s", text);
}

std::string LlamaBenchDialog::findLlamaBenchPath() const {
    // Поиск llama-bench в стандартных местах

    // 1. Родительская директория проекта
    std::string parent_path = "../llama-b7472/llama-bench";
    if (llama_gui::bench::BenchCommon::fileExists(parent_path)) {
        return parent_path;
    }

    // 2. Абсолютный путь (для разработки)
    std::string abs_path = "/home/Alex/projects/llama-b7472-bin-ubuntu-x64/llama-b7472/llama-bench";
    if (llama_gui::bench::BenchCommon::fileExists(abs_path)) {
        return abs_path;
    }

    // 3. PATH
    const char* path_env = std::getenv("PATH");
    if (path_env) {
        std::istringstream iss(path_env);
        std::string path_dir;

        while (std::getline(iss, path_dir, ':')) {
            std::string full_path = path_dir + "/llama-bench";
            if (llama_gui::bench::BenchCommon::fileExists(full_path)) {
                return full_path;
            }
        }
    }

    return "";
}

} // namespace ui
} // namespace llama_gui

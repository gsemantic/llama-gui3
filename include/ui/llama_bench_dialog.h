#pragma once

#include "../external/imgui/imgui.h"
#include "../bench/llama_bench_module.h"
#include "bench_tab_base.h"
#include "run_benchmark_tab.h"
#include "compare_profiles_tab.h"
#include "results_tab.h"
#include "history_tab.h"
#include <string>
#include <vector>
#include <memory>

namespace llama_gui {
namespace core {
    class ServerManager;
}
}

namespace llama_gui {
namespace ui {

/**
 * @class LlamaBenchDialog
 * @brief Диалог для работы с llama-bench
 * 
 * Использует систему вкладок для организации функционала:
 * - RunBenchmarkTab - запуск одиночного теста
 * - CompareProfilesTab - сравнение нескольких профилей
 * - ResultsTab - просмотр и экспорт результатов
 * - HistoryTab - история запусков
 */
class LlamaBenchDialog {
public:
    /**
     * @brief Конструктор
     */
    LlamaBenchDialog();
    
    /**
     * @brief Деструктор
     */
    ~LlamaBenchDialog();

    // =========================================================================
    // Управление видимостью
    // =========================================================================
    
    /**
     * @brief Установить видимость
     */
    void setVisible(bool visible);
    
    /**
     * @brief Проверить видимость
     */
    bool isVisible() const;
    
    /**
     * @brief Переключить видимость
     */
    void toggle();

    // =========================================================================
    // Рендеринг
    // =========================================================================
    
    /**
     * @brief Рендеринг диалога
     */
    void render();

    // =========================================================================
    // Инициализация
    // =========================================================================
    
    /**
     * @brief Инициализировать модуль llama-bench
     * @param llama_bench_path Путь к llama-bench
     * @param profiles_dir Директория с профилями
     * @return true если успешно
     */
    bool initialize(const std::string& llama_bench_path,
                   const std::string& profiles_dir = "profiles");
    
    /**
     * @brief Завершить работу
     */
    void shutdown();
    
    /**
     * @brief Установить server manager для остановки/запуска
     */
    void setServerManager(std::shared_ptr<llama_gui::core::ServerManager> server_manager);

    // =========================================================================
    // Сброс состояния
    // =========================================================================
    
    /**
     * @brief Сбросить состояние диалога
     */
    void reset();

private:
    /**
     * @brief Внутренняя реализация (pimpl)
     */
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

    // =========================================================================
    // Обновление состояния
    // =========================================================================

    void updateTabState();

    // =========================================================================
    // Управление сервером
    // =========================================================================

    bool stopServerForBenchmark();
    bool restartServerAfterBenchmark();

    // =========================================================================
    // Вспомогательные методы
    // =========================================================================

    void updateProfileList();
    void refreshResults();
    std::string formatDuration(double seconds) const;
    std::string formatSpeed(double tps) const;
    std::string formatTimeMs(double ms) const;
    void drawColoredText(const char* text, uint32_t color);

    // Получить путь к llama-bench из окружения проекта
    std::string findLlamaBenchPath() const;
};

} // namespace ui
} // namespace llama_gui

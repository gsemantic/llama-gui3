#pragma once

#include "../external/imgui/imgui.h"
#include <string>
#include <vector>

namespace llama_gui {
namespace ui {

/**
 * @class BenchTabBase
 * @brief Базовый класс для всех вкладок в LlamaBenchDialog
 *
 * Предоставляет общие методы и интерфейс для вкладок.
 */
class BenchTabBase {
public:
    BenchTabBase() = default;
    virtual ~BenchTabBase() = default;

    // Запрет копирования
    BenchTabBase(const BenchTabBase&) = delete;
    BenchTabBase& operator=(const BenchTabBase&) = delete;

    /**
     * @brief Рендеринг вкладки
     */
    virtual void render() = 0;

    /**
     * @brief Получить заголовок вкладки
     */
    virtual const char* getTitle() const = 0;

    /**
     * @brief Получить иконку вкладки
     */
    virtual const char* getIcon() const = 0;

    /**
     * @brief Обновить состояние вкладки
     * @param running Статус выполнения теста
     * @param progress Прогресс выполнения (0-100)
     * @param currentStatus Текущий статус
     */
    virtual void updateState(bool running, int progress, const std::string& current_status) {
        running_ = running;
        progress_ = progress;
        current_status_ = current_status;
    }

protected:
    /**
     * @brief Получить статус выполнения теста
     */
    bool isRunning() const { return running_; }

    /**
     * @brief Получить прогресс выполнения теста
     */
    int getProgress() const { return progress_; }

    /**
     * @brief Получить текущий статус выполнения
     */
    const std::string& getCurrentStatus() const { return current_status_; }

    /**
     * @brief Проверить, видна ли вкладка
     */
    bool isVisible() const { return visible_; }

    /**
     * @brief Установить видимость вкладки
     */
    void setVisible(bool visible) { visible_ = visible; }

    /**
     * @brief Отрисовать компонент с отступами
     */
    void renderWithSpacing() {
        ImGui::Spacing();
    }

    /**
     * @brief Отрисовать разделитель
     */
    void renderSeparator() {
        ImGui::Separator();
    }

    /**
     * @brief Отрисовать информационное сообщение
     */
    void renderInfoMessage(const char* text) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "ⓘ %s", text);
    }

    /**
     * @brief Отрисовать предупреждение
     */
    void renderWarningMessage(const char* text) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "⚠️ %s", text);
    }

private:
    bool running_ = false;
    int progress_ = 0;
    std::string current_status_;
    bool visible_ = true;
};

} // namespace ui
} // namespace llama_gui

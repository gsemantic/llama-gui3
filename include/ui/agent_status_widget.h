#pragma once

/**
 * @file agent_status_widget.h
 * @brief Виджет отображения статуса агентов
 */

#include <agents/agents.h>
#include <string>
#include <vector>

namespace llama_gui {
namespace ui {

/**
 * @brief Виджет для отображения статуса агентов в главном окне
 * 
 * Показывает быстрые индикаторы состояния агентов.
 */
class AgentStatusWidget {
public:
    AgentStatusWidget();
    ~AgentStatusWidget();

    /**
     * @brief Инициализация виджета
     * @param registry Реестр агентов
     */
    void initialize(agents::AgentRegistry* registry);

    /**
     * @brief Отрисовка виджета
     */
    void render();

    /**
     * @brief Обновление статуса
     */
    void update();

    /**
     * @brief Установка активного агента
     * @param name Имя агента
     */
    void set_active_agent(const std::string& name);

    /**
     * @brief Сброс активного агента
     */
    void clear_active_agent();

    /**
     * @brief Получение количества активных агентов
     */
    int get_active_count() const;

    /**
     * @brief Получение общего количества агентов
     */
    int get_total_count() const;

private:
    /**
     * @brief Отрисовка индикатора агента
     */
    void render_agent_indicator(const std::string& name, bool active, bool ready);

    agents::AgentRegistry* registry_ = nullptr;
    
    std::string active_agent_;
    int active_count_ = 0;
    int total_count_ = 0;
};

} // namespace ui
} // namespace llama_gui

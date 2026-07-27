#pragma once

/**
 * @file agent_panel.h
 * @brief Панель управления агентами в UI
 */

#include <agents/agents.h>
#include <string>
#include <vector>
#include <functional>
#include "imgui.h"

namespace llama_gui {
namespace ui {

/**
 * @brief Статус агента для отображения
 */
enum class AgentStatusUI {
    Unknown,      ///< Статус неизвестен
    Ready,        ///< Готов к работе
    Busy,         ///< Занят выполнением
    Disabled,     ///< Отключён
    Error         ///< Ошибка
};

/**
 * @brief Информация об агенте для UI
 */
struct AgentInfoUI {
    std::string name;
    std::string description;
    std::string version;
    AgentStatusUI status;
    bool is_builtin;
    bool is_enabled;
    float progress;  ///< Прогресс выполнения (0-1)
};

/**
 * @brief Панель управления агентами
 * 
 * Отображает список агентов, их статус и позволяет управлять ими.
 */
class AgentPanel {
public:
    AgentPanel();
    ~AgentPanel();

    /**
     * @brief Инициализация панели
     * @param registry Реестр агентов
     * @return true если успешно
     */
    bool initialize(agents::AgentRegistry* registry);

    /**
     * @brief Отрисовка панели
     * @param visible Флаг видимости
     */
    void render(bool* visible);

    /**
     * @brief Обновление списка агентов
     */
    void refresh_agents();

    /**
     * @brief Включение/отключение агента
     * @param name Имя агента
     * @param enabled Состояние
     */
    void set_agent_enabled(const std::string& name, bool enabled);

    /**
     * @brief Получение информации об агенте
     * @param name Имя агента
     * @return Информация
     */
    AgentInfoUI get_agent_info(const std::string& name) const;

    /**
     * @brief Установка колбэка на выбор агента
     * @param callback Функция обратного вызова
     */
    void set_on_agent_selected(std::function<void(const std::string&)> callback);

    /**
     * @brief Установка колбэка на действие агента
     * @param callback Функция обратного вызова
     */
    void set_on_agent_action(std::function<void(const std::string&, const std::string&)> callback);

    /**
     * @brief Проверка видимости панели
     */
    bool is_visible() const;

    /**
     * @brief Показать/скрыть панель
     */
    void set_visible(bool visible);

private:
    /**
     * @brief Отрисовка элемента агента
     */
    void render_agent_item(const AgentInfoUI& info);

    /**
     * @brief Отрисовка панели настроек агента
     */
    void render_agent_settings(const AgentInfoUI& info);

    /**
     * @brief Отрисовка панели статистики
     */
    void render_statistics();

    /**
     * @brief Преобразование статуса в строку
     */
    const char* status_to_string(AgentStatusUI status) const;

    /**
     * @brief Преобразование статуса в цвет
     */
    void get_status_color(AgentStatusUI status, float* r, float* g, float* b) const;

    agents::AgentRegistry* registry_ = nullptr;
    std::vector<AgentInfoUI> agents_;

    bool visible_ = false;
    bool show_statistics_ = false;

    std::string selected_agent_;
    
    // Колбэки
    std::function<void(const std::string&)> on_agent_selected_;
    std::function<void(const std::string&, const std::string&)> on_agent_action_;
    
    // Статистика
    int total_agents_ = 0;
    int enabled_agents_ = 0;
    int disabled_agents_ = 0;
};

} // namespace ui
} // namespace llama_gui

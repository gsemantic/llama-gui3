#pragma once

#include "i_agent.h"
#include "agent_result.h"
#include "agent_request.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>

namespace agents {

class AgentContext;

/**
 * @brief Информация о зарегистрированном агенте
 */
struct AgentInfo {
    std::string name;
    std::string description;
    std::string version;
    AgentCapability capabilities;
    bool is_builtin;      ///< Встроенный или плагин
    bool is_enabled;      ///< Включён ли агент
    std::string path;     ///< Путь к файлу плагина (для плагинов)
};

/**
 * @brief Реестр агентов
 * 
 * Управляет регистрацией, поиском и выполнением агентов.
 * Потокобезопасен.
 */
class AgentRegistry {
public:
    AgentRegistry();
    ~AgentRegistry();

    /**
     * @brief Регистрация агента
     * @param agent Указатель на агент
     * @param is_builtin true если агент встроенный
     * @param path Путь к файлу (для плагинов)
     * @return true если регистрация успешна
     */
    bool register_agent(std::unique_ptr<IAgent> agent, 
                        bool is_builtin = false,
                        const std::string& path = "");

    /**
     * @brief Удаление агента из реестра
     * @param name Имя агента
     * @return true если агент удалён
     */
    bool unregister_agent(const std::string& name);

    /**
     * @brief Получение агента по имени
     * @param name Имя агента
     * @return Указатель на агент или nullptr
     */
    IAgent* get_agent(const std::string& name);

    /**
     * @brief Проверка наличия агента
     * @param name Имя агента
     * @return true если агент зарегистрирован
     */
    bool has_agent(const std::string& name) const;

    /**
     * @brief Выполнение запроса к агенту
     * @param request Запрос
     * @return Результат выполнения
     */
    AgentResult execute(const AgentRequest& request);

    /**
     * @brief Выполнение запроса к агенту по имени
     * @param agent_name Имя агента
     * @param action Действие
     * @param params Параметры
     * @return Результат выполнения
     */
    AgentResult execute(const std::string& agent_name,
                        const std::string& action,
                        const nlohmann::json& params = {});

    /**
     * @brief Список всех зарегистрированных агентов
     * @return Вектор с информацией об агентах
     */
    std::vector<AgentInfo> list_agents() const;

    /**
     * @brief Получение информации об агенте
     * @param name Имя агента
     * @return Информация об агенте или nullptr
     */
    const AgentInfo* get_agent_info(const std::string& name) const;

    /**
     * @brief Поиск агентов по возможностям
     * @param capability Требуемая возможность
     * @return Список агентов с этой возможностью
     */
    std::vector<AgentInfo> find_by_capability(AgentCapability capability) const;

    /**
     * @brief Включение/отключение агента
     * @param name Имя агента
     * @param enabled Состояние
     * @return true если успешно
     */
    bool set_agent_enabled(const std::string& name, bool enabled);

    /**
     * @brief Проверка включённости агента
     */
    bool is_agent_enabled(const std::string& name) const;

    /**
     * @brief Инициализация всех агентов
     * @param context Контекст выполнения
     * @return true если все агенты инициализированы
     */
    bool initialize_all(AgentContext* context);

    /**
     * @brief Остановка всех агентов
     */
    void shutdown_all();

    /**
     * @brief Получение контекста
     */
    AgentContext* get_context() const;

    /**
     * @brief Установка контекста
     */
    void set_context(AgentContext* context);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agents

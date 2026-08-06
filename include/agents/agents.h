#pragma once

/**
 * @file agents.h
 * @brief Главный заголовочный файл системы агентов
 * 
 * Подключает все необходимые компоненты для работы с агентами
 */

// Базовые типы и интерфейсы
#include "agent_types.h"
#include "i_agent.h"

// Запросы и результаты
#include "agent_request.h"
#include "agent_result.h"

// Возможности агентов
#include "agent_capabilities.h"

// Контекст и реестр
#include "agent_context.h"
#include "agent_registry.h"

// Загрузка плагинов
#include "plugin_loader.h"
#include "plugin_c_api.h"

/**
 * @namespace agents
 * @brief Система агентов с подключаемыми плагинами
 * 
 * @section overview Обзор
 * 
 * Система агентов позволяет расширять функциональность приложения
 * через плагины. Каждый агент реализует интерфейс IAgent и может
 * быть загружен динамически.
 * 
 * @section architecture Архитектура
 * 
 * - IAgent - базовый интерфейс всех агентов
 * - AgentRegistry - реестр для регистрации и поиска агентов
 * - AgentContext - контекст выполнения с доступом к сервисам
 * - PluginLoader - загрузчик плагинов из .so/.dll файлов
 * 
 * @section usage Пример использования
 * 
 * @code
 * #include <agents/agents.h>
 * 
 * using namespace agents;
 * 
 * // Создаём реестр и контекст
 * AgentRegistry registry;
 * AgentContext context;
 * 
 * // Регистрируем агента
 * auto agent = std::make_unique<MyAgent>();
 * registry.register_agent(std::move(agent));
 * 
 * // Инициализируем все агенты
 * registry.initialize_all(&context);
 * 
 * // Выполняем запрос
 * AgentRequest request("my_agent", "action", {{"param", "value"}});
 * AgentResult result = registry.execute(request);
 * 
 * if (result.is_ok()) {
 *     auto data = result.data();
 *     // ... обработка результата ...
 * }
 * @endcode
 * 
 * @section plugins Создание плагинов
 * 
 * Плагины компилируются как разделяемые библиотеки и экспортируют
 * функции через C-API (plugin_c_api.h).
 * 
 * @see plugin_c_api.h для деталей о создании плагинов
 */
namespace agents {

/**
 * @brief Версия системы агентов
 */
constexpr const char* VERSION = "1.0.0";
constexpr int VERSION_MAJOR = 1;
constexpr int VERSION_MINOR = 0;
constexpr int VERSION_PATCH = 0;

} // namespace agents

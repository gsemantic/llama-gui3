#pragma once

#include "agent_types.h"
#include <stdint.h>

/**
 * @file plugin_c_api.h
 * @brief C-API для динамической загрузки плагинов агентов
 * 
 * Этот заголовок определяет C-интерфейс для создания плагинов.
 * Плагины компилируются как разделяемые библиотеки (.so/.dll)
 * и экспортируют функции через extern "C".
 * 
 * Пример использования в плагине:
 * 
 * @code
 * extern "C" {
 *     AGENT_PLUGIN_EXPORT const char* plugin_get_name() {
 *         return "my_agent";
 *     }
 *     
 *     AGENT_PLUGIN_EXPORT IAgent* plugin_create_agent() {
 *         return new MyAgent();
 *     }
 * }
 * @endcode
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Версия API плагинов
 * 
 * Плагины должны проверять совместимость версии
 */
#define AGENT_PLUGIN_API_VERSION "1.0.0"
#define AGENT_PLUGIN_API_VERSION_MAJOR 1
#define AGENT_PLUGIN_API_VERSION_MINOR 0
#define AGENT_PLUGIN_API_VERSION_PATCH 0

/**
 * @brief Макрос для экспорта функций из плагина
 */
#if defined(_WIN32) || defined(_WIN64)
    #define AGENT_PLUGIN_EXPORT __declspec(dllexport)
    #define AGENT_PLUGIN_IMPORT __declspec(dllimport)
#else
    #define AGENT_PLUGIN_EXPORT __attribute__((visibility("default")))
    #define AGENT_PLUGIN_IMPORT
#endif

/**
 * @brief Forward declaration агента (непрозрачный указатель для C)
 */
typedef struct AgentHandle AgentHandle;

/**
 * @brief Forward declaration контекста (непрозрачный указатель для C)
 */
typedef struct AgentContextHandle AgentContextHandle;

/**
 * @brief Forward declaration результата (непрозрачный указатель для C)
 */
typedef struct AgentResultHandle AgentResultHandle;

/**
 * @brief Получить имя плагина
 * @return Имя плагина (NULL-terminated string)
 * 
 * Плагин должен реализовать эту функцию
 */
typedef const char* (*plugin_get_name_fn)(void);

/**
 * @brief Получить версию плагина
 * @return Версию в формате semver
 * 
 * Плагин должен реализовать эту функцию
 */
typedef const char* (*plugin_get_version_fn)(void);

/**
 * @brief Получить API версию с которой совместим плагин
 * @return Версию API агентов
 */
typedef const char* (*plugin_get_api_version_fn)(void);

/**
 * @brief Создать экземпляр агента
 * @return Указатель на агент или NULL при ошибке
 * 
 * Плагин должен реализовать эту функцию
 */
typedef AgentHandle* (*plugin_create_agent_fn)(void);

/**
 * @brief Уничтожить экземпляр агента
 * @param agent Указатель на агент
 * 
 * Плагин должен реализовать эту функцию
 */
typedef void (*plugin_destroy_agent_fn)(AgentHandle* agent);

/**
 * @brief Инициализировать плагин
 * @param context Контекст выполнения
 * @return 1 если успешно, 0 при ошибке
 */
typedef int (*plugin_initialize_fn)(AgentContextHandle* context);

/**
 * @brief Остановить плагин и освободить ресурсы
 */
typedef void (*plugin_shutdown_fn)(void);

/**
 * @brief Структура экспортируемых функций плагина
 */
typedef struct {
    const char* api_version;
    plugin_get_name_fn get_name;
    plugin_get_version_fn get_version;
    plugin_get_api_version_fn get_api_version;
    plugin_create_agent_fn create_agent;
    plugin_destroy_agent_fn destroy_agent;
    plugin_initialize_fn initialize;
    plugin_shutdown_fn shutdown;
} PluginExports;

/**
 * @brief Получить экспортируемые функции плагина
 * @return Структура с функциями
 * 
 * Эта функция должна быть экспортирована из каждого плагина
 */
typedef PluginExports* (*plugin_get_exports_fn)(void);

#ifdef __cplusplus
}
#endif

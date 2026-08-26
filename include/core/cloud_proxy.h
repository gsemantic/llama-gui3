#pragma once

#include <string>
#include <atomic>
#include <thread>
#include "core/settings.h"

namespace llama_gui {
namespace core {

/**
 * @brief Настройки облачного прокси-режима
 */
struct CloudProxyOptions {
    std::string host;         // пусто = из настроек server_runtime
    int port = 0;             // 0 = из настроек server_runtime
    bool auto_port = false;   // автоматически найти свободный порт
    std::string endpoint_url; // переопределить endpoint провайдера (для тестов/других провайдеров)
    std::string api_key;      // переопределить API-ключ (иначе из .env профиля)
    bool manage_curl = true;  // вызывать curl_global_init/cleanup (false при встраивании в GUI)
    // GUI-режим (--proxy): если порт прокси совпадает с портом локального
    // чат-сервера (settings.server().port), сдвинуть прокси на свободный порт.
    // Без этого оба сервера занимают один порт (по разные семейства адресов),
    // и health-проверки GUI попадают в прокси вместо llama-server.
    bool avoid_local_server_port = false;
};

/**
 * @brief Запуск облачного прокси (без GUI)
 *
 * Поднимает OpenAI-совместимый endpoint (localhost), который пересылает
 * запросы в облачного провайдера, настроенного в профиле (endpoint_url,
 * model_id, API-ключ из .env). При включённом RAG запрос дополняется
 * локальным контекстом (поиск и эмбеддинги — локальные; в облако уходит
 * уже дополненное сообщение).
 *
 * Приоритет параметров генерации: запрос клиента > настройки приложения >
 * дефолты провайдера.
 *
 * @param settings Настройки приложения (профиль уже загружен)
 * @param opts Опции прокси (host/port/auto-port)
 * @return 0 при успешном завершении, иначе код ошибки
 */
int run_cloud_proxy(Settings& settings, const CloudProxyOptions& opts);

/**
 * @brief Запуск облачного прокси в отдельном потоке (для GUI-режима).
 *
 * Прокси использует ту же ссылку на Settings, что и GUI: при каждом запросе
 * endpoint_url, api_key и параметры RAG читаются из актуального состояния
 * настроек, поэтому смена облачного провайдера (или включение RAG) в GUI
 * подхватывается без перезапуска прокси. Порт берётся из server_runtime.
 *
 * @param settings Живые настройки приложения (GUI)
 * @param opts Опции прокси (пустой host/port = из настроек)
 * @param stop_flag Атомарный флаг остановки (устанавливает вызывающий код)
 * @return std::thread Фоновый поток прокси (детектированный; остановка - флагом)
 */
std::thread start_cloud_proxy_in_thread(Settings& settings,
                                        const CloudProxyOptions& opts,
                                        std::atomic<bool>& stop_flag);

} // namespace core
} // namespace llama_gui

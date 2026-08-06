#pragma once

#include "i_agent.h"
#include "agent_registry.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>

namespace agents {

/**
 * @brief Информация о загруженном плагине
 */
struct PluginInfo {
    std::string name;
    std::string version;
    std::string api_version;
    std::string path;
    std::string description;
    bool is_loaded;
    void* handle;  // Дескриптор библиотеки
};

/**
 * @brief Загрузчик плагинов
 * 
 * Управляет динамической загрузкой и выгрузкой плагинов.
 * Поддерживает горячую перезагрузку.
 */
class PluginLoader {
public:
    PluginLoader();
    ~PluginLoader();

    /**
     * @brief Загрузка плагина из файла
     * @param path Путь к файлу плагина (.so или .dll)
     * @return true если загрузка успешна
     */
    bool load_plugin(const std::string& path);

    /**
     * @brief Выгрузка плагина
     * @param name Имя плагина
     * @return true если плагин выгружен
     */
    bool unload_plugin(const std::string& name);

    /**
     * @brief Выгрузка всех плагинов
     */
    void unload_all();

    /**
     * @brief Проверка загружен ли плагин
     * @param name Имя плагина
     * @return true если плагин загружен
     */
    bool is_plugin_loaded(const std::string& name) const;

    /**
     * @brief Получение информации о плагине
     * @param name Имя плагина
     * @return Информация о плагине или nullptr
     */
    const PluginInfo* get_plugin_info(const std::string& name) const;

    /**
     * @brief Список всех загруженных плагинов
     * @return Вектор с информацией о плагинах
     */
    std::vector<PluginInfo> list_plugins() const;

    /**
     * @brief Создание агента из плагина
     * @param plugin_name Имя плагина
     * @param registry Реестр для регистрации агента
     * @return true если агент создан и зарегистрирован
     */
    bool create_agent_from_plugin(const std::string& plugin_name,
                                   AgentRegistry* registry);

    /**
     * @brief Сканирование директории на наличие плагинов
     * @param dir Директория для сканирования
     * @return Список найденных файлов плагинов
     */
    std::vector<std::string> scan_directory(const std::string& dir);

    /**
     * @brief Загрузка всех плагинов из директории
     * @param dir Директория с плагинами
     * @param registry Реестр для регистрации агентов
     * @return Количество успешно загруженных плагинов
     */
    int load_plugins_from_directory(const std::string& dir,
                                     AgentRegistry* registry);

    /**
     * @brief Получение расширения для файлов плагинов
     */
    static std::string get_plugin_extension();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agents

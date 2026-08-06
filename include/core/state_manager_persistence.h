#pragma once

#include "state_manager.h"
#include <string>

namespace llama_gui {
namespace core {

/**
 * @brief Сохранение и загрузка состояния
 * 
 * Этот модуль содержит все операции сериализации:
 * - Сохранение в файл
 * - Загрузка из файла
 * - Сериализация в JSON
 * - Десериализация из JSON
 */
class StateManagerPersistence {
public:
    StateManagerPersistence(StateManager& state_manager);
    ~StateManagerPersistence() = default;

    // Запрет копирования
    StateManagerPersistence(const StateManagerPersistence&) = delete;
    StateManagerPersistence& operator=(const StateManagerPersistence&) = delete;

    // Сериализация
    bool save_to_file(const std::string& file_path);
    bool load_from_file(const std::string& file_path);
    std::string serialize_to_json() const;
    bool deserialize_from_json(const std::string& json_data);

    // Утилиты
    std::string generate_id() const;
    void clear_all_data();

private:
    StateManager& state_manager_;
};

} // namespace core
} // namespace llama_gui

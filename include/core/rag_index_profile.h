#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <filesystem>

namespace llama_gui {
namespace core {

/**
 * @brief Профиль RAG индекса
 * 
 * Позволяет пользователю создавать отдельные индексы для разных папок/проектов.
 * Например: "велосипеды", "самокаты", "документация" и т.д.
 */
struct RagIndexProfile {
    std::string name;                    // Имя профиля (например, "велосипеды")
    std::string index_path;              // Путь к файлу индекса FAISS
    std::string metadata_path;           // Путь к файлу метаданных JSON
    std::string source_directory;        // Исходная директория с документами
    std::vector<std::string> documents;  // Список документов в профиле
    int chunk_count;                     // Количество чанков в индексе
    int64_t created_at;                  // Время создания
    int64_t modified_at;                 // Время последнего изменения

    // Module/directory structure for hierarchical organization
    struct Module {
        std::string name;                // "src/core", "plugins/official"
        std::string path;                // Относительный путь от source_directory
        std::vector<std::string> files;  // Файлы в модуле
        int chunk_count = 0;
    };
    std::vector<Module> modules;         // Модули внутри профиля

    RagIndexProfile() 
        : chunk_count(0), created_at(0), modified_at(0) {}

    RagIndexProfile(const std::string& profile_name)
        : name(profile_name), chunk_count(0), created_at(0), modified_at(0) {}
};

/**
 * @brief Менеджер профилей RAG индексов
 * 
 * Управляет созданием, загрузкой, переключением и удалением профилей индексов.
 * Профили хранятся в ~/.llama-gui/rag_profiles/
 */
class RagIndexProfileManager {
public:
    RagIndexProfileManager();
    ~RagIndexProfileManager();

    /**
     * @brief Инициализировать менеджер профилей
     * @param profiles_directory Директория для хранения профилей (по умолчанию ~/.llama-gui/rag_profiles/)
     * @return true если успешно
     */
    bool initialize(const std::string& profiles_directory = "");

    /**
     * @brief Получить список всех профилей
     * @return Вектор с именами профилей
     */
    std::vector<std::string> get_profile_names() const;

    /**
     * @brief Получить информацию о профиле
     * @param profile_name Имя профиля
     * @return Профиль или nullptr если не найден
     */
    const RagIndexProfile* get_profile(const std::string& profile_name) const;

    /**
     * @brief Получить текущий активный профиль
     * @return Имя текущего профиля или пустая строка
     */
    std::string get_current_profile() const;

    /**
     * @brief Установить текущий профиль
     * @param profile_name Имя профиля
     * @return true если успешно
     */
    bool set_current_profile(const std::string& profile_name);

    /**
     * @brief Создать новый профиль
     * @param profile_name Имя профиля
     * @param source_directory Исходная директория с документами (опционально)
     * @return true если успешно
     */
    bool create_profile(const std::string& profile_name, 
                       const std::string& source_directory = "");

    /**
     * @brief Удалить профиль
     * @param profile_name Имя профиля
     * @param delete_index_file Удалить также файл индекса
     * @return true если успешно
     */
    bool delete_profile(const std::string& profile_name, bool delete_index_file = false);

    /**
     * @brief Сохранить профиль
     * @param profile Профиль для сохранения
     * @return true если успешно
     */
    bool save_profile(const RagIndexProfile& profile);

    /**
     * @brief Загрузить профиль
     * @param profile_name Имя профиля
     * @return Загруженный профиль или nullptr если не найден
     */
    RagIndexProfile* load_profile(const std::string& profile_name);

    /**
     * @brief Получить путь к индексу для текущего профиля
     * @return Путь к файлу индекса
     */
    std::string get_current_index_path() const;

    /**
     * @brief Получить путь к метаданным для текущего профиля
     * @return Путь к файлу метаданных
     */
    std::string get_current_metadata_path() const;

    /**
     * @brief Проверить существование профиля
     * @param profile_name Имя профиля
     * @return true если профиль существует
     */
    bool has_profile(const std::string& profile_name) const;

    /**
     * @brief Добавить документ в текущий профиль
     * @param document_path Путь к документу
     */
    void add_document_to_current_profile(const std::string& document_path);

    /**
     * @brief Обновить количество чанков в текущем профиле
     * @param chunk_count Количество чанков
     */
    void update_current_profile_chunk_count(int chunk_count);

    // Module management
    void add_module_to_current_profile(const std::string& module_path, const std::string& file_path);
    std::vector<RagIndexProfile::Module> get_current_profile_modules() const;

private:
    std::string profiles_directory_;  // Директория для хранения профилей
    std::string current_profile_;     // Текущий активный профиль
    std::unordered_map<std::string, RagIndexProfile> profiles_;  // Все профили

    /**
     * @brief Загрузить все профили из директории
     */
    void load_all_profiles();

    /**
     * @brief Сохранить конфигурацию менеджера (текущий профиль)
     */
    void save_manager_config() const;

    /**
     * @brief Загрузить конфигурацию менеджера
     */
    void load_manager_config();

    /**
     * @brief Получить путь к конфигурационному файлу менеджера
     */
    std::string get_config_path() const;

    /**
     * @brief Получить путь к файлу профиля
     */
    std::string get_profile_config_path(const std::string& profile_name) const;
};

} // namespace core
} // namespace llama_gui

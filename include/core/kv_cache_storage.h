#pragma once

#include "kv_cache_types.h"
#include <string>
#include <vector>
#include <memory>

namespace llama_gui {
namespace core {

/**
 * @brief Управление файловым хранилищем KV-cache
 *
 * Отвечает за:
 * - Генерацию имён файлов
 * - Управление директорией хранения
 * - Очистку старых файлов
 * - Проверку целостности
 */
class KVCacheStorage {
public:
    /**
     * @brief Конструктор хранилища
     * @param base_path Базовый путь для хранения файлов
     */
    explicit KVCacheStorage(const std::string& base_path);

    /**
     * @brief Деструктор
     */
    ~KVCacheStorage();

    // =========================================================================
    // Пути к файлам
    // =========================================================================

    /**
     * @brief Получить путь к файлу KV-cache для документа
     * @param doc_id ID документа
     * @return Полный путь к файлу
     */
    std::string get_cache_path(const std::string& doc_id) const;

    /**
     * @brief Получить путь по имени файла
     * @param filename Имя файла
     * @return Полный путь
     */
    std::string get_full_path(const std::string& filename) const;

    /**
     * @brief Проверить наличие KV-cache для документа
     * @param doc_id ID документа
     * @return true если файл существует
     */
    bool has_cache(const std::string& doc_id) const;

    // =========================================================================
    // Операции с файлами
    // =========================================================================

    /**
     * @brief Удалить KV-cache документа
     * @param doc_id ID документа
     * @return true если успешно
     */
    bool delete_cache(const std::string& doc_id);

    /**
     * @brief Получить размер файла KV-cache
     * @param doc_id ID документа
     * @return Размер в байтах или 0 если файл не найден
     */
    size_t get_cache_size(const std::string& doc_id) const;

    /**
     * @brief Получить hash файла для проверки целостности
     * @param doc_id ID документа
     * @return MD5 hash или пустая строка
     */
    std::string get_cache_hash(const std::string& doc_id) const;

    // =========================================================================
    // Очистка
    // =========================================================================

    /**
     * @brief Удалить старые файлы KV-cache
     * @param older_than_seconds Удалить файлы старше указанного времени
     * @return Количество удалённых файлов
     */
    int cleanup_old_caches(int older_than_seconds);

    /**
     * @brief Удалить все файлы KV-cache
     * @return Количество удалённых файлов
     */
    int clear_all_caches();

    /**
     * @brief Получить список всех сохранённых документов
     * @return Вектор ID документов
     */
    std::vector<std::string> get_all_cached_documents() const;

    // =========================================================================
    // Информация
    // =========================================================================

    /**
     * @brief Получить общий размер хранилища
     * @return Размер в байтах
     */
    size_t get_total_storage_size() const;

    /**
     * @brief Получить количество файлов в хранилище
     * @return Количество файлов
     */
    int get_file_count() const;

    /**
     * @brief Получить базовый путь хранилища
     * @return Базовый путь
     */
    std::string get_base_path() const;

    // =========================================================================
    // Валидация
    // =========================================================================

    /**
     * @brief Проверить доступность хранилища для записи
     * @return true если хранилище доступно
     */
    bool is_writable() const;

    /**
     * @brief Получить доступное место в хранилище
     * @return Доступное место в байтах
     */
    size_t get_available_space() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;

    std::string base_path_;
};

} // namespace core
} // namespace llama_gui

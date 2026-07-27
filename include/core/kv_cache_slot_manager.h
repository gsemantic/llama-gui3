#pragma once

#include "kv_cache_types.h"
#include <memory>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace llama_gui {
namespace core {

class KVCacheAPI;
class KVCacheStorage;

/**
 * @brief Менеджер слотов KV-cache
 *
 * Управляет жизненным циклом слотов:
 * - Выделение слотов для документов
 * - Отслеживание состояния слотов
 * - Автоматическое переиспользование
 */
class KVCacheSlotManager {
public:
    /**
     * @brief Конструктор менеджера слотов
     * @param api API для работы с KV-cache
     * @param storage Хранилище KV-cache
     */
    KVCacheSlotManager(std::shared_ptr<KVCacheAPI> api,
                       std::shared_ptr<KVCacheStorage> storage);

    /**
     * @brief Деструктор
     */
    ~KVCacheSlotManager();

    // =========================================================================
    // Выделение слотов
    // =========================================================================

    /**
     * @brief Выделить слот для документа
     * @param doc_id ID документа
     * @return ID выделенного слота или -1 если нет доступных
     */
    int allocate_slot(const std::string& doc_id);

    /**
     * @brief Освободить слот
     * @param slot_id ID слота
     * @return true если успешно
     */
    bool release_slot(int slot_id);

    /**
     * @brief Принудительно сбросить слот
     * @param slot_id ID слота
     * @return true если успешно
     */
    bool force_reset_slot(int slot_id);

    // =========================================================================
    // Состояние слотов
    // =========================================================================

    /**
     * @brief Получить документ, загруженный в слот
     * @param slot_id ID слота
     * @return ID документа или пустая строка
     */
    std::string get_slot_document(int slot_id) const;

    /**
     * @brief Проверить занятость слота
     * @param slot_id ID слота
     * @return true если слот занят
     */
    bool is_slot_busy(int slot_id) const;

    /**
     * @brief Получить количество доступных слотов
     * @return Количество свободных слотов
     */
    int get_available_slots_count() const;

    /**
     * @brief Получить общее количество слотов
     * @return Общее количество
     */
    int get_total_slots_count() const;

    // =========================================================================
    // Оптимизация
    // =========================================================================

    /**
     * @brief Найти слот с загруженным KV-cache для документа
     * @param doc_id ID документа
     * @return ID слота или -1 если не найден
     */
    int find_slot_with_cache(const std::string& doc_id) const;

    /**
     * @brief Очистить неактивные слоты
     * @param idle_timeout_seconds Таймаут бездействия
     * @return Количество очищенных слотов
     */
    int cleanup_idle_slots(int idle_timeout_seconds);

    // =========================================================================
    // Информация
    // =========================================================================

    /**
     * @brief Получить информацию о всех слотах
     * @return Вектор информации о слотах
     */
    std::vector<SlotInfo> get_all_slots_info() const;

    /**
     * @brief Получить статистику использования слотов
     * @return JSON со статистикой
     */
    std::string get_usage_statistics() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
    mutable std::mutex mutex_;
};

} // namespace core
} // namespace llama_gui

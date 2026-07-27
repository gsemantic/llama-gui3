#pragma once

#include "kv_cache_types.h"
#include <memory>
#include <vector>

namespace llama_gui {
namespace core {

class LlamaInterface;

/**
 * @brief Основной API для работы с KV-cache
 *
 * Предоставляет высокоуровневые операции:
 * - Сохранение/восстановление KV-cache слотов
 * - Управление состоянием слотов
 * - Валидация KV-cache
 */
class KVCacheAPI {
public:
    /**
     * @brief Конструктор API
     * @param llama_interface Интерфейс к llama.cpp серверу
     */
    explicit KVCacheAPI(std::shared_ptr<LlamaInterface> llama_interface);

    /**
     * @brief Деструктор
     */
    ~KVCacheAPI();

    // =========================================================================
    // Основные операции
    // =========================================================================

    /**
     * @brief Сохранить KV-cache слота в файл
     * @param slot_id ID слота
     * @param filename Имя файла
     * @return Результат операции
     */
    KVCacheOperationResult save(int slot_id, const std::string& filename);

    /**
     * @brief Восстановить KV-cache слота из файла
     * @param slot_id ID слота
     * @param filename Имя файла
     * @return Результат операции
     */
    KVCacheOperationResult restore(int slot_id, const std::string& filename);

    /**
     * @brief Сбросить слот
     * @param slot_id ID слота
     * @return true если успешно
     */
    bool reset(int slot_id);

    /**
     * @brief Удалить KV-cache слота
     * @param slot_id ID слота
     * @return true если успешно
     */
    bool erase(int slot_id);

    // =========================================================================
    // Информация о слотах
    // =========================================================================

    /**
     * @brief Получить статус всех слотов
     * @return Вектор информации о слотах
     */
    std::vector<SlotInfo> get_slots_status() const;

    /**
     * @brief Получить статус конкретного слота
     * @param slot_id ID слота
     * @return Информация о слоте или SlotInfo с id=-1 если не найден
     */
    SlotInfo get_slot_status(int slot_id) const;

    /**
     * @brief Найти свободный слот
     * @return ID свободного слота или -1 если нет свободных
     */
    int find_free_slot() const;

    // =========================================================================
    // Валидация
    // =========================================================================

    /**
     * @brief Проверить доступность слота для операции
     * @param slot_id ID слота
     * @return true если слот доступен (Idle)
     */
    bool is_slot_available(int slot_id) const;

    /**
     * @brief Проверить корректность файла KV-cache
     * @param filepath Путь к файлу
     * @return true если файл существует и имеет минимальный размер
     */
    bool validate_cache_file(const std::string& filepath) const;

    // =========================================================================
    // Метрики
    // =========================================================================

    /**
     * @brief Получить текущие метрики
     * @return Копия метрик
     */
    KVCacheMetrics get_metrics() const;

    /**
     * @brief Сбросить метрики
     */
    void reset_metrics();

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace core
} // namespace llama_gui

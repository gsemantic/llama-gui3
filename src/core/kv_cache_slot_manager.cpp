#include "../include/core/kv_cache_slot_manager.h"
#include "../include/core/kv_cache_api.h"
#include "../include/core/kv_cache_storage.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>

namespace llama_gui {
namespace core {

// ============================================================================
// Private Impl Class
// ============================================================================

class KVCacheSlotManager::Impl {
public:
    std::shared_ptr<KVCacheAPI> api;
    std::shared_ptr<KVCacheStorage> storage;
    std::unordered_map<int, std::string> slot_documents;  // slot_id -> doc_id
    int total_slots = 4;  // По умолчанию 4 слота

    Impl(std::shared_ptr<KVCacheAPI> api_ptr, std::shared_ptr<KVCacheStorage> storage_ptr)
        : api(std::move(api_ptr)), storage(std::move(storage_ptr)) {}

    /**
     * @brief Обновить информацию о слотах из API
     */
    void update_slots_info() {
        auto slots = api->get_slots_status();
        total_slots = static_cast<int>(slots.size());
    }
};

// ============================================================================
// Public Methods
// ============================================================================

KVCacheSlotManager::KVCacheSlotManager(
    std::shared_ptr<KVCacheAPI> api,
    std::shared_ptr<KVCacheStorage> storage)
    : pImpl(std::make_unique<Impl>(std::move(api), std::move(storage))) {
    pImpl->update_slots_info();
}

KVCacheSlotManager::~KVCacheSlotManager() = default;

int KVCacheSlotManager::allocate_slot(const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Сначала ищем слот с уже загруженным KV-cache для этого документа
    int cached_slot = find_slot_with_cache(doc_id);
    if (cached_slot >= 0) {
        std::cout << "[KV-SLOT] Reusing slot " << cached_slot
                  << " with cached KV-cache for document " << doc_id << std::endl;
        pImpl->slot_documents[cached_slot] = doc_id;
        return cached_slot;
    }

    // Ищем свободный слот
    int free_slot = pImpl->api->find_free_slot();
    if (free_slot >= 0) {
        std::cout << "[KV-SLOT] Allocated free slot " << free_slot
                  << " for document " << doc_id << std::endl;
        pImpl->slot_documents[free_slot] = doc_id;
        return free_slot;
    }

    std::cerr << "[KV-SLOT] No available slots for document " << doc_id << std::endl;
    return -1;
}

bool KVCacheSlotManager::release_slot(int slot_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pImpl->slot_documents.find(slot_id);
    if (it != pImpl->slot_documents.end()) {
        pImpl->slot_documents.erase(it);
        std::cout << "[KV-SLOT] Released slot " << slot_id << std::endl;
        return true;
    }

    return false;
}

bool KVCacheSlotManager::force_reset_slot(int slot_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    bool success = pImpl->api->reset(slot_id);
    if (success) {
        pImpl->slot_documents.erase(slot_id);
        std::cout << "[KV-SLOT] Force reset slot " << slot_id << std::endl;
    }
    return success;
}

std::string KVCacheSlotManager::get_slot_document(int slot_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pImpl->slot_documents.find(slot_id);
    if (it != pImpl->slot_documents.end()) {
        return it->second;
    }
    return "";
}

bool KVCacheSlotManager::is_slot_busy(int slot_id) const {
    auto slot = pImpl->api->get_slot_status(slot_id);
    return slot.is_busy();
}

int KVCacheSlotManager::get_available_slots_count() const {
    auto slots = pImpl->api->get_slots_status();
    int count = 0;
    for (const auto& slot : slots) {
        if (slot.is_idle()) {
            count++;
        }
    }
    return count;
}

int KVCacheSlotManager::get_total_slots_count() const {
    auto slots = pImpl->api->get_slots_status();
    return static_cast<int>(slots.size());
}

int KVCacheSlotManager::find_slot_with_cache(const std::string& doc_id) const {
    // Проверяем, есть ли слот с уже загруженным KV-cache для этого документа
    auto slots = pImpl->api->get_slots_status();
    for (const auto& slot : slots) {
        if (slot.is_idle() && !slot.loaded_document_id.empty() &&
            slot.loaded_document_id == doc_id) {
            return slot.id;
        }
    }
    return -1;
}

int KVCacheSlotManager::cleanup_idle_slots(int idle_timeout_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);

    int cleaned_count = 0;
    auto now = std::time(nullptr);
    auto slots = pImpl->api->get_slots_status();

    for (const auto& slot : slots) {
        if (slot.is_idle() && slot.last_activity > 0) {
            auto idle_time = std::difftime(now, slot.last_activity);
            if (idle_time > idle_timeout_seconds) {
                if (pImpl->api->reset(slot.id)) {
                    pImpl->slot_documents.erase(slot.id);
                    cleaned_count++;
                    std::cout << "[KV-SLOT] Cleaned up idle slot " << slot.id
                              << " (idle for " << idle_time << "s)" << std::endl;
                }
            }
        }
    }

    return cleaned_count;
}

std::vector<SlotInfo> KVCacheSlotManager::get_all_slots_info() const {
    return pImpl->api->get_slots_status();
}

std::string KVCacheSlotManager::get_usage_statistics() const {
    std::stringstream ss;
    auto slots = pImpl->api->get_slots_status();

    int idle_count = 0;
    int busy_count = 0;

    for (const auto& slot : slots) {
        if (slot.is_idle()) {
            idle_count++;
        } else {
            busy_count++;
        }
    }

    ss << "Slot Usage Statistics:\n";
    ss << "  Total slots: " << slots.size() << "\n";
    ss << "  Idle slots: " << idle_count << "\n";
    ss << "  Busy slots: " << busy_count << "\n";
    ss << "  Usage: " << (slots.size() > 0 ? (busy_count * 100 / slots.size()) : 0) << "%\n";

    return ss.str();
}

} // namespace core
} // namespace llama_gui

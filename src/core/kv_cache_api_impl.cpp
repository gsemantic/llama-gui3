#include "../include/core/kv_cache_api.h"
#include "../include/core/llama_interface.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

using json = nlohmann::json;

namespace llama_gui {
namespace core {

// ============================================================================
// Private Impl Class
// ============================================================================

class KVCacheAPI::Impl {
public:
    std::shared_ptr<LlamaInterface> llama_interface;
    KVCacheMetrics metrics;

    explicit Impl(std::shared_ptr<LlamaInterface> interface)
        : llama_interface(std::move(interface)) {}

    /**
     * @brief Преобразовать JSON ответ в результат операции
     */
    KVCacheOperationResult convert_to_result(
        const json& response, int slot_id,
        const std::string& filename, bool is_save);

    /**
     * @brief Обновить метрики после сохранения
     */
    void update_save_metrics(const KVCacheOperationResult& result);

    /**
     * @brief Обновить метрики после восстановления
     */
    void update_restore_metrics(const KVCacheOperationResult& result);
};

// ============================================================================
// Impl Methods
// ============================================================================

KVCacheOperationResult KVCacheAPI::Impl::convert_to_result(
    const json& response, int slot_id, const std::string& filename, bool is_save)
{
    KVCacheOperationResult result;
    result.slot_id = slot_id;
    result.filename = filename;

    if (response.contains("error")) {
        result.success = false;
        result.error_message = response["error"].dump();
        return result;
    }

    result.success = true;
    result.n_tokens = response.value(is_save ? "n_saved" : "n_restored", 0);
    result.n_bytes = response.value(is_save ? "n_written" : "n_read", 0);

    if (response.contains("timings")) {
        result.processing_ms = response["timings"].value("prompt_ms", 0.0);
    }

    return result;
}

void KVCacheAPI::Impl::update_save_metrics(const KVCacheOperationResult& result) {
    if (result.success) {
        metrics.total_saves++;
        metrics.total_bytes_written += result.n_bytes;

        // Скользящее среднее для времени сохранения
        double alpha = 0.1;
        metrics.avg_save_time_ms = alpha * result.processing_ms +
                                    (1.0 - alpha) * metrics.avg_save_time_ms;
    }
}

void KVCacheAPI::Impl::update_restore_metrics(const KVCacheOperationResult& result) {
    if (result.success) {
        metrics.total_restores++;
        metrics.total_bytes_read += result.n_bytes;

        // Скользящее среднее для времени восстановления
        double alpha = 0.1;
        metrics.avg_restore_time_ms = alpha * result.processing_ms +
                                       (1.0 - alpha) * metrics.avg_restore_time_ms;
    }
}

// ============================================================================
// Public API Methods
// ============================================================================

KVCacheAPI::KVCacheAPI(std::shared_ptr<LlamaInterface> llama_interface)
    : pImpl(std::make_unique<Impl>(std::move(llama_interface))) {}

KVCacheAPI::~KVCacheAPI() = default;

KVCacheOperationResult KVCacheAPI::save(int slot_id, const std::string& filename) {
    try {
        json request_data = {{"filename", filename}};
        std::string endpoint = "/slots/" + std::to_string(slot_id) + "?action=save";

        std::string response_str = pImpl->llama_interface->make_http_request(
            endpoint, "POST", request_data);
        json response = json::parse(response_str);

        auto result = pImpl->convert_to_result(response, slot_id, filename, true);
        pImpl->update_save_metrics(result);

        return result;
    } catch (const std::exception& e) {
        KVCacheOperationResult result;
        result.slot_id = slot_id;
        result.filename = filename;
        result.error_message = std::string("Exception: ") + e.what();
        return result;
    }
}

KVCacheOperationResult KVCacheAPI::restore(int slot_id, const std::string& filename) {
    try {
        json request_data = {{"filename", filename}};
        std::string endpoint = "/slots/" + std::to_string(slot_id) + "?action=restore";

        std::string response_str = pImpl->llama_interface->make_http_request(
            endpoint, "POST", request_data);
        json response = json::parse(response_str);

        auto result = pImpl->convert_to_result(response, slot_id, filename, false);
        pImpl->update_restore_metrics(result);

        return result;
    } catch (const std::exception& e) {
        KVCacheOperationResult result;
        result.slot_id = slot_id;
        result.filename = filename;
        result.error_message = std::string("Exception: ") + e.what();
        return result;
    }
}

bool KVCacheAPI::reset(int slot_id) {
    try {
        std::string endpoint = "/slots/" + std::to_string(slot_id) + "?action=reset";
        pImpl->llama_interface->make_http_request(endpoint, "POST", json{});
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[KV-CACHE] Failed to reset slot " << slot_id
                  << ": " << e.what() << std::endl;
        return false;
    }
}

bool KVCacheAPI::erase(int slot_id) {
    try {
        std::string endpoint = "/slots/" + std::to_string(slot_id) + "?action=erase";
        pImpl->llama_interface->make_http_request(endpoint, "POST", json{});
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[KV-CACHE] Failed to erase slot " << slot_id
                  << ": " << e.what() << std::endl;
        return false;
    }
}

std::vector<SlotInfo> KVCacheAPI::get_slots_status() const {
    try {
        std::string response_str = pImpl->llama_interface->make_http_request(
            "/slots", "GET", json{});
        json response = json::parse(response_str);

        std::vector<SlotInfo> slots;
        if (response.contains("slots")) {
            for (const auto& slot_json : response["slots"]) {
                SlotInfo info;
                info.id = slot_json.value("id", -1);

                std::string status = slot_json.value("state", "unknown");
                if (status == "idle") info.status = SlotStatus::Idle;
                else if (status == "processing") info.status = SlotStatus::Processing;
                else info.status = SlotStatus::Error;

                info.num_prompt_tokens = slot_json.value("num_prompt_tokens", 0);
                info.num_infered_tokens = slot_json.value("num_infered_tokens", 0);
                info.last_activity = slot_json.value("last_activity", 0);

                slots.push_back(info);
            }
        }
        return slots;
    } catch (const std::exception& e) {
        std::cerr << "[KV-CACHE] Failed to get slots status: " << e.what() << std::endl;
        return {};
    }
}

SlotInfo KVCacheAPI::get_slot_status(int slot_id) const {
    auto slots = get_slots_status();
    for (const auto& slot : slots) {
        if (slot.id == slot_id) {
            return slot;
        }
    }
    return SlotInfo{-1, SlotStatus::Error, 0, 0, 0, ""};
}

int KVCacheAPI::find_free_slot() const {
    auto slots = get_slots_status();
    for (const auto& slot : slots) {
        if (slot.status == SlotStatus::Idle) {
            return slot.id;
        }
    }
    return -1;
}

bool KVCacheAPI::is_slot_available(int slot_id) const {
    auto slot = get_slot_status(slot_id);
    return slot.status == SlotStatus::Idle;
}

bool KVCacheAPI::validate_cache_file(const std::string& filepath) const {
    try {
        if (!fs::exists(filepath)) {
            return false;
        }

        auto file_size = fs::file_size(filepath);
        // Минимальный размер KV-cache файла (заголовок + данные)
        return file_size >= 1024;  // Минимум 1KB
    } catch (...) {
        return false;
    }
}

KVCacheMetrics KVCacheAPI::get_metrics() const {
    return pImpl->metrics;
}

void KVCacheAPI::reset_metrics() {
    pImpl->metrics = KVCacheMetrics{};
}

} // namespace core
} // namespace llama_gui

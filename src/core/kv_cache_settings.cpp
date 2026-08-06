#include "../include/core/kv_cache_settings.h"
#include <algorithm>
#include <cmath>

namespace llama_gui {
namespace core {

using json = nlohmann::json;

// =========================================================================
// Валидация настроек
// =========================================================================

bool KVCacheSettings::validate() const {
    if (n_parallel < 1 || n_parallel > 64) {
        return false;
    }

    if (!is_valid_cache_type(cache_type_k) || !is_valid_cache_type(cache_type_v)) {
        return false;
    }

    if (cache_reuse < 0) {
        return false;
    }

    if (max_file_age_seconds < 0) {
        return false;
    }

    return true;
}

std::string KVCacheSettings::get_validation_errors() const {
    std::string errors;

    if (n_parallel < 1 || n_parallel > 64) {
        errors += "Parallel slots must be between 1 and 64. ";
    }

    if (!is_valid_cache_type(cache_type_k)) {
        errors += "Invalid K-cache type: " + cache_type_k + ". ";
    }

    if (!is_valid_cache_type(cache_type_v)) {
        errors += "Invalid V-cache type: " + cache_type_v + ". ";
    }

    if (cache_reuse < 0) {
        errors += "Cache reuse must be non-negative. ";
    }

    if (max_file_age_seconds < 0) {
        errors += "Max file age must be non-negative. ";
    }

    return errors;
}

// =========================================================================
// Допустимые типы кэша
// =========================================================================

bool KVCacheSettings::is_valid_cache_type(const std::string& type) {
    static const std::vector<std::string> valid_types = {
        "f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl"
    };
    return std::find(valid_types.begin(), valid_types.end(), type) != valid_types.end();
}

std::vector<std::string> KVCacheSettings::get_valid_cache_types() {
    return {
        "f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl"
    };
}

// =========================================================================
// Оценка размера KV-cache
// =========================================================================

size_t KVCacheSettings::estimate_bytes_per_token(float model_params_b) const {
    // Базовый размер для f16: 2 bytes * 2 (K+V) * 2 (layers per billion params approx)
    // Для 7B модели: ~28 bytes per token (f16)
    // Для 70B модели: ~280 bytes per token (f16)

    float base_bytes = model_params_b * 4.0f;  // Приблизительно 4 bytes на 1B параметров

    // Коэффициент квантования
    float quant_factor = 1.0f;
    if (cache_type_k == "q8_0" || cache_type_v == "q8_0") {
        quant_factor = 0.5f;  // q8_0 уменьшает размер в 2 раза
    } else if (cache_type_k == "q4_0" || cache_type_k == "q4_1" ||
               cache_type_v == "q4_0" || cache_type_v == "q4_1") {
        quant_factor = 0.25f;  // q4_x уменьшает размер в 4 раза
    } else if (cache_type_k == "iq4_nl" || cache_type_v == "iq4_nl") {
        quant_factor = 0.3f;  // iq4_nl примерно 0.3
    }

    return static_cast<size_t>(base_bytes * quant_factor);
}

size_t KVCacheSettings::estimate_kv_cache_size(float model_params_b, int ctx_size) const {
    size_t bytes_per_token = estimate_bytes_per_token(model_params_b);
    return bytes_per_token * ctx_size;
}

// =========================================================================
// Сериализация
// =========================================================================

void to_json(nlohmann::json& j, const KVCacheSettings& s) {
    j = {
        {"slot_save_path", s.slot_save_path},
        {"n_parallel", s.n_parallel},
        {"cache_type_k", s.cache_type_k},
        {"cache_type_v", s.cache_type_v},
        {"cache_reuse", s.cache_reuse},
        {"auto_reuse_enabled", s.auto_reuse_enabled},
        {"max_storage_size_mb", s.max_storage_size_mb},
        {"max_file_age_seconds", s.max_file_age_seconds},
        {"auto_cleanup_enabled", s.auto_cleanup_enabled}
    };
}

void from_json(const nlohmann::json& j, KVCacheSettings& s) {
    s.slot_save_path = j.value("slot_save_path", "");
    s.n_parallel = j.value("n_parallel", 4);
    s.cache_type_k = j.value("cache_type_k", "q8_0");
    s.cache_type_v = j.value("cache_type_v", "q8_0");
    s.cache_reuse = j.value("cache_reuse", 0);
    s.auto_reuse_enabled = j.value("auto_reuse_enabled", true);
    s.max_storage_size_mb = j.value("max_storage_size_mb", 10240);
    s.max_file_age_seconds = j.value("max_file_age_seconds", 604800);
    s.auto_cleanup_enabled = j.value("auto_cleanup_enabled", true);
}

} // namespace core
} // namespace llama_gui

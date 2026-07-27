#pragma once

#include <string>
#include <vector>
#include <map>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки загрузки модели для llama.cpp
 * 
 * Включает все параметры загрузки моделей:
 * - Model paths (основная, draft, vocoder)
 * - Hugging Face integration
 * - LoRA adapters
 * - Multimodal projectors
 * - Device configuration
 */
struct ModelLoadingSettings {
    // =========================================================================
    // Model paths (основные пути)
    // =========================================================================
    
    /// Путь к модели (-m, --model)
    std::string model_path = "";
    
    /// URL модели (-mu, --model-url)
    std::string model_url = "";
    
    /// Репозиторий Hugging Face (-hf, --hf-repo)
    std::string hf_repo = "";
    
    /// Файл в Hugging Face (-hff, --hf-file)
    std::string hf_file = "";
    
    /// Токен Hugging Face (-hft, --hf-token)
    std::string hf_token = "";
    
    /// Псевдоним модели (-a, --alias)
    std::string model_alias = "";

    // =========================================================================
    // Draft model (speculative decoding)
    // =========================================================================
    
    /// Путь к черновой модели (-md, --model-draft)
    std::string model_draft = "";
    
    /// Репозиторий Hugging Face для draft (-hfd, --hf-repo-draft)
    std::string hf_repo_draft = "";
    
    /// Максимальное количество токенов для draft (--draft-max)
    int draft_max = 16;
    
    /// Минимальное количество токенов для draft (--draft-min)
    int draft_min = 0;
    
    /// Минимальная вероятность для draft (--draft-p-min)
    float draft_p_min = 0.8f;

    // =========================================================================
    // Vocoder model (TTS)
    // =========================================================================
    
    /// Путь к vocoder модели (-mv, --model-vocoder)
    std::string model_vocoder = "";
    
    /// Репозиторий Hugging Face для vocoder (-hfv, --hf-repo-v)
    std::string hf_repo_vocoder = "";
    
    /// Файл в Hugging Face для vocoder (-hffv, --hf-file-v)
    std::string hf_file_vocoder = "";

    // =========================================================================
    // LoRA adapters
    // =========================================================================
    
    /// Структура LoRA адаптера
    struct LoRAAdapter {
        std::string path;       /// Путь к адаптеру
        float scale = 1.0f;     /// Масштаб (--lora-scaled)
        
        bool operator==(const LoRAAdapter& other) const {
            return path == other.path && scale == other.scale;
        }
    };
    
    /// Список LoRA адаптеров (--lora, --lora-scaled)
    std::vector<LoRAAdapter> lora_adapters;
    
    /// Базовая модель для LoRA (--lora-base)
    std::string lora_base = "";
    
    /// Инициализировать LoRA без применения (--lora-init-without-apply)
    bool lora_init_without_apply = false;

    // =========================================================================
    // Multimodal (мультимодальные проекторы)
    // =========================================================================
    
    /// Путь к мультимодальному проектору (--mmproj)
    std::string mmproj = "";
    
    /// URL мультимодального проектора (--mmproj-url)
    std::string mmproj_url = "";
    
    /// Не использовать мультимодальный проектор (--no-mmproj)
    bool no_mmproj = false;
    
    /// Не выгружать мультимодальный проектор (--no-mmproj-offload)
    bool no_mmproj_offload = false;

    // =========================================================================
    // Model validation (валидация модели)
    // =========================================================================
    
    /// Проверять тензоры модели (--check-tensors)
    bool check_tensors = false;
    
    /// Переопределение ключей модели (--override-kv)
    /// Ключ: key name, Значение: value
    std::map<std::string, std::string> override_kv;

    // =========================================================================
    // Device configuration (конфигурация устройств)
    // =========================================================================
    
    /// Устройство для модели (-dev, --device)
    std::string device = "";
    
    /// Устройство для draft модели (-devd, --device-draft)
    std::string device_draft = "";
    
    /// Список устройств (--list-devices)
    bool list_devices = false;

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        if (draft_max < 0) return false;
        if (draft_min < 0) return false;
        if (draft_min > draft_max) return false;
        if (draft_p_min < 0.0f || draft_p_min > 1.0f) return false;
        
        for (const auto& adapter : lora_adapters) {
            if (adapter.path.empty()) return false;
            if (adapter.scale < 0.0f) return false;
        }
        
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;
        
        if (draft_max < 0) {
            errors += "Draft max must be non-negative. ";
        }
        if (draft_min < 0) {
            errors += "Draft min must be non-negative. ";
        }
        if (draft_min > draft_max) {
            errors += "Draft min must be <= draft max. ";
        }
        if (draft_p_min < 0.0f || draft_p_min > 1.0f) {
            errors += "Draft p-min must be between 0 and 1. ";
        }
        
        for (size_t i = 0; i < lora_adapters.size(); ++i) {
            if (lora_adapters[i].path.empty()) {
                errors += "LoRA adapter " + std::to_string(i) + " has no path. ";
            }
            if (lora_adapters[i].scale < 0.0f) {
                errors += "LoRA adapter " + std::to_string(i) + " scale must be non-negative. ";
            }
        }
        
        return errors;
    }

    /**
     * @brief Добавить LoRA адаптер
     * @param path Путь к адаптеру
     * @param scale Масштаб (по умолчанию 1.0)
     */
    void add_lora_adapter(const std::string& path, float scale = 1.0f) {
        lora_adapters.push_back({path, scale});
    }

    /**
     * @brief Удалить LoRA адаптер по индексу
     * @param index Индекс адаптера
     * @return true если успешно удален
     */
    bool remove_lora_adapter(size_t index) {
        if (index >= lora_adapters.size()) return false;
        lora_adapters.erase(lora_adapters.begin() + index);
        return true;
    }
};

} // namespace core
} // namespace llama_gui

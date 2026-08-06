#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>

namespace llama_gui {
namespace core {

struct ModelManagerInfo {
    std::string name;
    std::string path;
    std::string type;
    long long size_bytes;
    std::string parameters;
    bool is_loaded;
    std::string last_used;
};

// Callback для обновления прогресса загрузки модели
using ModelProgressCallback = std::function<void(const std::string& model_name, float progress, const std::string& status)>;

// Callback для уведомления об извлечении chat template
using ChatTemplateCallback = std::function<void(const std::string& model_path, const std::string& chat_template, const std::string& source)>;

class ModelManager {
public:
    using ModelListCallback = std::function<void(const std::vector<ModelManagerInfo>& models)>;
    using ModelStatusCallback = std::function<void(const std::string& model_name, bool success, const std::string& message)>;
    using ChatTemplateCallback_t = ChatTemplateCallback;

    ModelManager();
    ~ModelManager();

    // Запрет копирования
    ModelManager(const ModelManager&) = delete;
    ModelManager& operator=(const ModelManager&) = delete;

    // Инициализация
    bool initialize(const std::string& models_directory);

    // Скан моделей
    std::vector<ModelManagerInfo> scan_models() const;
    ModelManagerInfo get_model_info(const std::string& model_path) const;

    // Управление моделями
    bool load_model(const std::string& model_path);
    bool load_model_with_progress(const std::string& model_path, ModelProgressCallback progress_callback);
    bool unload_model();
    bool is_model_loaded() const;
    std::string get_current_model() const;

    // Прогресс загрузки
    float get_load_progress() const { return load_progress_.load(); }
    std::string get_load_status() const {
        std::lock_guard<std::mutex> lock(load_status_mutex_);
        return load_status_;
    }
    bool is_loading() const { return is_loading_.load(); }

    // Коллбеки
    void set_model_list_callback(ModelListCallback callback);
    void set_model_status_callback(ModelStatusCallback callback);
    void set_model_progress_callback(ModelProgressCallback callback);
    void set_chat_template_callback(ChatTemplateCallback_t callback);

    // Настройки
    void set_models_directory(const std::string& directory);
    std::string get_models_directory() const;

    // Информация о модели
    std::vector<std::string> get_supported_model_types() const;
    bool is_valid_model_file(const std::string& file_path) const;

    // Обновление списка моделей
    void refresh_model_list();

private:
    std::string models_directory_;
    std::string current_model_path_;
    std::atomic<bool> model_loaded_;
    std::atomic<float> load_progress_;
    mutable std::string load_status_;  // Не atomic, так как std::string не поддерживает atomic
    std::atomic<bool> is_loading_;
    mutable std::mutex load_status_mutex_;  // Мьютекс для защиты load_status_ (mutable для const методов)
    ModelListCallback model_list_callback_;
    ModelStatusCallback model_status_callback_;
    ModelProgressCallback model_progress_callback_;
    ChatTemplateCallback_t chat_template_callback_;

    // Вспомогательные методы
    void notify_model_list_updated();
    void notify_model_status(const std::string& model_name, bool success, const std::string& message);
    void notify_model_progress(const std::string& model_name, float progress, const std::string& status);
    std::string get_file_extension(const std::string& file_path) const;
    long long get_file_size(const std::string& file_path) const;
    
    // Chat template integration
    void extract_chat_template_if_needed(const std::string& model_path);
};

} // namespace core
} // namespace llama_gui

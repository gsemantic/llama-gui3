#include "../../include/core/model_manager.h"
#include "../include/core/chat_template_manager.h"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <thread>

namespace llama_gui {
namespace core {

ModelManager::ModelManager()
    : models_directory_("/home/Alex/projects/gguf/")
    , model_loaded_(false)
    , load_progress_(0.0f)
    , load_status_("")
    , is_loading_(false) {
    std::cout << "ModelManager: Инициализация" << std::endl;
}

ModelManager::~ModelManager() {
    unload_model();
    std::cout << "ModelManager: Завершение работы" << std::endl;
}

bool ModelManager::initialize(const std::string& models_directory) {
    if (models_directory.empty()) {
        std::cerr << "ModelManager: Путь к директории моделей не может быть пустым" << std::endl;
        return false;
    }

    models_directory_ = models_directory;

    // Проверка существования директории
    if (!std::filesystem::exists(models_directory_)) {
        try {
            std::filesystem::create_directories(models_directory_);
            std::cout << "ModelManager: Создана директория для моделей: " << models_directory_ << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "ModelManager: Не удалось создать директорию: " << e.what() << std::endl;
            return false;
        }
    }

    std::cout << "ModelManager: Инициализирован с директорией: " << models_directory_ << std::endl;
    return true;
}

std::vector<ModelManagerInfo> ModelManager::scan_models() const {
    std::vector<ModelManagerInfo> models;

    try {
        if (!std::filesystem::exists(models_directory_)) {
            std::cout << "ModelManager: Директория моделей не существует: " << models_directory_ << std::endl;
            return models;
        }

        for (const auto& entry : std::filesystem::directory_iterator(models_directory_)) {
            if (entry.is_regular_file()) {
                std::string file_path = entry.path().string();
                if (is_valid_model_file(file_path)) {
                    ModelManagerInfo model_info = get_model_info(file_path);
                    models.push_back(model_info);
                }
            }
        }

        // Сортировка по имени
        std::sort(models.begin(), models.end(),
            [](const ModelManagerInfo& a, const ModelManagerInfo& b) {
                return a.name < b.name;
            });

    } catch (const std::exception& e) {
        std::cerr << "ModelManager: Ошибка сканирования моделей: " << e.what() << std::endl;
    }

    return models;
}

ModelManagerInfo ModelManager::get_model_info(const std::string& model_path) const {
    ModelManagerInfo info;
    info.path = model_path;
    info.name = std::filesystem::path(model_path).filename().string();
    info.type = get_file_extension(model_path);
    info.size_bytes = get_file_size(model_path);
    info.is_loaded = (model_path == current_model_path_ && model_loaded_);

    // Форматирование размера
    std::ostringstream size_stream;
    if (info.size_bytes < 1024) {
        size_stream << info.size_bytes << " B";
    } else if (info.size_bytes < 1024 * 1024) {
        size_stream << (info.size_bytes / 1024) << " KB";
    } else if (info.size_bytes < 1024 * 1024 * 1024) {
        size_stream << (info.size_bytes / (1024 * 1024)) << " MB";
    } else {
        size_stream << (info.size_bytes / (1024 * 1024 * 1024)) << " GB";
    }

    info.parameters = size_stream.str();

    // Получение текущей даты/времени
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm = *std::localtime(&now_time);

    std::ostringstream time_stream;
    time_stream << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S");
    info.last_used = time_stream.str();

    return info;
}

bool ModelManager::load_model(const std::string& model_path) {
    return load_model_with_progress(model_path, nullptr);
}

bool ModelManager::load_model_with_progress(const std::string& model_path, ModelProgressCallback progress_callback) {
    if (!is_valid_model_file(model_path)) {
        std::cerr << "ModelManager: Неверный файл модели: " << model_path << std::endl;
        notify_model_status(model_path, false, "Неверный файл модели");
        return false;
    }

    if (model_loaded_.load() && current_model_path_ == model_path) {
        std::cout << "ModelManager: Модель уже загружена: " << model_path << std::endl;
        notify_model_status(model_path, true, "Модель уже загружена");
        return true;
    }

    // Выгрузка текущей модели
    if (model_loaded_.load()) {
        unload_model();
    }

    // Симуляция загрузки модели с прогрессом
    std::cout << "ModelManager: Загрузка модели: " << model_path << std::endl;

    // Проверка существования файла
    if (!std::filesystem::exists(model_path)) {
        std::cerr << "ModelManager: Файл модели не существует: " << model_path << std::endl;
        notify_model_status(model_path, false, "Файл модели не существует");
        return false;
    }

    // Получаем размер файла для прогресса
    long long file_size = get_file_size(model_path);
    is_loading_.store(true);
    load_progress_.store(0.0f);
    {
        std::lock_guard<std::mutex> lock(load_status_mutex_);
        load_status_ = "Инициализация загрузки...";
    }

    // Симуляция прогресса загрузки (в реальности здесь будет actual загрузка)
    const int steps = 10;
    for (int i = 1; i <= steps; ++i) {
        float progress = static_cast<float>(i) / steps;
        load_progress_.store(progress);
        {
            std::lock_guard<std::mutex> lock(load_status_mutex_);
            load_status_ = "Загрузка: " + std::to_string(static_cast<int>(progress * 100)) + "%";
        }
        notify_model_progress(model_path, progress, load_status_);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Симуляция задержки
    }

    // Обновление состояния
    current_model_path_ = model_path;
    model_loaded_.store(true);
    is_loading_.store(false);
    load_progress_.store(1.0f);
    {
        std::lock_guard<std::mutex> lock(load_status_mutex_);
        load_status_ = "Модель загружена";
    }

    notify_model_progress(model_path, 1.0f, "Загрузка завершена");
    notify_model_status(model_path, true, "Модель успешно загружена");
    notify_model_list_updated();

    // Извлечение chat_template из GGUF (если включено авто-определение)
    extract_chat_template_if_needed(model_path);

    std::cout << "ModelManager: Модель загружена успешно: " << model_path << std::endl;
    return true;
}

bool ModelManager::unload_model() {
    if (!model_loaded_.load()) {
        std::cout << "ModelManager: Нет загруженной модели для выгрузки" << std::endl;
        return true;
    }

    std::cout << "ModelManager: Выгрузка модели: " << current_model_path_ << std::endl;

    // Симуляция выгрузки модели
    current_model_path_.clear();
    model_loaded_.store(false);

    notify_model_status("", true, "Модель успешно выгружена");
    notify_model_list_updated();

    std::cout << "ModelManager: Модель выгружена успешно" << std::endl;
    return true;
}

bool ModelManager::is_model_loaded() const {
    return model_loaded_.load();
}

std::string ModelManager::get_current_model() const {
    return current_model_path_;
}

void ModelManager::set_model_list_callback(ModelListCallback callback) {
    model_list_callback_ = callback;
}

void ModelManager::set_model_status_callback(ModelStatusCallback callback) {
    model_status_callback_ = callback;
}

void ModelManager::set_model_progress_callback(ModelProgressCallback callback) {
    model_progress_callback_ = callback;
}

void ModelManager::set_chat_template_callback(ChatTemplateCallback_t callback) {
    chat_template_callback_ = callback;
}

void ModelManager::set_models_directory(const std::string& directory) {
    if (directory != models_directory_) {
        models_directory_ = directory;
        notify_model_list_updated();
    }
}

void ModelManager::refresh_model_list() {
    notify_model_list_updated();
}

std::string ModelManager::get_models_directory() const {
    return models_directory_;
}

std::vector<std::string> ModelManager::get_supported_model_types() const {
    return {
        "gguf", "ggml", "bin", "pth", "pt", "ckpt",
        "safetensors", "h5", "onnx", "tflite"
    };
}

bool ModelManager::is_valid_model_file(const std::string& file_path) const {
    std::string ext = get_file_extension(file_path);
    if (ext.empty()) return false;

    // Удаление точки из расширения
    if (ext[0] == '.') {
        ext = ext.substr(1);
    }

    const auto& supported_types = get_supported_model_types();
    return std::find(supported_types.begin(), supported_types.end(), ext) != supported_types.end();
}

void ModelManager::notify_model_list_updated() {
    if (model_list_callback_) {
        try {
            auto models = scan_models();
            model_list_callback_(models);
        } catch (const std::exception& e) {
            std::cerr << "ModelManager: Ошибка уведомления о списке моделей: " << e.what() << std::endl;
        }
    }
}

void ModelManager::notify_model_status(const std::string& model_name, bool success, const std::string& message) {
    if (model_status_callback_) {
        try {
            model_status_callback_(model_name, success, message);
        } catch (const std::exception& e) {
            std::cerr << "ModelManager: Ошибка уведомления о статусе модели: " << e.what() << std::endl;
        }
    }
}

void ModelManager::notify_model_progress(const std::string& model_name, float progress, const std::string& status) {
    if (model_progress_callback_) {
        try {
            model_progress_callback_(model_name, progress, status);
        } catch (const std::exception& e) {
            std::cerr << "ModelManager: Ошибка уведомления о прогрессе модели: " << e.what() << std::endl;
        }
    }
}

std::string ModelManager::get_file_extension(const std::string& file_path) const {
    size_t pos = file_path.find_last_of('.');
    if (pos == std::string::npos) {
        return "";
    }
    return file_path.substr(pos);
}

long long ModelManager::get_file_size(const std::string& file_path) const {
    try {
        return std::filesystem::file_size(file_path);
    } catch (const std::exception& e) {
        std::cerr << "ModelManager: Не удалось получить размер файла: " << e.what() << std::endl;
        return 0;
    }
}

/**
 * @brief Извлечь chat_template из GGUF если требуется авто-определение
 * @param model_path Путь к модели
 *
 * Этот метод вызывается после загрузки модели для автоматического
 * извлечения chat_template из метаданных GGUF.
 *
 * Результат извлечения кэшируется в ChatTemplateManager для повторного использования.
 * При успешном извлечении вызывается chat_template_callback_ для сохранения в настройки.
 */
void ModelManager::extract_chat_template_if_needed(const std::string& model_path) {
    auto& template_manager = llama_gui::core::ChatTemplateManager::instance();

    std::cout << "[ModelManager] Extracting chat template from GGUF: " << model_path << std::endl;

    auto result = template_manager.extract_from_gguf(model_path);

    if (result.success) {
        std::cout << "[ModelManager] Chat template extracted: " << result.template_str.size() << " bytes" << std::endl;
        std::cout << "[ModelManager] Template source: " << result.source << std::endl;
        if (!result.model_name.empty()) {
            std::cout << "[ModelManager] Model name: " << result.model_name << std::endl;
        }
        
        // Уведомляем через callback для сохранения в настройки
        if (chat_template_callback_) {
            chat_template_callback_(model_path, result.template_str, result.source);
        }
    } else {
        std::cout << "[ModelManager] Chat template not found in GGUF metadata" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "[ModelManager] Error: " << result.error_message << std::endl;
        }
    }
}

} // namespace core
} // namespace llama_gui

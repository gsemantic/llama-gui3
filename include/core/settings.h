#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <utility> // для std::pair
#include <nlohmann/json.hpp>
#include "rag_settings.h"
#include "settings_sampling.h"
#include "settings_model_loading.h"
#include "settings_gpu.h"
#include "settings_cache.h"
#include "settings_rope.h"
#include "settings_control_vector.h"
#include "settings_tensor_override.h"
#include "settings_server_runtime.h"
#include "settings_batch.h"
#include "settings_grammar.h"
#include "settings_output.h"
#include "model_performance_stats.h"
#include "ini_parser.h"

namespace llama_gui {
namespace core {

/**
 * @brief Типы тем интерфейса
 */
enum class ThemeType {
    Dark,
    Light,
    Auto,
    Custom
};

/**
 * @brief Настройки отображения
 */
struct DisplaySettings {
    int window_width = 1200;
    int window_height = 800;
    bool window_maximized = false;
    int window_x = 0;
    int window_y = 0;
    bool use_dark_theme = true;
    float font_size = 14.0f;
    std::string font_family = "default";
    bool enable_animation = true;
    int frame_rate_limit = 60;
    std::string language = "ru";   // UI language code (e.g. "ru", "en"); default is Russian
    
    // Новые поля для адаптации к разрешению монитора
    int screen_width = 1920;
    int screen_height = 1080;
    float dpi_scale = 1.0f;
    bool auto_resize = true;
    int min_window_width = 800;
    int min_window_height = 600;
    bool center_window = true;
    int margin = 10;
};

/**
 * @brief Настройки производительности
 */
struct PerformanceSettings {
    bool enable_vsync = true;
    int target_fps = 60;
    int idle_fps = 15;
    int idle_timeout_ms = 5000; // 5 seconds of inactivity to switch to idle FPS
    bool enable_smart_redraw = false; // Отключено по умолчанию для совместимости
    bool show_performance_overlay = false;
    int performance_update_interval_ms = 250;
    bool enable_logging = true;
    std::string log_level = "Info";
    bool log_to_file = false;
    std::string log_file_path = "llama-gui.log";
    std::string log_flush_policy = "Immediate";
    bool debug_mode = false; // Debug logging mode
};

/**
 * @brief Настройки сервера
 */
struct ServerSettings {
    std::string host = "localhost";
    int port = 8081;
    std::string api_url = "http://localhost:8081";
    int connection_timeout = 30000; // миллисекунды
    int request_timeout = 60000;    // миллисекунды
    int max_retries = 3;
    bool verify_ssl = true;
    std::string auth_token = "";
};

/**
 * @brief Настройки облачного провайдера (OpenAI-совместимый)
 *
 * API ключ хранится в .env файле, не здесь.
 */
struct CloudProviderSettings {
    bool enabled = false;                    // Использовать облачный провайдер вместо локальной модели
    std::string provider_name = "";          // Имя провайдера (e.g. "OpenRouter", "Custom")
    std::string endpoint_url = "";           // Base URL (e.g. "https://openrouter.ai/api/v1")
    std::string model_id = "";               // Выбранная модель (ID)
    int context_length = 0;                  // Контекстное окно выбранной модели (из /models, 0 = неизвестно)
    int timeout_ms = 60000;                  // Таймаут запросов (60s default for cloud providers)
    int max_output_tokens = 0;               // Лимит токенов вывода (0 = не ограничено, полагаемся на модель/провайдера)
    bool reasoning_enabled = false;          // Режим размышлений/thinking (для поддерживающих моделей: GLM, DeepSeek, o-серия)
    int reasoning_budget = 0;                // Бюджет токенов на reasoning (0 = по умолчанию провайдера/модели)
    bool free_models_only = false;           // Показывать только бесплатные модели
    std::string last_search_query = "";      // Последний поисковый запрос

    // Автозаполнение
    std::vector<std::string> recent_models;  // Недавние модели
};

// Backward compatibility alias
using OpenRouterSettings = CloudProviderSettings;

/**
 * @brief Настройки чата
 */
struct ChatSettings {
    bool auto_scroll = true;
    int max_messages_display = 100;
    bool show_timestamps = true;
    bool show_system_messages = true;
    bool preserve_formatting = true;
    std::string default_system_prompt = "You are a helpful assistant.";

    // Expanded Generation parameters
    int max_tokens = 2048;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    float min_p = 0.05f;
    float repeat_penalty = 1.1f;
    float presence_penalty = 0.0f;
    float frequency_penalty = 0.0f;

    int mirostat_mode = 0; // 0=Disabled, 1=Mirostat, 2=Mirostat 2.0
    float mirostat_tau = 5.0f;
    float mirostat_eta = 0.1f;

    bool stop_on_newline = false;

    // Additional llama.cpp parameters
    int threads = 4; // CPU threads
    int n_ctx = 4096; // Context size
    int seed = -1; // Random seed (-1 for random)
    float tfs_z = 1.0f; // Tail free sampling (1.0 = disabled)
    float typical_p = 1.0f; // Typical sampling (1.0 = disabled)
    int n_gpu_layers = 0; // GPU layers
    std::string tensor_split = ""; // GPU tensor split
    std::string numa = "none"; // NUMA strategy
    std::vector<std::string> lora_adapters; // LoRA adapters
    std::string lora_base = ""; // Base model for LoRA
    std::string mmproj = ""; // Multimodal projector
    std::string grammar = ""; // Grammar file
    std::string chat_template = ""; // Chat template
    bool embedding = false; // Embedding mode
    std::vector<std::string> reverse_prompt; // Reverse prompts (stop sequences)
    std::string log_format = "text"; // Log format
    int verbosity = 0; // Verbosity level
};

/**
 * @brief Настройки файлов
 */
struct FileSettings {
    std::string default_save_path = "";
    std::string default_export_path = "";
    std::string auto_save_path = "";
    bool auto_save_enabled = false;
    int auto_save_interval = 300; // секунды
    std::vector<std::string> supported_import_formats = {"json", "txt", "md", "csv"};
    std::vector<std::string> supported_export_formats = {"json", "txt", "csv", "html"};
    size_t max_file_size = 10485760; // 10MB
};

/**
 * @brief Класс управления настройками приложения
 */
class Settings {
public:
    Settings();
    ~Settings();

    // Запрет копирования
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    // Загрузка и сохранение
    bool load_from_file(const std::string& file_path);
    bool save_to_file(const std::string& file_path) const;
    bool reset_to_defaults();

    // Методы для работы с INI файлом настроек (аналог php.ini)
    bool load_from_ini(const std::string& file_path);
    bool save_to_ini(const std::string& file_path) const;
    std::string get_ini_file_path() const { return ini_file_path_; }
    void set_ini_file_path(const std::string& path) { ini_file_path_ = path; }

    // =========================================================================
    // Синхронизация настроек между различными источниками
    // =========================================================================
    
    /**
     * @brief Явная синхронизация настроек при старте приложения
     * 
     * Логика:
     * 1. Если существует settings.ini и profiles - приоритет у profiles (последний сохранённый)
     * 2. Если существует только settings.ini - загружаем его
     * 3. Если ничего нет - используем настройки по умолчанию
     * 
     * @return true если настройки успешно загружены/синхронизированы
     */
    bool synchronize_at_startup();

    // Сериализация JSON
    std::string serialize_to_json() const;
    bool deserialize_from_json(const std::string& json_data);

    // Модульные методы сериализации (для разделения на файлы)
    // Основные настройки
    void serializeDisplaySettings(nlohmann::json& j) const;
    void serializeServerSettings(nlohmann::json& j) const;
    void serializeChatSettings(nlohmann::json& j) const;
    void serializeFileSettings(nlohmann::json& j) const;
    void serializePerformanceSettings(nlohmann::json& j) const;
    void serializeRagSettings(nlohmann::json& j) const;

    // Расширенные настройки llama.cpp
    void serializeSamplingSettings(nlohmann::json& j) const;
    void serializeModelLoadingSettings(nlohmann::json& j) const;
    void serializeGpuSettings(nlohmann::json& j) const;
    void serializeCacheSettings(nlohmann::json& j) const;
    void serializeRopeSettings(nlohmann::json& j) const;
    void serializeControlVectorSettings(nlohmann::json& j) const;
    void serializeTensorOverrideSettings(nlohmann::json& j) const;

    // Настройки сервера и выполнения
    void serializeServerRuntimeSettings(nlohmann::json& j) const;
    void serializeBatchSettings(nlohmann::json& j) const;
    void serializeGrammarSettings(nlohmann::json& j) const;
    void serializeOutputSettings(nlohmann::json& j) const;

    // OpenRouter настройки
    void serializeOpenRouterSettings(nlohmann::json& j) const;

    // Модульные методы десериализации
    void deserializeDisplaySettings(const nlohmann::json& j);
    void deserializeServerSettings(const nlohmann::json& j);
    void deserializeChatSettings(const nlohmann::json& j);
    void deserializeFileSettings(const nlohmann::json& j);
    void deserializePerformanceSettings(const nlohmann::json& j);
    void deserializeRagSettings(const nlohmann::json& j);
    void deserializeSamplingSettings(const nlohmann::json& j);
    void deserializeModelLoadingSettings(const nlohmann::json& j);
    void deserializeGpuSettings(const nlohmann::json& j);
    void deserializeCacheSettings(const nlohmann::json& j);
    void deserializeRopeSettings(const nlohmann::json& j);
    void deserializeControlVectorSettings(const nlohmann::json& j);
    void deserializeTensorOverrideSettings(const nlohmann::json& j);
    void deserializeServerRuntimeSettings(const nlohmann::json& j);
    void deserializeBatchSettings(const nlohmann::json& j);
    void deserializeGrammarSettings(const nlohmann::json& j);
    void deserializeOutputSettings(const nlohmann::json& j);
    void deserializeOpenRouterSettings(const nlohmann::json& j);

    // Управление профилями
    std::vector<std::string> list_profiles() const;
    bool save_profile(const std::string& profile_name) const;
    bool load_profile(const std::string& profile_name);
    void sync_ctx_size(); // Синхронизация n_ctx и ctx_size
    void sync_max_tokens(); // Синхронизация max_tokens и n_predict

    // INI load/save helpers
    void load_display_settings(const IniParser::Document& doc);
    void load_performance_settings(const IniParser::Document& doc);
    void load_server_settings(const IniParser::Document& doc);
    void load_chat_settings(const IniParser::Document& doc);
    void load_file_settings(const IniParser::Document& doc);
    void load_rag_settings(const IniParser::Document& doc);
    void load_sampling_settings(const IniParser::Document& doc);
    void load_model_loading_settings(const IniParser::Document& doc);
    void load_gpu_settings(const IniParser::Document& doc);
    void load_cache_settings(const IniParser::Document& doc);
    void load_rope_settings(const IniParser::Document& doc);
    void load_control_vector_settings(const IniParser::Document& doc);
    void load_server_runtime_settings(const IniParser::Document& doc);
    void load_batch_settings(const IniParser::Document& doc);
    void load_grammar_settings(const IniParser::Document& doc);
    void load_output_settings(const IniParser::Document& doc);

    bool load_last_profile();
    bool delete_profile(const std::string& profile_name);
    std::string get_current_profile_name() const { return current_profile_name_; }
    void set_current_profile_name(const std::string& name) { current_profile_name_ = name; }
    std::string get_profiles_directory() const { return profiles_directory_; }
    void set_profiles_directory(const std::string& path) { profiles_directory_ = path; }

    // Настройки синхронизации
    bool is_sync_max_tokens_enabled() const { return sync_max_tokens_enabled_; }
    void set_sync_max_tokens_enabled(bool enabled) { sync_max_tokens_enabled_ = enabled; }

    // Настройки дисплея
    DisplaySettings& display() { return display_settings_; }
    const DisplaySettings& display() const { return display_settings_; }
    void set_display_settings(const DisplaySettings& settings);

    // Настройки сервера
    ServerSettings& server() { return server_settings_; }
    const ServerSettings& server() const { return server_settings_; }
    void set_server_settings(const ServerSettings& settings);

    // Настройки чата
    ChatSettings& chat() { return chat_settings_; }
    const ChatSettings& chat() const { return chat_settings_; }
    void set_chat_settings(const ChatSettings& settings);

    // Настройки файлов
    FileSettings& files() { return file_settings_; }
    const FileSettings& files() const { return file_settings_; }
    void set_file_settings(const FileSettings& settings);

    // Настройки производительности
    PerformanceSettings& performance() { return performance_settings_; }
    const PerformanceSettings& performance() const { return performance_settings_; }
    void set_performance_settings(const PerformanceSettings& settings);

    // Настройки RAG
    RagSettings& rag() { return rag_settings_; }
    const RagSettings& rag() const { return rag_settings_; }
    void set_rag_settings(const RagSettings& settings);

    // =========================================================================
    // Новые группы настроек (llama.cpp server parameters)
    // =========================================================================

    /// Настройки сэмплирования
    SamplingSettings& sampling() { return sampling_settings_; }
    const SamplingSettings& sampling() const { return sampling_settings_; }
    void set_sampling_settings(const SamplingSettings& settings) { sampling_settings_ = settings; }

    /// Настройки загрузки модели
    ModelLoadingSettings& model_loading() { return model_loading_settings_; }
    const ModelLoadingSettings& model_loading() const { return model_loading_settings_; }
    void set_model_loading_settings(const ModelLoadingSettings& settings) { model_loading_settings_ = settings; }

    /// Настройки GPU
    GPUSettings& gpu() { return gpu_settings_; }
    const GPUSettings& gpu() const { return gpu_settings_; }
    void set_gpu_settings(const GPUSettings& settings) { gpu_settings_ = settings; }

    /// Настройки кэша
    CacheSettings& cache() { return cache_settings_; }
    const CacheSettings& cache() const { return cache_settings_; }
    void set_cache_settings(const CacheSettings& settings) { cache_settings_ = settings; }

    /// Настройки RoPE
    RoPESettings& rope() { return rope_settings_; }
    const RoPESettings& rope() const { return rope_settings_; }
    void set_rope_settings(const RoPESettings& settings) { rope_settings_ = settings; }

    /// Настройки векторов управления
    ControlVectorSettings& control_vector() { return control_vector_settings_; }
    const ControlVectorSettings& control_vector() const { return control_vector_settings_; }
    void set_control_vector_settings(const ControlVectorSettings& settings) { control_vector_settings_ = settings; }

    /// Настройки переопределения тензоров
    TensorOverrideSettings& tensor_override() { return tensor_override_settings_; }
    const TensorOverrideSettings& tensor_override() const { return tensor_override_settings_; }
    void set_tensor_override_settings(const TensorOverrideSettings& settings) { tensor_override_settings_ = settings; }

    /// Настройки выполнения сервера
    ServerRuntimeSettings& server_runtime() { return server_runtime_settings_; }
    const ServerRuntimeSettings& server_runtime() const { return server_runtime_settings_; }
    void set_server_runtime_settings(const ServerRuntimeSettings& settings) { server_runtime_settings_ = settings; }

    /// Настройки пакетной обработки
    BatchSettings& batch() { return batch_settings_; }
    const BatchSettings& batch() const { return batch_settings_; }
    void set_batch_settings(const BatchSettings& settings) { batch_settings_ = settings; }

    /// Настройки грамматики
    GrammarSettings& grammar() { return grammar_settings_; }
    const GrammarSettings& grammar() const { return grammar_settings_; }
    void set_grammar_settings(const GrammarSettings& settings) { grammar_settings_ = settings; }

    /// Настройки вывода
    OutputSettings& output() { return output_settings_; }
    const OutputSettings& output() const { return output_settings_; }
    void set_output_settings(const OutputSettings& settings) { output_settings_ = settings; }

    /// Настройки облачного провайдера (alias: openrouter)
    CloudProviderSettings& cloud_provider() { return openrouter_settings_; }
    const CloudProviderSettings& cloud_provider() const { return openrouter_settings_; }
    void set_cloud_provider_settings(const CloudProviderSettings& settings) { openrouter_settings_ = settings; }

    // Backward compat aliases
    OpenRouterSettings& openrouter() { return openrouter_settings_; }
    const OpenRouterSettings& openrouter() const { return openrouter_settings_; }
    void set_openrouter_settings(const OpenRouterSettings& settings) { openrouter_settings_ = settings; }

    /// Статистика производительности моделей
    ModelPerformanceManager& model_performance() { return model_performance_manager_; }
    const ModelPerformanceManager& model_performance() const { return model_performance_manager_; }

    // Настройки сервера
    void set_server_url(const std::string& url);
    std::string get_server_url() const;
    bool is_server_reachable() const;

    // Темы и стили
    void set_theme(ThemeType theme);
    ThemeType get_theme() const;
    bool is_dark_theme() const;

    // Валидация настроек
    bool validate() const;
    std::string get_validation_errors() const;

    // Валидация max_tokens относительно ctx_size
    bool validate_max_tokens() const;
    std::string get_max_tokens_validation_error() const;
    int get_recommended_max_tokens() const;
    int get_max_allowed_tokens() const;
    int get_prompt_reserve() const;

    // Пользовательские настройки
    void set_custom_setting(const std::string& key, const std::string& value);
    std::string get_custom_setting(const std::string& key, const std::string& default_value = "") const;
    bool has_custom_setting(const std::string& key) const;
    void remove_custom_setting(const std::string& key);

    // Отладка
    std::string get_debug_info() const;

    // Путь к модели
    std::string get_model_path() const;
    void set_model_path(const std::string& model_path);

    // Алиас модели
    std::string get_model_alias() const;
    void set_model_alias(const std::string& model_alias);

    // Путь к модели эмбеддингов
    std::string get_embedding_model_path() const;
    void set_embedding_model_path(const std::string& model_path);
    
    // Новые методы для адаптации к разрешению монитора
    int get_screen_width() const;
    int get_screen_height() const;
    float get_dpi_scale() const;
    bool should_auto_resize() const;
    void set_auto_resize(bool auto_resize);
    void set_window_position(int x, int y);
    std::pair<int, int> get_window_position() const;
    void set_window_maximized(bool maximized);
    bool is_window_maximized() const;
    std::pair<int, int> get_optimal_window_size() const;
    std::pair<int, int> get_safe_window_bounds() const;
    void adapt_ui_components();

private:
    DisplaySettings display_settings_;
    ServerSettings server_settings_;
    ChatSettings chat_settings_;
    FileSettings file_settings_;
    PerformanceSettings performance_settings_;
    RagSettings rag_settings_;

    // Новые группы настроек (llama.cpp server parameters)
    SamplingSettings sampling_settings_;
    ModelLoadingSettings model_loading_settings_;
    GPUSettings gpu_settings_;
    CacheSettings cache_settings_;
    RoPESettings rope_settings_;
    ControlVectorSettings control_vector_settings_;
    TensorOverrideSettings tensor_override_settings_;
    ServerRuntimeSettings server_runtime_settings_;
    BatchSettings batch_settings_;
    GrammarSettings grammar_settings_;
    OutputSettings output_settings_;
    OpenRouterSettings openrouter_settings_;

    ModelPerformanceManager model_performance_manager_;

    std::unordered_map<std::string, std::string> custom_settings_;
    ThemeType current_theme_ = ThemeType::Dark;

    std::string config_file_path_;
    std::string ini_file_path_ = "settings.ini";
    std::string profiles_directory_ = "profiles";
    std::string current_profile_name_;

    // Флаг синхронизации max_tokens и n_predict
    bool sync_max_tokens_enabled_ = true;

    // Вспомогательные методы
    void apply_theme_settings();
    void update_server_url();
    void determine_screen_resolution();
    void adapt_window_size_to_screen();
};

} // namespace core
} // namespace llama_gui

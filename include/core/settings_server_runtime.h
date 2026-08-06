#pragma once

#include <string>
#include <vector>

namespace llama_gui {
namespace core {

/**
 * @brief Настройки выполнения сервера (Server Runtime) для llama.cpp
 * 
 * Включает все параметры запуска сервера:
 * - Network (сеть: host, port, timeout)
 * - HTTP threads (потоки HTTP)
 * - Static files (статические файлы)
 * - API security (безопасность API)
 * - Features (функции: embeddings, reranking, metrics)
 * - Logging (логирование)
 */
struct ServerRuntimeSettings {
    // =========================================================================
    // Network (сетевые настройки)
    // =========================================================================
    
    /// Хост сервера (--host)
    std::string host = "127.0.0.1";
    
    /// Порт сервера (--port)
    int port = 8081;
    
    /// Таймаут запроса в секундах (-to, --timeout)
    int timeout = 600;
    
    /// Префикс API (--api-prefix)
    std::string api_prefix = "";
    
    /// Оффлайн режим (--offline)
    bool offline = false;

    // =========================================================================
    // HTTP threads (потоки HTTP)
    // =========================================================================
    
    /// Количество HTTP потоков (--threads-http), -1 = по умолчанию
    int threads_http = -1;

    // =========================================================================
    // Static files (статические файлы)
    // =========================================================================
    
    /// Путь к статическим файлам (--path)
    std::string static_path = "";
    
    /// Отключить WebUI (--no-webui)
    bool no_webui = false;

    // =========================================================================
    // API security (безопасность API)
    // =========================================================================
    
    /// API ключи (--api-key)
    std::vector<std::string> api_keys;
    
    /// Файл с API ключами (--api-key-file)
    std::string api_key_file = "";
    
    /// Файл SSL ключа (--ssl-key-file)
    std::string ssl_key_file = "";
    
    /// Файл SSL сертификата (--ssl-cert-file)
    std::string ssl_cert_file = "";

    // =========================================================================
    // Server binary (путь к бинарнику сервера)
    // =========================================================================

    /// Путь к llama-server бинарнику
    std::string server_binary_path = "llama-server";

    // =========================================================================
    // Features (функции)
    // =========================================================================

    /// Режим эмбеддингов (--embeddings)
    bool embeddings_mode = false;

    /// Режим реранжирования (--reranking)
    bool reranking_mode = false;

    /// Включить метрики (--metrics)
    bool metrics_enabled = false;

    // =========================================================================
    // KV-cache persistence (персистентность KV-cache)
    // =========================================================================

    /// Путь для сохранения KV-cache слотов (--slot-save-path)
    std::string slot_save_path = "";

    /// Количество параллельных слотов (-np, --parallel)
    int n_parallel = 4;

    /// Тип K cache (-ctk, --cache-type-k)
    std::string cache_type_k = "q8_0";

    /// Тип V cache (-ctv, --cache-type-v)
    std::string cache_type_v = "q8_0";

    /// Минимальный размер чанка для reuse (--cache-reuse)
    int cache_reuse = 0;

    // =========================================================================
    // Logging (логирование)
    // =========================================================================
    
    /// Отключить логирование (--log-disable)
    bool log_disabled = false;
    
    /// Файл лога (--log-file)
    std::string log_file = "";
    
    /// Цветной вывод логов (--log-colors)
    bool log_colors = false;
    
    /// Подробное логирование (-v, --verbose)
    bool log_verbose = false;
    
    /// Уровень детализации (-lv, --verbosity)
    int log_verbosity = 0;
    
    /// Формат лога (--log-format)
    std::string log_format = "json";
    
    /// Префикс в логе (--log-prefix)
    bool log_prefix = true;
    
    /// Метки времени в логе (--log-timestamps)
    bool log_timestamps = true;

    // =========================================================================
    // Методы валидации
    // =========================================================================
    
    /**
     * @brief Проверка корректности настроек
     * @return true если все параметры в допустимых пределах
     */
    bool validate() const {
        if (port < 1 || port > 65535) return false;
        if (timeout < 0) return false;
        if (threads_http < -1) return false;
        if (log_verbosity < 0 || log_verbosity > 3) return false;
        if (n_parallel < 1 || n_parallel > 64) return false;
        if (cache_reuse < 0) return false;
        return true;
    }

    /**
     * @brief Получить строку с ошибками валидации
     * @return Описание ошибок или пустая строка если ошибок нет
     */
    std::string get_validation_errors() const {
        std::string errors;

        if (port < 1 || port > 65535) {
            errors += "Port must be between 1 and 65535. ";
        }
        if (timeout < 0) {
            errors += "Timeout must be non-negative. ";
        }
        if (threads_http < -1) {
            errors += "HTTP threads must be -1 or greater. ";
        }
        if (log_verbosity < 0 || log_verbosity > 3) {
            errors += "Log verbosity must be between 0 and 3. ";
        }
        if (n_parallel < 1 || n_parallel > 64) {
            errors += "Parallel slots must be between 1 and 64. ";
        }
        if (cache_reuse < 0) {
            errors += "Cache reuse must be non-negative. ";
        }

        return errors;
    }

    // =========================================================================
    // Helper methods (вспомогательные методы)
    // =========================================================================
    
    /**
     * @brief Добавить API ключ
     * @param key API ключ
     */
    void add_api_key(const std::string& key) {
        api_keys.push_back(key);
    }

    /**
     * @brief Удалить API ключ по индексу
     * @param index Индекс ключа
     * @return true если успешно удален
     */
    bool remove_api_key(size_t index) {
        if (index >= api_keys.size()) return false;
        api_keys.erase(api_keys.begin() + index);
        return true;
    }

    /**
     * @brief Проверка наличия SSL конфигурации
     * @return true если настроены SSL ключ и сертификат
     */
    bool has_ssl_config() const {
        return !ssl_key_file.empty() && !ssl_cert_file.empty();
    }

    /**
     * @brief Проверка наличия API ключей
     * @return true если есть хотя бы один API ключ
     */
    bool has_api_keys() const {
        return !api_keys.empty();
    }

    /**
     * @brief Получить полный URL сервера
     * @return Строка формата "host:port"
     */
    std::string get_server_url() const {
        return host + ":" + std::to_string(port);
    }

    /**
     * @brief Проверка включённого логирования
     * @return true если логирование включено
     */
    bool is_logging_enabled() const {
        return !log_disabled;
    }
};

} // namespace core
} // namespace llama_gui

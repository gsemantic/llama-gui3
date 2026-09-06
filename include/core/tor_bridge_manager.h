#pragma once

#include <string>
#include <vector>
#include <functional>

namespace llama_gui {
namespace core {

/**
 * @brief Менеджер мостов Tor
 *
 * Управление мостами: чтение/запись torrc, валидация, обновление,
 * запрос новых мостов через BridgeDB.
 */
class TorBridgeManager {
public:
    struct BridgeInfo {
        std::string raw_line;     // Полная строка моста (для записи в torrc)
        std::string transport;    // Тип транспорта (obfs4, snowflake, meek)
        std::string address;      // IP:PORT
        std::string fingerprint;  // Отпечаток
        bool valid = false;       // Прошёл ли валидацию
        long latency_ms = 0;      // Задержка при валидации
    };

    TorBridgeManager() = default;

    /**
     * @brief Чтение текущих мостов из torrc
     */
    std::vector<BridgeInfo> read_bridges_from_torrc(const std::string& torrc_path = "/etc/tor/torrc");

    /**
     * @brief Запись мостов в torrc (заменяет все Bridge-строки)
     */
    bool write_bridges_to_torrc(const std::vector<BridgeInfo>& bridges,
                                const std::string& torrc_path = "/etc/tor/torrc");

    /**
     * @brief Запрос новых мостов через BridgeDB (через Tor)
     *
     * Использует BridgeDB HTML API для получения obfs4 мостов.
     * Требует работающий Tor на 127.0.0.1:9050.
     *
     * @param transport Тип транспорта ("obfs4", "snowflake", "meek")
     * @param count Количество мостов (0 = по умолчанию ~5)
     * @return Список полученных мостов
     */
    std::vector<BridgeInfo> fetch_bridges(const std::string& transport = "obfs4",
                                          int count = 0);

    /**
     * @brief Валидация моста: проверка соединения через SOCKS5
     *
     * Пытается подключиться через мост к указанному адресу.
     *
     * @param bridge Мост для проверки
     * @param test_url URL для теста (по умолчанию https://api.ipify.org)
     * @param timeout_ms Таймаут проверки
     * @return true если мост работает
     */
    bool validate_bridge(const BridgeInfo& bridge,
                         const std::string& test_url = "https://api.ipify.org",
                         int timeout_ms = 15000);

    /**
     * @brief Валидация всех мостов
     */
    void validate_all_bridges(std::vector<BridgeInfo>& bridges,
                              const std::string& test_url = "https://api.ipify.org",
                              int timeout_ms = 15000,
                              std::function<void(int current, int total)> progress = nullptr);

    /**
     * @brief Перезапуск Tor service
     */
    bool restart_tor();

    /**
     * @brief Проверка: запущен ли Tor и слушает ли он на порту
     */
    bool is_tor_running(const std::string& host = "127.0.0.1", int port = 9050);

    /**
     * @brief Получение IP через Tor (проверка работоспособности)
     */
    std::string get_tor_ip(const std::string& proxy_host = "127.0.0.1:9050",
                           int timeout_ms = 10000);

private:
    std::string torrc_path_ = "/etc/tor/torrc";
};

} // namespace core
} // namespace llama_gui

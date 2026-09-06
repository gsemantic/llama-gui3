#include "../../include/core/tor_bridge_manager.h"
#include "../../include/core/logger.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <regex>
#include <thread>
#include <chrono>
#include <curl/curl.h>
#include <cstdlib>

namespace llama_gui {
namespace core {

// ============================================================================
// CURL callbacks
// ============================================================================
namespace {

size_t write_string_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

// Парсинг obfs4 строки моста из BridgeDB HTML
// Формат: obfs4 IP:PORT FINGERPRINT cert=... iat-mode=...
std::vector<TorBridgeManager::BridgeInfo> parse_bridge_lines(const std::string& text) {
    std::vector<TorBridgeManager::BridgeInfo> result;

    // Ищем строки вида "obfs4 IP:PORT FINGERPRINT cert=..." в HTML/text
    std::regex bridge_re(R"((obfs4|snowflake|meeklite)\s+(\d+\.\d+\.\d+\.\d+:\d+)\s+([A-F0-9]{40})\s+(cert=\S+)\s+(iat-mode=\d))");
    std::sregex_iterator it(text.begin(), text.end(), bridge_re);
    std::sregex_iterator end;

    while (it != end) {
        TorBridgeManager::BridgeInfo info;
        info.transport = (*it)[1].str();
        info.address = (*it)[2].str();
        info.fingerprint = (*it)[3].str();
        // Собираем raw_line из всех групп
        info.raw_line = info.transport + " " + info.address + " " + info.fingerprint +
                        " " + (*it)[4].str() + " " + (*it)[5].str();
        // Декодируем HTML entities в cert
        std::string& raw = info.raw_line;
        // Заменяем &#43; на +
        size_t pos;
        while ((pos = raw.find("&#43;")) != std::string::npos) {
            raw.replace(pos, 5, "+");
        }
        result.push_back(std::move(info));
        ++it;
    }

    return result;
}

} // anonymous namespace

// ============================================================================
// Read bridges from torrc
// ============================================================================
std::vector<TorBridgeManager::BridgeInfo> TorBridgeManager::read_bridges_from_torrc(
    const std::string& torrc_path) {
    std::vector<BridgeInfo> bridges;
    std::ifstream file(torrc_path);
    if (!file.is_open()) {
        LOG_WARNING("TorBridgeManager: не удалось открыть " + torrc_path);
        return bridges;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Убираем комментарии и пробелы
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // Ищем строки Bridge
        if (trimmed.find("Bridge ") == 0) {
            std::string bridge_line = trimmed.substr(7); // убираем "Bridge "
            // Убираем ведущие пробелы
            bridge_line.erase(0, bridge_line.find_first_not_of(" \t"));

            BridgeInfo info;
            info.raw_line = bridge_line;

            // Парсим транспорт
            std::istringstream iss(bridge_line);
            iss >> info.transport;
            iss >> info.address;

            bridges.push_back(std::move(info));
        }
    }

    std::cout << "[TorBridgeManager] Найдено " << bridges.size() << " мостов в " << torrc_path << std::endl;
    return bridges;
}

// ============================================================================
// Write bridges to torrc (replaces Bridge lines, preserves other config)
// ============================================================================
bool TorBridgeManager::write_bridges_to_torrc(const std::vector<BridgeInfo>& bridges,
                                               const std::string& torrc_path) {
    // Читаем весь файл, заменяем Bridge-строки
    std::ifstream file(torrc_path);
    if (!file.is_open()) {
        LOG_ERROR("TorBridgeManager: не удалось открыть для записи " + torrc_path);
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    bool found_bridge_section = false;

    while (std::getline(file, line)) {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));

        // Пропускаем старые Bridge-строки и комментарии перед ними
        if (trimmed.find("Bridge ") == 0) {
            found_bridge_section = true;
            continue;
        }
        // Пропускаем пустые строки и комментарии между Bridge-строками
        if (found_bridge_section && (trimmed.empty() || trimmed[0] == '#')) {
            continue;
        }
        if (found_bridge_section && trimmed.find("Bridge ") != 0) {
            found_bridge_section = false;
        }
        lines.push_back(line);
    }
    file.close();

    // Добавляем UseBridges и ClientTransportPlugin если их нет
    bool has_usebridges = false;
    bool has_transport_plugin = false;
    for (const auto& l : lines) {
        std::string t = l;
        t.erase(0, t.find_first_not_of(" \t"));
        if (t.find("UseBridges") == 0) has_usebridges = true;
        if (t.find("ClientTransportPlugin") == 0) has_transport_plugin = true;
    }

    // Добавляем перед мостами
    if (!has_usebridges) {
        lines.push_back("UseBridges 1");
    }
    if (!has_transport_plugin) {
        lines.push_back("ClientTransportPlugin obfs4 exec /usr/bin/obfs4proxy managed");
    }

    // Добавляем мосты
    for (const auto& bridge : bridges) {
        lines.push_back("Bridge " + bridge.raw_line);
    }

    // Записываем обратно
    std::ofstream out(torrc_path);
    if (!out.is_open()) {
        LOG_ERROR("TorBridgeManager: не удалось записать " + torrc_path);
        return false;
    }

    for (const auto& l : lines) {
        out << l << "\n";
    }

    std::cout << "[TorBridgeManager] Записано " << bridges.size() << " мостов в " << torrc_path << std::endl;
    return true;
}

// ============================================================================
// Fetch bridges from BridgeDB
// ============================================================================
std::vector<TorBridgeManager::BridgeInfo> TorBridgeManager::fetch_bridges(
    const std::string& transport, int count) {

    std::cout << "[TorBridgeManager] Запрос мостов через BridgeDB (transport=" << transport << ")" << std::endl;

    // Формируем URL для BridgeDB
    std::string url = "https://bridges.torproject.org/bridges?transport=" + transport;

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("TorBridgeManager: не удалось инициализировать CURL");
        return {};
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0");

    // Используем Tor SOCKS5 прокси
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Пробуем через Tor, если доступен
    curl_easy_setopt(curl, CURLOPT_PROXY, "socks5h://127.0.0.1:9050");
    curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        std::cerr << "[TorBridgeManager] BridgeDB запрос не удался: curl="
                  << curl_easy_strerror(res) << ", HTTP " << http_code << std::endl;

        // Фолбэк: пробуем без Tor (прямое подключение)
        curl = curl_easy_init();
        if (!curl) return {};

        response.clear();
        headers = nullptr;
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK || http_code != 200) {
            std::cerr << "[TorBridgeManager] Прямой запрос тоже не удался" << std::endl;
            return {};
        }
    }

    // Парсим мосты из HTML
    auto bridges = parse_bridge_lines(response);

    // Ограничиваем количество
    if (count > 0 && (int)bridges.size() > count) {
        bridges.resize(count);
    }

    std::cout << "[TorBridgeManager] Получено " << bridges.size() << " мостов" << std::endl;
    return bridges;
}

// ============================================================================
// Validate single bridge
// ============================================================================
bool TorBridgeManager::validate_bridge(const BridgeInfo& bridge,
                                       const std::string& test_url,
                                       int timeout_ms) {
    // Создаём obfs4proxy managed-connection через SOCKS
    // Простой тест: подключаемся через мост к test_url
    // Для этого используем Tor ControlPort или obfs4proxy напрямую

    // Упрощённая валидация: пытаемся подключиться к адресу моста
    std::string addr = bridge.address;
    auto colon = addr.find(':');
    if (colon == std::string::npos) return false;

    std::string host = addr.substr(0, colon);
    int port = std::atoi(addr.substr(colon + 1).c_str());

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    // Тестируем TCP-соединение с мостом
    curl_easy_setopt(curl, CURLOPT_URL, ("http://" + host + ":" + std::to_string(port)).c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    auto t0 = std::chrono::steady_clock::now();
    CURLcode res = curl_easy_perform(curl);
    auto t1 = std::chrono::steady_clock::now();

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    long latency = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // TCP-соединение установлено = мост доступен
    // (obfs4proxy может отклонить, но это уже проверяется через Tor)
    bool reachable = (res == CURLE_OK || res == CURLE_HTTP_RETURNED_ERROR);

    std::cout << "[TorBridgeManager] Bridge " << bridge.address
              << " - " << (reachable ? "REACHABLE" : "UNREACHABLE")
              << " (" << latency << "ms)" << std::endl;

    return reachable;
}

// ============================================================================
// Validate all bridges
// ============================================================================
void TorBridgeManager::validate_all_bridges(
    std::vector<BridgeInfo>& bridges,
    const std::string& test_url,
    int timeout_ms,
    std::function<void(int, int)> progress) {

    int total = bridges.size();
    for (int i = 0; i < total; i++) {
        if (progress) progress(i + 1, total);

        bridges[i].valid = validate_bridge(bridges[i], test_url, timeout_ms);
    }
}

// ============================================================================
// Restart Tor
// ============================================================================
bool TorBridgeManager::restart_tor() {
    std::cout << "[TorBridgeManager] Перезапуск Tor..." << std::endl;

    // Проверяем PID файл
    std::string pid_file = "/var/run/tor/tor.pid";
    std::ifstream pf(pid_file);
    int pid = -1;
    if (pf.is_open()) {
        pf >> pid;
        pf.close();
    }

    if (pid > 0) {
        // Останавливаем
        std::string stop_cmd = "kill " + std::to_string(pid) + " 2>/dev/null";
        int ret = std::system(stop_cmd.c_str());
        if (ret == 0) {
            std::cout << "[TorBridgeManager] Tor остановлен (PID " << pid << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    // Запускаем заново
    int ret = std::system("tor --runasdaemon 1 2>/dev/null");
    if (ret != 0) {
        // Пробуем альтернативный способ
        ret = std::system("service tor restart 2>/dev/null || systemctl restart tor 2>/dev/null || tor &");
    }

    // Ждём пока Tor поднимется
    for (int i = 0; i < 10; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (is_tor_running()) {
            std::cout << "[TorBridgeManager] Tor запущен и слушает" << std::endl;
            return true;
        }
    }

    LOG_WARNING("TorBridgeManager: Tor не удалось запустить за 10 секунд");
    return false;
}

// ============================================================================
// Check if Tor is running
// ============================================================================
bool TorBridgeManager::is_tor_running(const std::string& host, int port) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = "http://" + host + ":" + std::to_string(port);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}

// ============================================================================
// Get Tor IP
// ============================================================================
std::string TorBridgeManager::get_tor_ip(const std::string& proxy_host, int timeout_ms) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    std::string proxy = "socks5h://" + proxy_host;

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.ipify.org");
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && !response.empty()) {
        return response;
    }
    return "";
}

} // namespace core
} // namespace llama_gui

#pragma once

#include <string>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace llama_gui {
namespace core {

/**
 * @brief Извлечь хост из URL (scheme://host[:port]/path,UserInfo,IPv6 в скобках)
 */
inline std::string url_host(const std::string& url) {
    std::string rest = url;
    size_t p = rest.find("://");
    if (p != std::string::npos) rest = rest.substr(p + 3);
    size_t end = rest.find_first_of("/?#");
    if (end != std::string::npos) rest = rest.substr(0, end);
    size_t at = rest.rfind('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    if (!rest.empty() && rest.front() == '[') {
        size_t rb = rest.find(']');
        return rb == std::string::npos ? rest.substr(1) : rest.substr(1, rb - 1);
    }
    size_t colon = rest.rfind(':');
    if (colon != std::string::npos) rest = rest.substr(0, colon);
    return rest;
}

/**
 * @brief true если URL указывает на локальный (loopback) адрес.
 *
 * Используется для обхода SOCKS/Tor-прокси: Tor не может (и не должен)
 * ходить в localhost — запросы к локальному прокси-серверу (например,
 * gpt2giga на http://localhost:8090) должны идти напрямую.
 */
inline bool url_is_loopback(const std::string& url) {
    std::string host = url_host(url);
    for (auto& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (host == "localhost" || host == "::1" || host == "0.0.0.0" ||
        host.rfind("127.", 0) == 0 || host.rfind("::ffff:127.", 0) == 0) {
        return true;
    }
    return false;
}

/**
 * @brief Проверка, свободен ли TCP-порт (bind-тест)
 * @param port Порт для проверки
 * @param host Хост привязки (адрес или имя; для "localhost"/имён используется INADDR_ANY)
 * @return true если порт уже занят
 */
inline bool is_port_in_use(int port, const std::string& host) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    int rc = bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    int saved_errno = errno;
    close(fd);
    if (rc == 0) {
        return false; // порт свободен
    }
    return (saved_errno == EADDRINUSE);
}

/**
 * @brief Найти первый свободный порт, начиная с base
 * @param base Начальный порт
 * @param host Хост привязки
 * @return Свободный порт или -1, если не найден в диапазоне base..base+99
 */
inline int find_free_port(int base, const std::string& host) {
    for (int port = base; port < base + 100; ++port) {
        if (!is_port_in_use(port, host)) {
            return port;
        }
    }
    return -1;
}

} // namespace core
} // namespace llama_gui

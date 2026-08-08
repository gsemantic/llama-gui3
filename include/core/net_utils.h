#pragma once

#include <string>
#include <cstdint>
#include <cerrno>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace llama_gui {
namespace core {

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

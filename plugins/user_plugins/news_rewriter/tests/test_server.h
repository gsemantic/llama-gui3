#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// Минимальный локальный HTTP-сервер для тестов (Linux).
// Отвечает одним ответом на один запрос.
namespace news_rewriter_test {

class MiniHttpServer {
public:
    ~MiniHttpServer() { stop(); }

    // Запускает сервер на 127.0.0.1:0 (эпhemeral порт).
    bool start(int status, const std::string& body,
               const std::string& content_type = "text/plain",
               int delay_ms = 0) {
        status_ = status;
        body_ = body;
        content_type_ = content_type;
        delay_ms_ = delay_ms;

        listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock_ < 0) return false;
        int yes = 1;
        setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            return false;
        }
        socklen_t len = sizeof(addr);
        if (getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
            return false;
        }
        port_ = ntohs(addr.sin_port);
        if (listen(listen_sock_, 1) < 0) return false;

        running_ = true;
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    int port() const { return port_; }
    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    void stop() {
        running_ = false;
        if (listen_sock_ >= 0) {
            shutdown(listen_sock_, SHUT_RDWR);
            close(listen_sock_);
            listen_sock_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    void serve() {
        sockaddr_in client{};
        socklen_t clen = sizeof(client);
        const int c = accept(listen_sock_, reinterpret_cast<sockaddr*>(&client), &clen);
        if (c < 0) return;
        if (delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        char buf[4096];
        (void)recv(c, buf, sizeof(buf), 0);

        const char* reason = (status_ >= 400) ? " Error" : " OK";
        const std::string resp =
            "HTTP/1.1 " + std::to_string(status_) + reason + "\r\n"
            "Content-Type: " + content_type_ + "\r\n"
            "Content-Length: " + std::to_string(body_.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body_;
        send(c, resp.data(), resp.size(), 0);
        close(c);
    }

    int listen_sock_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int status_ = 200;
    std::string body_;
    std::string content_type_ = "text/plain";
    int delay_ms_ = 0;
};

} // namespace news_rewriter_test

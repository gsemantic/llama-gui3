#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// Минимальный локальный HTTP-сервер для тестов (Linux).
// Обслуживает несколько соединений, копирует запросы (последний доступен через
// last_request()) и считает их (request_count()).
namespace news_rewriter_test {

class MiniHttpServer {
public:
    ~MiniHttpServer() { stop(); }

    // Запускает сервер на 127.0.0.1:0 (эephmeral порт).
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
        if (listen(listen_sock_, 32) < 0) return false;

        running_ = true;
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    // Дополнительные маршруты: путь → тело. Основной body_ остаётся fallback'ом.
    void add_route(const std::string& path, const std::string& body,
                   const std::string& content_type = "text/plain") {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        routes_[path] = Route{body, content_type};
    }

    // Маршрут для конкретного метода (GET/POST): приоритетнее add_route.
    // Нужен, когда GET-поиск и POST-создание идут на один путь (wp/v2/posts).
    void add_route_method(const std::string& method, const std::string& path,
                          const std::string& body,
                          const std::string& content_type = "text/plain") {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        method_routes_[method + " " + path] = Route{body, content_type};
    }

    int port() const { return port_; }
    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    // Сырые байты всех принятых запросов (заголовки + тело).
    std::string last_request() const {
        std::lock_guard<std::mutex> lock(req_mutex_);
        return request_;
    }

    int request_count() const { return request_count_.load(); }

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
    struct Route {
        std::string body;
        std::string content_type;
    };

    std::string request_path(const std::string& req) const {
        const std::size_t start = req.find(' ');
        if (start == std::string::npos) return "/";
        const std::size_t end = req.find(' ', start + 1);
        if (end == std::string::npos) return "/";
        std::string p = req.substr(start + 1, end - start - 1);
        const std::size_t q = p.find('?');
        if (q != std::string::npos) p = p.substr(0, q);
        return p;
    }

    void serve() {
        for (int n = 0; running_ && n < 64; n++) {
            sockaddr_in client{};
            socklen_t clen = sizeof(client);
            const int c = accept(listen_sock_, reinterpret_cast<sockaddr*>(&client), &clen);
            if (c < 0) break;
            request_count_++;

            // Читаем весь запрос (таймаут чтения 2 с).
            timeval tv{2, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::string req;
            char buf[4096];
            for (;;) {
                const ssize_t nrecv = recv(c, buf, sizeof(buf), 0);
                if (nrecv > 0) {
                    req.append(buf, static_cast<std::size_t>(nrecv));
                } else {
                    break;   // 0 = соединение закрыто, -1 = таймаут/ошибка
                }
            }
            {
                std::lock_guard<std::mutex> lock(req_mutex_);
                request_ += req;
            }

            // Выбираем ответ: метод+путь, затем путь, затем fallback-тело.
            std::string body = body_;
            std::string ctype = content_type_;
            int status = status_;
            {
                std::lock_guard<std::mutex> lock(routes_mutex_);
                const std::string method =
                    req.substr(0, req.find(' '));
                const auto im = method_routes_.find(method + " " +
                                                    request_path(req));
                if (im != method_routes_.end()) {
                    body = im->second.body;
                    ctype = im->second.content_type;
                    status = 200;
                } else {
                    const auto it = routes_.find(request_path(req));
                    if (it != routes_.end()) {
                        body = it->second.body;
                        ctype = it->second.content_type;
                        status = 200;
                    }
                }
            }

            if (delay_ms_ > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
            }

            const char* reason = (status >= 400) ? " Error" : " OK";
            const std::string resp =
                "HTTP/1.1 " + std::to_string(status) + reason + "\r\n"
                "Content-Type: " + ctype + "\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body;
            send(c, resp.data(), resp.size(), 0);
            close(c);
        }
    }

    int listen_sock_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int status_ = 200;
    std::string body_;
    std::string content_type_ = "text/plain";
    int delay_ms_ = 0;
    std::string request_;
    mutable std::mutex req_mutex_;
    std::atomic<int> request_count_{0};
    std::map<std::string, Route> routes_;
    std::map<std::string, Route> method_routes_;
    mutable std::mutex routes_mutex_;
};

} // namespace news_rewriter_test

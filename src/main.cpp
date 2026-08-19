#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <csignal>
#include <atomic>
#include <thread>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "core/llama_interface.h"
#include "core/settings.h"
#include "core/version.h"
#include "core/state_manager.h"
#include "core/rag_manager.h"
#include "core/logger.h"
#include "core/server_manager.h"
#include "core/net_utils.h"
#include "core/cloud_proxy.h"
#include "ui/main_window.h"
// #include "agents/agents.h"  // ОТКЛЮЧЕНО: агенты временно отключены

using namespace llama_gui::core;
using namespace llama_gui::ui;
// using namespace agents;  // ОТКЛЮЧЕНО: агенты временно отключены

// =========================================================================
// Single-instance guard
// =========================================================================

// Путь к lock-файлу в пользовательской директории данных (~/.llama-gui/)
static std::string get_lock_file_path() {
    const char* home = getenv("HOME");
    if (!home) {
        home = ".";
    }
    return std::string(home) + "/.llama-gui/llama-gui.lock";
}

// Дескриптор lock-файла. Держим открытым весь цикл работы приложения:
// ядро ОС автоматически снимает flock при завершении процесса (в т.ч. при
// падении), поэтому устаревших блокировок не возникает.
static int g_lock_fd = -1;

// Пытаемся захватить эксклюзивную блокировку. Возвращает false, если другой
// экземпляр приложения уже запущен.
static bool acquire_single_instance_lock() {
    std::string lock_path = get_lock_file_path();

    try {
        std::filesystem::create_directories(std::filesystem::path(lock_path).parent_path());
    } catch (const std::exception&) {
        // Не блокируем запуск, если директорию создать не удалось
    }

    g_lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (g_lock_fd == -1) {
        std::cerr << "Llama GUI: не удалось открыть lock-файл: " << lock_path << std::endl;
        return true;
    }

    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        std::cerr << "Llama GUI: приложение уже запущено (допускается только один экземпляр)." << std::endl;
        std::cerr << "Lock-файл: " << lock_path << std::endl;
        close(g_lock_fd);
        g_lock_fd = -1;
        return false;
    }

    // Записываем PID запущенного экземпляра для диагностики
    ftruncate(g_lock_fd, 0);
    std::string pid_str = std::to_string(getpid());
    write(g_lock_fd, pid_str.c_str(), pid_str.size());

    return true;
}

// =========================================================================
// Headless (server-only) mode
// =========================================================================

struct HeadlessOptions {
    bool enabled = false;
    bool cloud = false;
    bool auto_port = false;
    bool gui_proxy = false;   // GUI-режим: поднять облачный прокси в фоне
    std::string host;
    int port = 0;            // 0 = использовать из настроек
    std::string model_path;
    std::string profile;
    std::string endpoint_url; // cloud: переопределить endpoint провайдера
    std::string api_key;      // cloud: переопределить API-ключ
};

static std::atomic<bool> g_stop_requested{false};
static std::atomic<bool> g_restart_requested{false};

static void headless_signal_handler(int sig) {
    if (sig == SIGHUP) {
        g_restart_requested.store(true);
    } else {
        g_stop_requested.store(true);
    }
}

static void print_headless_help() {
    std::cout << "Серверный режим (без GUI) приложения llama-gui:" << std::endl;
    std::cout << "  --headless | -s             Запустить llama-server без GUI (endpoint для внешних клиентов)" << std::endl;
    std::cout << "                              (Это режим САМОГО приложения, НЕ headless-браузер!)" << std::endl;
    std::cout << "                              Для рендеринга веб-страниц через Chromium используйте" << std::endl;
    std::cout << "                              панель «Headless-браузер» в основном окне или агента web_render_agent." << std::endl;
    std::cout << "  --cloud                     Облачный прокси: endpoint пересылает запросы в облачного" << std::endl;
    std::cout << "                              провайдера (OpenRouter и пр.) с настройками профиля + RAG" << std::endl;
    std::cout << "  --proxy                     (GUI-режим) поднять облачный прокси в фоне на порту из" << std::endl;
    std::cout << "                              настроек server_runtime — для внешних клиентов (qwen cli)" << std::endl;
    std::cout << "  --host=HOST                  Адрес привязки (по умолчанию из настроек, обычно 127.0.0.1)" << std::endl;
    std::cout << "                               Для доступа из LAN: --host=0.0.0.0" << std::endl;
    std::cout << "  --port=PORT                  Порт (по умолчанию из настроек, 8081)" << std::endl;
    std::cout << "  --auto-port                  Автоматически найти свободный порт, если занят" << std::endl;
    std::cout << "  --model=PATH                 Путь к модели (по умолчанию из активного профиля; local)" << std::endl;
    std::cout << "  --profile=NAME               Профиль настроек для запуска" << std::endl;
    std::cout << "  Управление: Ctrl+C / SIGTERM - остановка, SIGHUP - перезапуск (local)" << std::endl;
}

static int run_headless(Settings& settings, const HeadlessOptions& opts) {
    // Определяем host и port (приоритет: CLI > настройки > дефолт)
    std::string host = opts.host.empty() ? settings.server_runtime().host : opts.host;
    if (host.empty()) {
        host = "127.0.0.1";
    }
    int port = opts.port > 0 ? opts.port : settings.server_runtime().port;
    if (port <= 0) {
        port = 8081;
    }

    // Авто-поиск свободного порта
    if (opts.auto_port || llama_gui::core::is_port_in_use(port, host)) {
        int free_port = llama_gui::core::find_free_port(port, host);
        if (free_port < 0) {
            LOG_ERROR("Не удалось найти свободный порт, начиная с " + std::to_string(port));
            return 1;
        }
        if (free_port != port) {
            std::cout << "Порт " << port << " занят, использую свободный порт " << free_port << std::endl;
            port = free_port;
        }
    }

    // Модель (приоритет: CLI > профиль)
    std::string model_path = opts.model_path.empty() ? settings.get_model_path() : opts.model_path;
    if (model_path.empty()) {
        LOG_ERROR("Не указана модель. Укажите --model=PATH или задайте модель в профиле/настройках.");
        return 1;
    }
    if (!std::filesystem::is_regular_file(model_path)) {
        LOG_ERROR("Файл модели не найден: " + model_path);
        std::cout << "Укажите существующий путь через --model=PATH (например .gguf)" << std::endl;
        return 1;
    }

    ServerManager server(settings);
    server.set_host_port(host, port);
    server.set_model_path(model_path);

    std::signal(SIGINT, headless_signal_handler);
    std::signal(SIGTERM, headless_signal_handler);
    std::signal(SIGHUP, headless_signal_handler);

    std::cout << "======================================================" << std::endl;
    std::cout << "  БЕЗГОЛОВЫЙ РЕЖИМ (headless server)" << std::endl;
    std::cout << "  Модель: " << model_path << std::endl;
    std::cout << "  Хост:   " << host << std::endl;
    std::cout << "  Порт:   " << port << std::endl;
    std::cout << "======================================================" << std::endl;

    if (!server.start_server()) {
        LOG_ERROR("Не удалось запустить сервер");
        return 1;
    }

    // Ожидание готовности (до 120 секунд)
    int wait_loops = 0;
    while (!server.is_server_ready() && !g_stop_requested.load() && wait_loops < 120) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++wait_loops;
    }

    std::string url = server.get_server_url();
    if (server.is_server_ready()) {
        std::cout << "\n======================================================" << std::endl;
        std::cout << "  ENDPOINT ГОТОВ (OpenAI-совместимый API)" << std::endl;
        std::cout << "  Base URL:             " << url << "/v1" << std::endl;
        std::cout << "  Chat completions:     POST " << url << "/v1/chat/completions" << std::endl;
        std::cout << "  Models list:          GET  " << url << "/v1/models" << std::endl;
        std::cout << "  Embeddings:           POST " << url << "/v1/embeddings" << std::endl;
        std::cout << "  Health:               GET  " << url << "/health" << std::endl;
        std::cout << "  Подключение (qwen cli / OpenAI SDK): base_url = " << url << "/v1" << std::endl;
        std::cout << "======================================================" << std::endl;
        std::cout << "  Ctrl+C / SIGTERM - остановка, SIGHUP - перезапуск" << std::endl;
        std::cout << "======================================================\n" << std::endl;
    } else {
        std::cout << "⚠ Сервер не ответил на health-запрос за отведённое время." << std::endl;
        std::cout << "  Проверьте лог llama-server выше. Endpoint: " << url << "/v1" << std::endl;
    }

    // Основной цикл ожидания сигналов
    while (!g_stop_requested.load()) {
        if (g_restart_requested.exchange(false)) {
            std::cout << "SIGHUP: перезапуск сервера..." << std::endl;
            server.restart_server();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nОстановка сервера..." << std::endl;
    server.stop_server(true);
    std::cout << "Сервер остановлен. Выход." << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    bool debug_mode = false;
    bool show_help = false;
    std::string server_url = "http://localhost:8081";
    HeadlessOptions headless;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--debug" || arg == "-d") {
            debug_mode = true;
        } else if (arg == "--headless" || arg == "-s" || arg == "--server") {
            headless.enabled = true;
        } else if (arg == "--cloud" || arg == "-cld") {
            headless.cloud = true;
        } else if (arg == "--proxy") {
            headless.gui_proxy = true;
        } else if (arg.find("--host=") == 0) {
            headless.host = arg.substr(7);
        } else if (arg.find("--port=") == 0) {
            headless.port = std::atoi(arg.substr(7).c_str());
        } else if (arg == "--auto-port") {
            headless.auto_port = true;
        } else if (arg.find("--model=") == 0) {
            headless.model_path = arg.substr(8);
        } else if (arg.find("--profile=") == 0) {
            headless.profile = arg.substr(10);
        } else if (arg.find("--endpoint=") == 0) {
            headless.endpoint_url = arg.substr(11);
        } else if (arg.find("--api-key=") == 0) {
            headless.api_key = arg.substr(10);
        } else if (arg == "--help" || arg == "-h") {
            show_help = true;
        } else if (arg.find("--url=") == 0) {
            server_url = arg.substr(6);
        } else if (i + 1 < argc && arg != "--debug" && arg != "-d") {
            // Positional argument (URL)
            server_url = arg;
        }
    }

    if (show_help) {
        print_headless_help();
        return 0;
    }
    
    // Initialize logger with mode from settings
    Logger::instance().set_debug_mode(debug_mode);
    
    std::cout << "======================================================" << std::endl;
    std::cout << "        Llama.cpp C++ GUI - ЗАПУСК" << std::endl;
    std::cout << "======================================================" << std::endl;
    std::cout << "  Версия: " << llama_gui::core::getVersionFull() << std::endl;
    std::cout << "  Сборка: " << llama_gui::core::getBuildDate() << " " << llama_gui::core::getBuildTime() << std::endl;
    std::cout << "  Git:    " << llama_gui::core::getGitCommitHash() << std::endl;
    std::cout << "======================================================" << std::endl;
    if (debug_mode) {
        std::cout << "🔧 РЕЖИМ ОТЛАДКИ включён" << std::endl;
        std::cout << "======================================================" << std::endl;
    }
    std::cout << "" << std::endl;

    // =========================================================================
    // Headless (server-only) mode: запуск llama-server без GUI
    // =========================================================================
    if (headless.enabled || headless.cloud) {
        try {
            Settings settings;
            settings.synchronize_at_startup();

            // Загрузка указанного профиля, если задан
            if (!headless.profile.empty()) {
                if (!settings.load_profile(headless.profile)) {
                    LOG_ERROR("Профиль не найден: " + headless.profile);
                    return 1;
                }
                std::cout << "Профиль: " << headless.profile << std::endl;
            }

            if (headless.cloud) {
                CloudProxyOptions proxy_opts;
                proxy_opts.host = headless.host;
                proxy_opts.port = headless.port;
                proxy_opts.auto_port = headless.auto_port;
                proxy_opts.endpoint_url = headless.endpoint_url;
                proxy_opts.api_key = headless.api_key;
                return run_cloud_proxy(settings, proxy_opts);
            }

            return run_headless(settings, headless);
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("Ошибка в безголовом режиме: ") + e.what());
            return 1;
        }
    }

    // Проверка single-instance: если другой экземпляр уже запущен — завершаемся
    if (!acquire_single_instance_lock()) {
        return 1;
    }

    try {
        // Инициализация core компонентов
        LOG_INFO("Инициализация core компонентов...");

        Settings settings;
        // Явная синхронизация настроек при старте
        // Приоритет: profiles > settings.ini > настройки по умолчанию
        settings.synchronize_at_startup();
        LOG_INFO("Settings инициализированы");

        LlamaInterface llama_interface;
        if (argc > 1 && server_url != "http://localhost:8081") {
            // URL from command line
        } else {
            server_url = settings.get_server_url();
        }

        if (llama_interface.initialize(server_url)) {
            LOG_INFO("LlamaInterface подключен к: " + server_url);
        } else {
            LOG_WARNING("LlamaInterface в режиме заглушки");
        }

        StateManager state_manager;
        state_manager.initialize(settings);
        LOG_INFO("StateManager инициализирован");

        std::cout << "" << std::endl;

        // Облачный прокси в фоне (--proxy): endpoint для внешних клиентов
        std::atomic<bool> proxy_stop{false};
        std::thread proxy_thread;
        if (headless.gui_proxy) {
            LOG_INFO("Запуск облачного прокси в фоне (--proxy)...");
            CloudProxyOptions proxy_opts;
            proxy_opts.host = headless.host;
            proxy_opts.port = headless.port;
            proxy_opts.auto_port = headless.auto_port;
            proxy_opts.endpoint_url = headless.endpoint_url;
            proxy_opts.api_key = headless.api_key;
            proxy_thread = start_cloud_proxy_in_thread(settings, proxy_opts, proxy_stop);
        }

        // Создание и запуск GUI приложения
        LOG_INFO("Запуск GUI приложения...");

        // Создание главного окна с автоматическим определением размера
        try {
            MainWindow main_window(state_manager, settings, llama_interface);
            if (main_window.initialize(0, 0)) { // 0,0 означает автоопределение размера
                LOG_INFO("MainWindow инициализирован");

                // Запуск главного цикла приложения
                LOG_INFO("Запуск главного цикла приложения...");
                main_window.run();

            } else {
                LOG_ERROR("Ошибка инициализации MainWindow");
                if (proxy_thread.joinable()) {
                    proxy_stop.store(true);
                    proxy_thread.join();
                }
                return 1;
            }
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("Ошибка при инициализации MainWindow: ") + e.what());
            if (proxy_thread.joinable()) {
                proxy_stop.store(true);
                proxy_thread.join();
            }
            return 1;
        }

        // Остановка фонового прокси после закрытия GUI
        if (proxy_thread.joinable()) {
            proxy_stop.store(true);
            proxy_thread.join();
            LOG_INFO("Фоновый облачный прокси остановлен");
        }

    } catch (const std::exception& e) {
        LOG_ERROR(std::string("Ошибка: ") + e.what());
        return 1;
    }

    std::cout << "" << std::endl;
    std::cout << "======================================================" << std::endl;
    std::cout << "           Приложение завершено" << std::endl;
    std::cout << "======================================================" << std::endl;

    return 0;
}

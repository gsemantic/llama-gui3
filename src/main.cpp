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
#include "core/llama_interface.h"
#include "core/settings.h"
#include "core/version.h"
#include "core/state_manager.h"
#include "core/rag_manager.h"
#include "core/logger.h"
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

int main(int argc, char* argv[]) {
    // Parse command line arguments
    bool debug_mode = false;
    std::string server_url = "http://localhost:8081";
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--debug" || arg == "-d") {
            debug_mode = true;
        } else if (arg.find("--url=") == 0) {
            server_url = arg.substr(6);
        } else if (i + 1 < argc && arg != "--debug" && arg != "-d") {
            // Positional argument (URL)
            server_url = arg;
        }
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
                return 1;
            }
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("Ошибка при инициализации MainWindow: ") + e.what());
            return 1;
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

#!/bin/bash

# Llama GUI - Скрипт запуска
# Простой запуск GUI с различными опциями

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
GUI_BINARY="$BUILD_DIR/llama-gui-core"

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Функция для вывода цветного текста
print_colored() {
    local color=$1
    shift
    echo -e "${color}$@${NC}"
}

# Функция справки
show_help() {
    echo "======================================================"
    print_colored $BLUE "     Llama GUI - Скрипт запуска"
    echo "======================================================"
    echo ""
    echo "Использование: $0 [ОПЦИИ]"
    echo ""
    echo "ОПЦИИ:"
    echo "  -h, --help              Показать эту справку"
    echo "  -l, --local             Запустить с локальным сервером (localhost:8081)"
    echo "  -c, --custom URL        Запустить с пользовательским URL сервера"
    echo "  -d, --debug             Запуск в debug режиме"
    echo "  -s, --status            Показать статус серверов"
    echo "  -k, --kill              Остановить все llama-server процессы"
    echo "  -r, --rebuild           Пересобрать проект перед запуском"
    echo ""
    echo "ПРИМЕРЫ:"
    echo "  $0 -l                   # Запуск с локальным сервером"
    echo "  $0 -c http://192.168.1.100:8081  # Запуск с удаленным сервером"
    echo "  $0 -r -l                # Пересборка + запуск с локальным сервером"
    echo ""
    echo "СЕРВЕР:"
    echo "  По умолчанию используется: localhost:8081"
    echo "  Путь к llama-server: /home/Alex/projects/llama-b7472-bin-ubuntu-x64/llama-b7472/llama-server"
    echo "======================================================"
}

# Функция проверки сборки
check_build() {
    if [ ! -f "$GUI_BINARY" ]; then
        print_colored $RED "❌ Исполняемый файл не найден: $GUI_BINARY"
        echo ""
        print_colored $YELLOW "🔧 Запускаю пересборку проекта..."
        ./build.sh
        echo ""
        if [ ! -f "$GUI_BINARY" ]; then
            print_colored $RED "❌ Сборка не удалась. Проверьте ошибки выше."
            exit 1
        fi
    fi
    
    print_colored $GREEN "✅ Исполняемый файл найден: $GUI_BINARY"
}

# Функция показа статуса серверов
show_status() {
    echo "======================================================"
    print_colored $BLUE "     СТАТУС СЕРВЕРОВ"
    echo "======================================================"
    
    # Проверяем процессы llama-server
    if pgrep -f llama-server > /dev/null; then
        print_colored $GREEN "✅ Найдены активные llama-server процессы:"
        ps aux | grep llama-server | grep -v grep
    else
        print_colored $RED "❌ Активные llama-server процессы не найдены"
    fi
    
    echo ""
    
    # Проверяем порт 8081
    if lsof -i :8081 > /dev/null 2>&1; then
        print_colored $GREEN "✅ Порт 8081 занят:"
        lsof -i :8081
    else
        print_colored $RED "❌ Порт 8081 свободен"
    fi
    
    echo ""
    print_colored $BLUE "Путь к llama-server: /home/Alex/projects/llama-b7472-bin-ubuntu-x64/llama-b7472/llama-server"
    
    if [ -f "/home/Alex/projects/llama-b7472-bin-ubuntu-x64/llama-b7472/llama-server" ]; then
        print_colored $GREEN "✅ Файл llama-server существует"
    else
        print_colored $RED "❌ Файл llama-server не найден"
    fi
}

# Функция остановки серверов
kill_servers() {
    echo "======================================================"
    print_colored $BLUE "     ОСТАНОВКА СЕРВЕРОВ"
    echo "======================================================"
    
    if pgrep -f llama-server > /dev/null; then
        print_colored $YELLOW "🛑 Останавливаю llama-server процессы..."
        pkill -f llama-server
        sleep 2
        
        if pgrep -f llama-server > /dev/null; then
            print_colored $YELLOW "⚠ Принудительная остановка..."
            pkill -9 -f llama-server
        fi
        
        print_colored $GREEN "✅ Серверы остановлены"
    else
        print_colored $GREEN "✅ Активные серверы не найдены"
    fi
    
    # Освобождаем порт 8081
    if lsof -i :8081 > /dev/null 2>&1; then
        print_colored $YELLOW "🛑 Освобождаю порт 8081..."
        fuser -k 8081/tcp 2>/dev/null || true
        print_colored $GREEN "✅ Порт 8081 освобожден"
    fi
}

# Функция пересборки
rebuild_project() {
    print_colored $YELLOW "🔧 Пересборка проекта..."
    ./build.sh
    echo ""
    check_build
}

# Основная логика
main() {
    local server_url=""
    local need_rebuild=false
    local run_mode="local"
    
    # Парсинг аргументов
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -l|--local)
                run_mode="local"
                shift
                ;;
            -c|--custom)
                run_mode="custom"
                server_url="$2"
                shift 2
                ;;
            -d|--debug)
                print_colored $YELLOW "🔧 Debug режим - пересборка в Debug конфигурации"
                need_rebuild=true
                run_mode="local"
                shift
                ;;
            -s|--status)
                show_status
                exit 0
                ;;
            -k|--kill)
                kill_servers
                exit 0
                ;;
            -r|--rebuild)
                need_rebuild=true
                shift
                ;;
            *)
                print_colored $RED "❌ Неизвестная опция: $1"
                echo "Используйте $0 --help для справки"
                exit 1
                ;;
        esac
    done
    
    echo "======================================================"
    print_colored $BLUE "     Llama GUI - Запуск"
    echo "======================================================"
    echo ""
    
    # Проверка и пересборка если нужно
    if [ "$need_rebuild" = true ]; then
        rebuild_project
    else
        check_build
    fi
    
    # Подготовка команды запуска
    local run_command="$GUI_BINARY"
    
    case $run_mode in
        "local")
            print_colored $GREEN "🚀 Запуск с локальным сервером (localhost:8081)"
            run_command="$GUI_BINARY"
            ;;
        "custom")
            if [ -z "$server_url" ]; then
                print_colored $RED "❌ Не указан URL для custom режима"
                echo "Используйте: $0 -c http://server:port"
                exit 1
            fi
            print_colored $GREEN "🚀 Запуск с сервером: $server_url"
            run_command="$GUI_BINARY $server_url"
            ;;
    esac
    
    echo ""
    print_colored $BLUE "Команда запуска: $run_command"
    echo ""
    print_colored $YELLOW "💡 Для остановки серверов используйте: $0 --kill"
    echo ""
    
    # Запуск
    exec $run_command
}

# Проверка наличия директории сборки
if [ ! -d "$BUILD_DIR" ]; then
    print_colored $RED "❌ Папка сборки не найдена: $BUILD_DIR"
    echo "Запустите сначала: ./build.sh"
    exit 1
fi

# Запуск основной функции
main "$@"

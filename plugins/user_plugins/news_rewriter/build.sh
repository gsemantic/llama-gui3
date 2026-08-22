#!/bin/bash
#
# build.sh — сборка плагина news_rewriter автономно (без полной сборки GUI).
#
# Плагин собирается как отдельный CMake-проект: его CMakeLists.txt сам находит
# include/ и external/imgui репозитория (PROJECT_ROOT), поэтому итерация по
# плагину быстрая — собираются только его цели, а не весь llama-gui.
#
# Использование:
#   ./build.sh                конфигурация (тесты включены) + сборка .so и тестов
#   ./build.sh --tests        то же + прогон юнит-тестов
#   ./build.sh --deploy DIR   сборка + копирование .so и news_rewriter.json в DIR
#   ./build.sh --clean        удалить каталог сборки (build/)
#   ./build.sh --help         эта справка
#
# Каталог сборки — build/ рядом со скриптом (игнорируется корневым .gitignore).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

RUN_TESTS=0
DEPLOY_DIR=""

usage() {
    awk 'NR>1 && !started && /^#/ { started=1 }
         started && /^#/ { sub(/^# ?/, ""); print; next }
         started { exit }' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --tests)  RUN_TESTS=1; shift ;;
        --clean)  rm -rf "$BUILD_DIR"; echo "Каталог сборки удалён: $BUILD_DIR"; exit 0 ;;
        --deploy)
            if [ $# -lt 2 ]; then
                echo "Ошибка: --deploy требует каталог (например: ./build.sh --deploy ../../../../build/plugins)"
                exit 1
            fi
            DEPLOY_DIR="$2"; shift 2
            ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Неизвестный аргумент: $1 (см. ./build.sh --help)"; exit 1 ;;
    esac
done

if [ "$RUN_TESTS" -eq 1 ]; then
    echo "Режим: сборка + тесты"
else
    echo "Режим: сборка (тесты не запускаем)"
fi
[ -n "$DEPLOY_DIR" ] && echo "Развёртывание в: $DEPLOY_DIR"

if ! command -v cmake &> /dev/null; then
    echo "Ошибка: CMake не найден (нужен 3.14+)."
    exit 1
fi

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# Конфигурация: тесты включены по умолчанию, чтобы бинарь всегда был свежим.
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DBUILD_NEWS_REWRITER_TESTS=ON -G "Unix Makefiles"

# Сборка только целей плагина (инкрементально, без пересборки GUI).
cmake --build "$BUILD_DIR" --target news_rewriter news_rewriter_tests -j"$JOBS"

echo "Готово:"
echo "  .so:       $BUILD_DIR/plugins/libnews_rewriter.so"
echo "  манифест:  $BUILD_DIR/plugins/news_rewriter.json"

if [ "$RUN_TESTS" -eq 1 ]; then
    echo ""
    echo "Запуск юнит-тестов..."
    "$BUILD_DIR/news_rewriter_tests"
fi

if [ -n "$DEPLOY_DIR" ]; then
    mkdir -p "$DEPLOY_DIR"
    cp "$BUILD_DIR/plugins/libnews_rewriter.so" "$DEPLOY_DIR/"
    cp "$BUILD_DIR/plugins/news_rewriter.json" "$DEPLOY_DIR/"
    echo "Скопировано в: $DEPLOY_DIR"
fi

# Авто-деплой в папку плагинов GUI (рядом с корневым build/), если она есть.
# Путь: plugins/user_plugins/news_rewriter -> ../../../build/plugins
GUI_PLUGINS="$SCRIPT_DIR/../../../build/plugins"
if [ -d "$GUI_PLUGINS" ]; then
    cp "$BUILD_DIR/plugins/libnews_rewriter.so" "$GUI_PLUGINS/"
    cp "$BUILD_DIR/plugins/news_rewriter.json" "$GUI_PLUGINS/"
    echo "Авто-деплой в GUI-плагины: $GUI_PLUGINS"
fi

echo "Сборка завершена."

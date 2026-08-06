#!/bin/bash

# Llama GUI - Скрипт быстрой сборки
# Автономная сборка GUI интерфейса

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

echo "======================================================"
echo "     Llama GUI - Автономная сборка"
echo "======================================================"
echo ""

# Проверка CMake
if ! command -v cmake &> /dev/null; then
    echo "❌ CMake не найден. Установите CMake 3.14+"
    exit 1
fi

echo "🔧 Создание папки сборки..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Конфигурация сборки
echo "⚙️  Конфигурация сборки..."

# Определяем доступные библиотеки
USE_SDL2=""
USE_OPENGL=""
USE_TESTS=""

if pkg-config --exists sdl2; then
    USE_SDL2="-DUSE_SDL2=ON"
    echo "✓ SDL2 найден"
else
    USE_SDL2="-DUSE_SDL2=OFF"
    echo "⚠ SDL2 не найден - GUI будет работать в консольном режиме"
fi

if pkg-config --exists gl; then
    USE_OPENGL="-DUSE_OPENGL=ON"
    echo "✓ OpenGL найден"
else
    USE_OPENGL="-DUSE_OPENGL=OFF"
    echo "⚠ OpenGL не найден - рендеринг будет отключен"
fi

# Сборка с тестами если нужна
if [[ "$1" == "--tests" ]]; then
    USE_TESTS="-DBUILD_TESTS=ON"
    echo "✓ Тесты будут включены"
fi

# Debug режим
BUILD_TYPE="Release"
if [[ "$1" == "--debug" ]]; then
    BUILD_TYPE="Debug"
    echo "✓ Debug сборка"
fi

echo "🔨 Запуск CMake..."
cmake "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    $USE_SDL2 \
    $USE_OPENGL \
    $USE_TESTS \
    -G "Unix Makefiles"

echo ""
echo "🛠️  Сборка проекта..."
make -j$(nproc)

echo ""
echo "✅ Сборка завершена успешно!"
echo ""
echo "Использование:"
echo "  ./llama-gui                    # Запуск с localhost:8081"
echo "  ./llama-gui http://server:8081 # Запуск с пользовательским URL"
echo ""
echo "Для очистки: rm -rf build"
echo "======================================================"
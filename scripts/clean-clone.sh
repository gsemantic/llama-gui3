#!/bin/bash
# clean-clone.sh - Скрипт для чистого клонирования проекта
# Клонирует репозиторий и сразу очищает от мусора

set -e

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}   Чистое клонирование проекта         ${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Проверка аргументов
if [ $# -lt 1 ]; then
    echo -e "${RED}Ошибка: Необходимо указать URL репозитория${NC}"
    echo ""
    echo "Использование:"
    echo "  $0 <repository-url> [directory-name]"
    echo ""
    echo "Примеры:"
    echo "  $0 git@github.com:user/repo.git"
    echo "  $0 https://github.com/user/repo.git my-project"
    exit 1
fi

REPO_URL=$1
DIR_NAME=${2:-$(basename "$REPO_URL" .git)}

echo -e "${YELLOW}[1/4] Клонирование репозитория...${NC}"
echo "   URL: $REPO_URL"
echo "   Директория: $DIR_NAME"
echo ""

git clone "$REPO_URL" "$DIR_NAME"

cd "$DIR_NAME"

echo -e "\n${YELLOW}[2/4] Настройка git ignore для будущего мусора...${NC}"

# Проверяем, есть ли .gitignore
if [ ! -f ".gitignore" ]; then
    echo "   Создаем .gitignore..."
    cat > .gitignore << 'EOF'
# Build artifacts
build/
cmake-build-*/
_build/
CMakeCache.txt
CMakeFiles/
cmake_install.cmake
Makefile
compile_commands.json

# Compiled objects
*.o
*.a
*.so
*.so.*
*.dll
*.dylib
*.lib

# Executables
llama-gui-core
test_*

# IDE
.vscode/
.idea/
*.swp
*.swo
*~

# OS
.DS_Store
Thumbs.db

# Logs
*.log
logs/

# Temporary files
*.tmp
*.bak
*.cache

# Environment variables
.env

# Dependencies (если не коммитятся)
deps/build/
external/build/

# Python
__pycache__/
*.py[cod]
*$py.class
*.egg-info/
venv/
env/

# Node.js (если используется)
node_modules/
npm-debug.log
yarn-error.log
EOF
    echo "   ✅ .gitignore создан"
else
    echo "   ✅ .gitignore уже существует"
fi

echo -e "\n${YELLOW}[3/4] Очистка существующего мусора...${NC}"

# Удаляем build-директории
BUILD_DIRS=("build" "cmake-build-*" "_build")
for dir in "${BUILD_DIRS[@]}"; do
    if [ -d "$dir" ]; then
        SIZE=$(du -sh "$dir" 2>/dev/null | cut -f1)
        echo "   Удаляем $dir ($SIZE)"
        rm -rf "$dir"
    fi
done

# Удаляем файлы CMake
CMAKE_FILES=("CMakeCache.txt" "CMakeFiles" "cmake_install.cmake" "Makefile" "compile_commands.json")
for file in "${CMAKE_FILES[@]}"; do
    if [ -e "$file" ]; then
        rm -rf "$file"
        echo "   Удаляем $file"
    fi
done

# Удаляем объектные файлы
find . -type f \( -name "*.o" -o -name "*.a" -o -name "*.so" -o -name "*.so.*" \) -delete 2>/dev/null || true

# Удаляем временные файлы
find . -type f \( -name "*.log" -o -name "*.tmp" -o -name "*.bak" -o -name "*~" \) -delete 2>/dev/null || true

# Удаляем IDE мусор
find . -type d \( -name ".vscode" -o -name ".idea" \) -exec rm -rf {} + 2>/dev/null || true
find . -type f -name ".DS_Store" -delete 2>/dev/null || true

echo "   ✅ Очистка завершена"

echo -e "\n${YELLOW}[4/4] Инициализация подмодулей (если есть)...${NC}"
if [ -f ".gitmodules" ]; then
    git submodule update --init --recursive
    echo "   ✅ Подмодули инициализированы"
else
    echo "   ℹ️  Подмодули не обнаружены"
fi

# Показываем итоговый статус
echo -e "\n${GREEN}========================================${NC}"
echo -e "${GREEN}   ✅ Чистое клонирование завершено!   ${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "Проект готов к работе!"
echo ""
echo "Следующие шаги:"
echo -e "  ${BLUE}1.${NC} Перейдите в директорию: ${YELLOW}cd $DIR_NAME${NC}"
echo -e "  ${BLUE}2.${NC} Установите зависимости (если нужно)"
echo -e "  ${BLUE}3.${NC} Настройте проект: ${YELLOW}./build.sh${NC} или ${YELLOW}cmake -B build && cmake --build build${NC}"
echo ""
echo "Полезные команды:"
echo -e "  • Проверка статуса: ${YELLOW}git status${NC}"
echo -e "  • Чистый пуш: ${YELLOW}./scripts/clean-push.sh${NC}"
echo -e "  • Сборка проекта: ${YELLOW}./build.sh${NC}"
echo ""

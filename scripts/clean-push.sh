#!/bin/bash
# clean-push.sh - Скрипт для чистого пуша в репозиторий
# Очищает проект от мусора перед коммитом и пушем

set -e

echo "🧹 Начинаем очистку проекта перед пушем..."

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Функция для подсчета размера директории
get_dir_size() {
    if [ -d "$1" ]; then
        du -sh "$1" 2>/dev/null | cut -f1
    else
        echo "0"
    fi
}

# 1. Очистка build-артефактов
echo -e "\n${YELLOW}[1/6] Очистка build-артефактов...${NC}"
BUILD_DIRS=("build" "cmake-build-*" "_build")
for dir in "${BUILD_DIRS[@]}"; do
    if [ -d "$dir" ]; then
        SIZE=$(get_dir_size "$dir")
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

# 2. Очистка скомпилированных объектов
echo -e "\n${YELLOW}[2/6] Очистка скомпилированных объектов...${NC}"
find . -type f \( -name "*.o" -o -name "*.a" -o -name "*.so" -o -name "*.so.*" -o -name "*.dll" -o -name "*.lib" -o -name "*.dylib" \) -delete 2>/dev/null || true
echo "   Объектные файлы удалены"

# 3. Очистка исполняемых файлов
echo -e "\n${YELLOW}[3/6] Очистка исполняемых файлов...${NC}"
EXECUTABLES=("llama-gui-core" "test_*")
for exec in "${EXECUTABLES[@]}"; do
    find . -maxdepth 2 -type f -name "$exec" -delete 2>/dev/null || true
done
echo "   Исполняемые файлы удалены"

# 4. Очистка временных файлов и логов
echo -e "\n${YELLOW}[4/6] Очистка временных файлов и логов...${NC}"
find . -type f \( -name "*.log" -o -name "*.tmp" -o -name "*.bak" -o -name "*~" -o -name "*.swp" -o -name "*.swo" \) -delete 2>/dev/null || true
find . -type d -name "logs" -empty -delete 2>/dev/null || true
echo "   Временные файлы удалены"

# 5. Очистка IDE и OS мусора
echo -e "\n${YELLOW}[5/6] Очистка IDE и OS мусора...${NC}"
find . -type d \( -name ".vscode" -o -name ".idea" -o -name ".DS_Store" \) -exec rm -rf {} + 2>/dev/null || true
find . -type f -name ".DS_Store" -delete 2>/dev/null || true
find . -type f -name "Thumbs.db" -delete 2>/dev/null || true
echo "   IDE и OS файлы удалены"

# 6. Проверка git статуса
echo -e "\n${YELLOW}[6/6] Проверка состояния git...${NC}"
git status --short

# Предложение сделать коммит
echo -e "\n${GREEN}✅ Очистка завершена!${NC}"
echo ""
echo "Следующие шаги:"
echo "  1. Проверьте изменения: git status"
echo "  2. Добавьте файлы: git add ."
echo "  3. Сделайте коммит: git commit -m 'your message'"
echo "  4. Сделайте пуш: git push"
echo ""
echo "Или выполните одну команду:"
echo -e "   ${YELLOW}git add . && git commit -m 'clean: remove build artifacts' && git push${NC}"

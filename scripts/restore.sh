#!/bin/bash
# ============================================================================
# restore.sh - Восстановление проекта llama-gui из архива бэкапа
# ============================================================================
# Использование:
#   ./restore.sh <архив.tar.gz>            # Восстановить в текущий каталог
#   ./restore.sh <архив.tar.gz> /tmp/test  # Восстановить в указанный каталог
#   ./restore.sh --help                    # Показать справку
# ============================================================================
# Восстанавливает код из архива, затем подставляет секреты из _secrets/.
# Секреты НЕ хранятся в архиве — они берутся из _secrets/ рядом с проектом.
# ============================================================================

set -e

# Настройки
PROJECT_NAME="llama-gui"

# Цвета
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[OK]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1"; }

show_help() {
    echo "Использование: $0 <архив.tar.gz> [целевой_каталог]"
    echo ""
    echo "Аргументы:"
    echo "  архив.tar.gz        Путь к архиву бэкапа"
    echo "  целевой_каталог     Куда распаковать (по умолчанию: текущий каталог)"
    echo ""
    echo "Опции:"
    echo "  --help              Показать эту справку"
    echo "  --no-secrets        Пропустить подстановку секретов"
    echo "  --no-build          Не запускать cmake/make после распаковки"
    echo ""
    echo "Примеры:"
    echo "  $0 _backups/llama-gui-0.5.3-2026_09_13_14_30-alex.tar.gz"
    echo "  $0 _backups/latest.tar.gz /tmp/restore_test"
    echo "  $0 archive.tar.gz --no-build"
    exit 0
}

# Парсим аргументы
ARCHIVE=""
TARGET_DIR=""
NO_SECRETS=false
NO_BUILD=false

while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h)
            show_help
            ;;
        --no-secrets)
            NO_SECRETS=true
            shift
            ;;
        --no-build)
            NO_BUILD=true
            shift
            ;;
        -*)
            log_error "Неизвестная опция: $1"
            exit 1
            ;;
        *)
            if [ -z "$ARCHIVE" ]; then
                ARCHIVE="$1"
            elif [ -z "$TARGET_DIR" ]; then
                TARGET_DIR="$1"
            else
                log_error "Лишний аргумент: $1"
                exit 1
            fi
            shift
            ;;
    esac
done

if [ -z "$ARCHIVE" ]; then
    log_error "Укажите архив бэкапа"
    echo "Использование: $0 <архив.tar.gz> [целевой_каталог]"
    exit 1
fi

if [ ! -f "$ARCHIVE" ]; then
    log_error "Архив не найден: $ARCHIVE"
    exit 1
fi

# Абсолютные пути
ARCHIVE="$(cd "$(dirname "$ARCHIVE")" && pwd)/$(basename "$ARCHIVE")"
TARGET_DIR="${TARGET_DIR:-.}"
TARGET_DIR="$(cd "$(dirname "$TARGET_DIR")" 2>/dev/null && pwd)/$(basename "$TARGET_DIR")" || \
    TARGET_DIR="$(pwd)/$(basename "$TARGET_DIR")"

echo ""
log_info "Восстановление из архива"
echo "  Архив:   $ARCHIVE"
echo "  Цель:    $TARGET_DIR"
echo ""

# === 1. Проверяем целевой каталог ===
if [ -d "$TARGET_DIR/$PROJECT_NAME" ]; then
    log_warning "Каталог $TARGET_DIR/$PROJECT_NAME уже существует!"
    echo -n "  Перезаписать? [y/N] "
    read -r answer
    if [ "$answer" != "y" ] && [ "$answer" != "Y" ]; then
        log_info "Отменено"
        exit 0
    fi
fi

# === 2. Распаковка архива ===
log_info "Распаковка архива..."
mkdir -p "$TARGET_DIR"
tar -xzf "$ARCHIVE" -C "$TARGET_DIR"

if [ ! -d "$TARGET_DIR/$PROJECT_NAME" ]; then
    log_error "Архив не содержит каталог $PROJECT_NAME"
    exit 1
fi

local archive_size=$(du -h "$ARCHIVE" | cut -f1)
log_success "Архив распакован в $TARGET_DIR/$PROJECT_NAME/"

# === 3. Подстановка секретов из _secrets/ ===
PROJECT_RESTORED="$TARGET_DIR/$PROJECT_NAME"
SECRETS_SRC="$PROJECT_RESTORED/_secrets"

if [ "$NO_SECRETS" = true ]; then
    log_info "Подстановка секретов пропущена (--no-secrets)"
elif [ -d "$SECRETS_SRC" ]; then
    log_info "Подстановка секретов из $SECRETS_SRC/..."
    local secret_count=0
    while IFS= read -r -d '' secret_file; do
        local rel_path="${secret_file#$SECRETS_SRC/}"
        local target="$PROJECT_RESTORED/$rel_path"
        mkdir -p "$(dirname "$target")"
        cp "$secret_file" "$target"
        secret_count=$((secret_count + 1))
    done < <(find "$SECRETS_SRC" -type f -print0)

    if [ "$secret_count" -gt 0 ]; then
        log_success "Подставлено секретов: $secret_count"
    else
        log_warning "Секреты в _secrets/ не найдены"
    fi
else
    log_warning "_secrets/ не найден в распакованном проекте"
    log_info "Секреты нужно разместить вручную в .env файлы"
    echo ""
    echo "  Необходимые файлы:"
    echo "    profiles/.env          — API-ключи провайдеров"
    echo "    plugins/*/deploy.env   — креды деплоя плагинов"
    echo ""
fi

# === 4. Информация о бэкапе ===
if [ -f "$PROJECT_RESTORED/BACKUP_INFO.txt" ]; then
    log_info "Информация о бэкапе:"
    echo ""
    head -10 "$PROJECT_RESTORED/BACKUP_INFO.txt"
    echo ""
fi

# === 5. Сборка (опционально) ===
if [ "$NO_BUILD" = true ]; then
    log_info "Сборка пропущена (--no-build)"
else
    log_info "Запуск сборки..."
    echo ""

    cd "$PROJECT_RESTORED"
    mkdir -p build

    if [ -f "build.sh" ]; then
        bash build.sh
    else
        cd build
        cmake ..
        make -j"$(nproc)"
    fi

    if [ $? -eq 0 ]; then
        log_success "Сборка завершена успешно"
    else
        log_error "Сборка завершилась с ошибками"
        exit 1
    fi
fi

# === Итог ===
echo ""
log_success "Восстановление завершено!"
echo ""
echo "  Каталог:    $PROJECT_RESTORED"
if [ -f "$PROJECT_RESTORED/run_gui.sh" ]; then
    echo "  Запуск:     cd $PROJECT_RESTORED && bash run_gui.sh"
fi
echo ""

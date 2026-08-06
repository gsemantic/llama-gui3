#!/bin/bash
# ============================================================================
# backup.sh - Автоматический бэкап проекта llama-gui
# ============================================================================
# Использование:
#   ./backup.sh                    # Бэкап с комментарием по умолчанию
#   ./backup.sh "Добавил сетку"    # Бэкап с комментарием
#   ./backup.sh --help             # Показать справку
# ============================================================================
# ВАЖНО: Git НЕ требуется! Работает полностью автономно.
# ============================================================================

set -e

# Настройки
PROJECT_NAME="llama-gui"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BACKUP_DIR="$PROJECT_ROOT/_backups"
CLOUD_DIR="$HOME/Yandex.Disk/Backups/$PROJECT_NAME"  # Путь к Яндекс.Диску

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Функция для красивого вывода
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[OK]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Показать справку
show_help() {
    echo "Использование: $0 [КОММЕНТАРИЙ]"
    echo ""
    echo "Аргументы:"
    echo "  КОММЕНТАРИЙ    Описание изменений (необязательно)"
    echo ""
    echo "Примеры:"
    echo "  $0"
    echo "  $0 \"Добавил систему версионирования\""
    echo "  $0 \"Grid snapping + versioning\""
    echo ""
    echo "Опции:"
    echo "  --help                  Показать эту справку"
    echo "  --local-only            Сохранить только локально (без облака)"
    echo "  --list                  Показать список локальных бэкапов"
    echo "  --clean                 Удалить старые бэкапы (оставить последние 10)"
    echo "  --skip-version-incr     Не увеличивать версию (для откатов)"
    exit 0
}

# Показать список бэкапов
list_backups() {
    log_info "Локальные бэкапы:"
    echo ""
    ls -lh "$BACKUP_DIR" 2>/dev/null | grep ".tar.gz" || echo "  (нет бэкапов)"
    echo ""
    
    if [ -d "$CLOUD_DIR" ]; then
        log_info "Облачные бэкапы (Яндекс.Диск):"
        echo ""
        ls -lh "$CLOUD_DIR" 2>/dev/null | grep ".tar.gz" || echo "  (нет бэкапов)"
    fi
    exit 0
}

# Очистить старые бэкапы
clean_old_backups() {
    log_info "Очистка старых бэкапов..."
    
    # Оставить последние 10 локальных
    if [ -d "$BACKUP_DIR" ]; then
        cd "$BACKUP_DIR"
        ls -t *.tar.gz 2>/dev/null | tail -n +11 | xargs -r rm
        log_success "Локальные бэкапы очищены"
    fi
    
    # Оставить последние 10 облачных
    if [ -d "$CLOUD_DIR" ]; then
        cd "$CLOUD_DIR"
        ls -t *.tar.gz 2>/dev/null | tail -n +11 | xargs -r rm
        log_success "Облачные бэкапы очищены"
    fi
}

# Проверка зависимостей
check_dependencies() {
    local missing=()
    
    if ! command -v tar &> /dev/null; then
        missing+=("tar")
    fi
    
    if ! command -v rsync &> /dev/null; then
        missing+=("rsync")
    fi
    
    if [ ${#missing[@]} -ne 0 ]; then
        log_error "Отсутствуют зависимости: ${missing[*]}"
        log_info "Установите: sudo apt-get install ${missing[*]}"
        exit 1
    fi
    
    log_success "Все зависимости найдены"
}

# Получить текущую версию из CMakeLists.txt
get_version() {
    local version_file="$PROJECT_ROOT/CMakeLists.txt"
    if [ -f "$version_file" ]; then
        local major=$(grep "set(VERSION_MAJOR" "$version_file" | grep -oP '\d+' | head -1)
        local minor=$(grep "set(VERSION_MINOR" "$version_file" | grep -oP '\d+' | head -1)
        local patch=$(grep "set(VERSION_PATCH" "$version_file" | grep -oP '\d+' | head -1)
        local suffix=$(grep "set(VERSION_SUFFIX" "$version_file" | grep -oP '"\K[^"]+' | head -1)

        if [ -n "$major" ] && [ -n "$minor" ]; then
            echo "${major}.${minor}.${patch:-0}-${suffix:-dev}"
            return
        fi
    fi

    # Если не удалось получить версию, используем дату
    echo "dev-$(date +%Y%m%d)"
}

# Инкрементировать PATCH версию в CMakeLists.txt
increment_version() {
    local version_file="$PROJECT_ROOT/CMakeLists.txt"
    if [ ! -f "$version_file" ]; then
        log_warning "CMakeLists.txt не найден, пропускаю инкремент версии"
        return
    fi

    local current_patch=$(grep "set(VERSION_PATCH" "$version_file" | grep -oP '\d+' | head -1)
    local new_patch=$((${current_patch:-0} + 1))

    # Обновляем VERSION_PATCH в CMakeLists.txt
    sed -i "s/set(VERSION_PATCH [0-9]*)/set(VERSION_PATCH $new_patch)/" "$version_file"

    log_success "Версия обновлена: PATCH ${current_patch:-0} → $new_patch"
}

# Проверить, не является ли текущая версией из старого бэкапа
check_version_mismatch() {
    local version_file="$PROJECT_ROOT/CMakeLists.txt"
    local backup_info_file="$PROJECT_ROOT/BACKUP_INFO.txt"
    
    # Проверяем, есть ли файл BACKUP_INFO.txt (признак распаковки бэкапа)
    if [ ! -f "$backup_info_file" ]; then
        return 0
    fi
    
    # Получаем версию из BACKUP_INFO.txt
    local backup_version=$(grep "^Version:" "$backup_info_file" | awk '{print $2}')
    
    if [ -z "$backup_version" ]; then
        return 0
    fi
    
    # Получаем текущую версию из CMakeLists.txt
    local current_version=$(get_version)
    
    # Сравниваем версии по компонентам (MAJOR.MINOR.PATCH)
    local backup_major=$(echo "$backup_version" | grep -oP '^\d+' | head -1)
    local backup_minor=$(echo "$backup_version" | grep -oP '^\d+\.\K\d+' | head -1)
    local backup_patch=$(echo "$backup_version" | grep -oP '^\d+\.\d+\.\K\d+' | head -1)
    
    local current_major=$(grep "set(VERSION_MAJOR" "$version_file" | grep -oP '\d+' | head -1)
    local current_minor=$(grep "set(VERSION_MINOR" "$version_file" | grep -oP '\d+' | head -1)
    local current_patch=$(grep "set(VERSION_PATCH" "$version_file" | grep -oP '\d+' | head -1)
    
    # Преобразуем в числа для сравнения
    backup_major=${backup_major:-0}
    backup_minor=${backup_minor:-0}
    backup_patch=${backup_patch:-0}
    current_major=${current_major:-0}
    current_minor=${current_minor:-0}
    current_patch=${current_patch:-0}
    
    # Сравниваем: если версия бэкапа меньше текущей — это откат
    local is_rollback=false
    
    if [ "$backup_major" -lt "$current_major" ]; then
        is_rollback=true
    elif [ "$backup_major" -eq "$current_major" ]; then
        if [ "$backup_minor" -lt "$current_minor" ]; then
            is_rollback=true
        elif [ "$backup_minor" -eq "$current_minor" ]; then
            if [ "$backup_patch" -lt "$current_patch" ]; then
                is_rollback=true
            fi
        fi
    fi
    
    if [ "$is_rollback" = true ]; then
        log_warning "=============================================="
        log_warning "ОБНАРУЖЕН ОТКАТ К СТАРОЙ ВЕРСИИ!"
        log_warning "=============================================="
        log_warning "Версия бэкапа:   $backup_version"
        log_warning "Текущая версия:  $current_version"
        log_warning "=============================================="
        log_warning ""
        log_warning "ВНИМАНИЕ: При следующем бэкапе версия будет"
        log_warning "увеличена от СТАРОЙ версии (${backup_version})."
        log_warning ""
        log_warning "Если это нежелательно, используйте:"
        log_warning "  --skip-version-incr"
        log_warning "=============================================="
        echo ""
    fi
}

# Получить идентификатор сборки (без Git)
get_build_id() {
    # Используем первые 8 символов хоста и времени для уникальности
    echo "$(hostname | cut -c1-4)$(date +%H%M)"
}

# Основная функция бэкапа
create_backup() {
    local comment="$1"
    local local_only="$2"
    local skip_version_incr="$3"

    log_info "Создание бэкапа проекта..."
    echo ""

    # === ПРОВЕРКА НА ОТКАТ ===
    check_version_mismatch

    # === ИНКРЕМЕНТ ВЕРСИИ (если не пропущено) ===
    if [ "$skip_version_incr" != "true" ]; then
        increment_version
    else
        log_info "Инкремент версии пропущен (режим отката)"
    fi

    # Получаем информацию
    local version=$(get_version)
    local build_id=$(get_build_id)
    local timestamp=$(date +%Y_%m_%d_%H_%M)
    local hostname_short=$(hostname | cut -c1-8)
    
    # Формируем имя файла
    local backup_filename="${PROJECT_NAME}-${version}-${timestamp}-${hostname_short}"
    if [ -n "$comment" ]; then
        # Очищаем комментарий от спецсимволов
        local clean_comment=$(echo "$comment" | tr ' ' '_' | tr -cd '[:alnum:]_')
        backup_filename="${backup_filename}-${clean_comment}"
    fi
    backup_filename="${backup_filename}.tar.gz"
    
    # Создаём директорию для бэкапов
    mkdir -p "$BACKUP_DIR"
    
    # Получаем размер проекта
    local project_size=$(du -sh "$PROJECT_ROOT" --exclude="_backups" --exclude="build" 2>/dev/null | cut -f1)
    log_info "Размер проекта: $project_size"
    
    # Создаём временную директорию для бэкапа
    local temp_dir=$(mktemp -d)
    local backup_temp="$temp_dir/$PROJECT_NAME"
    
    log_info "Копирование файлов..."

    # Копируем файлы проекта (исключая ненужное)
    rsync -a \
        --exclude=".git" \
        --exclude="_backups" \
        --exclude="build" \
        --exclude="llama-gui-core" \
        --exclude="*.log" \
        --exclude="*.swp" \
        --exclude=".DS_Store" \
        --exclude="*.pyc" \
        --exclude="__pycache__" \
        --exclude="*.a" \
        --exclude="*.so" \
        --exclude="*.o" \
        "$PROJECT_ROOT/" "$backup_temp/"
    
    # Добавляем файл с информацией о бэкапе
    cat > "$backup_temp/BACKUP_INFO.txt" << EOF
============================================================================
BACKUP INFORMATION
============================================================================
Project:     $PROJECT_NAME
Version:     $version
Timestamp:   $(date '+%Y-%m-%d %H:%M:%S')
Hostname:    $hostname_short
Comment:     ${comment:-No comment}
Build ID:    $build_id
============================================================================

SYSTEM INFORMATION:
EOF
    
    # Добавляем информацию о системе
    echo "OS: $(uname -a)" >> "$backup_temp/BACKUP_INFO.txt"
    echo "Date: $(date)" >> "$backup_temp/BACKUP_INFO.txt"
    
    # Создаём архив
    log_info "Создание архива..."
    tar -czf "$BACKUP_DIR/$backup_filename" -C "$temp_dir" "$PROJECT_NAME"
    
    # Получаем размер архива
    local archive_size=$(du -h "$BACKUP_DIR/$backup_filename" | cut -f1)
    log_success "Локальный бэкап создан: $backup_filename ($archive_size)"
    
    # === ВАЖНО: Удаляем предыдущие локальные бэкапы ===
    log_info "Очистка предыдущих локальных бэкапов..."
    local old_backups=$(ls -1 "$BACKUP_DIR"/*.tar.gz 2>/dev/null | wc -l)
    if [ "$old_backups" -gt 1 ]; then
        # Удаляем все кроме последнего
        ls -t "$BACKUP_DIR"/*.tar.gz | tail -n +2 | xargs rm
        local deleted=$((old_backups - 1))
        log_success "Удалено старых бэкапов: $deleted"
    fi
    
    # Копируем в облако (если не --local-only)
    if [ "$local_only" != "true" ]; then
        if [ -d "$CLOUD_DIR" ] || mkdir -p "$CLOUD_DIR" 2>/dev/null; then
            log_info "Копирование в облако (Яндекс.Диск)..."
            cp "$BACKUP_DIR/$backup_filename" "$CLOUD_DIR/"
            log_success "Облачный бэкап создан: $CLOUD_DIR/$backup_filename"

            # Запускаем синхронизацию Яндекс.Диска
            if command -v yandex-disk &> /dev/null; then
                log_info "Синхронизация с Яндекс.Диском..."
                yandex-disk sync
                log_success "Синхронизация завершена"
            else
                log_warning "yandex-disk не найден. Пропускаем синхронизацию."
            fi

            # В облаке храним всю историю, но чистим очень старые (более 100)
            local cloud_backups=$(ls -1 "$CLOUD_DIR"/*.tar.gz 2>/dev/null | wc -l)
            if [ "$cloud_backups" -gt 100 ]; then
                log_info "Очистка очень старых облачных бэкапов..."
                cd "$CLOUD_DIR"
                ls -t *.tar.gz | tail -n +101 | xargs rm
                log_success "Удалено старых облачных бэкапов: $((cloud_backups - 100))"
            fi
        else
            log_warning "Яндекс.Диск не найден. Пропускаем копирование в облако."
            log_info "Проверьте путь: $CLOUD_DIR"
            log_info "Локальный бэкап сохранён."
        fi
    fi
    
    # Очищаем временную директорию
    rm -rf "$temp_dir"
    
    echo ""
    log_success "Бэкап завершён!"
    echo ""
    echo "  Версия:     $version"
    echo "  Build ID:   $build_id"
    echo "  Файл:       $backup_filename"
    echo "  Размер:     $archive_size"
    echo "  Локально:   $BACKUP_DIR/$backup_filename"
    if [ "$local_only" != "true" ] && [ -d "$CLOUD_DIR" ]; then
        echo "  Облако:     $CLOUD_DIR/$backup_filename"
    fi
    echo ""
}

# ============================================================================
# Основная программа
# ============================================================================

# Парсим аргументы
LOCAL_ONLY=false
SKIP_VERSION_INCR=false

for arg in "$@"; do
    case $arg in
        --help|-h)
            show_help
            ;;
        --list|-l)
            list_backups
            ;;
        --clean|-c)
            clean_old_backups
            ;;
        --local-only)
            LOCAL_ONLY=true
            ;;
        --skip-version-incr)
            SKIP_VERSION_INCR=true
            ;;
        *)
            COMMENT="$arg"
            ;;
    esac
done

# Проверяем зависимости
check_dependencies

# Создаём бэкап
create_backup "${COMMENT:-}" "$LOCAL_ONLY" "$SKIP_VERSION_INCR"

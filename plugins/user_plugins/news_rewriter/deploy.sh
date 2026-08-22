#!/bin/bash
#
# deploy.sh — деплой MU-плагина nr-seo.php на WordPress-сервер (Timeweb).
#
# Заливает plugins/user_plugins/news_rewriter/mu-plugins/nr-seo.php в
# <WP_REMOTE_DIR>/nr-seo.php на сервере по FTP/SFTP.
# Параметры сервера и креды берутся из deploy.env (НЕ коммитится).
#
# Использование:
#   ./deploy.sh            залить nr-seo.php на сервер
#   ./deploy.sh --help     эта справка
#
# Зависимости (выберется то, что есть):
#   - ftp/ftps: curl  (обычно уже установлен; создаёт папки через --ftp-create-dirs)
#   - sftp:     sftp (OpenSSH); для входа по паролю нужен sshpass либо ssh-agent/
#              ключ (рекомендуется WP_SSH_KEY).
#
# Пример deploy.env (скопируйте из deploy.env.example и заполните):
#   WP_HOST=vh442.timeweb.ru
#   WP_PORT=21                 # 21=ftp, 990=ftps, или порт SFTP от хостера (часто 22)
#   WP_PROTO=ftp               # ftp | ftps | sftp
#   WP_USER=ci37993
#   WP_PASS=********           # пароль FTP/SFTP (или WP_SSH_KEY для sftp по ключу)
#   WP_REMOTE_DIR=/home/c/ci37993/public_html/wp-content/mu-plugins
#
# ВАЖНО: WP_REMOTE_DIR — это ПОЛНЫЙ путь к папке mu-plugins на сервере
# (её можно узнать в файл-менеджере панели или командой `pwd` в SSH-консоли).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$SCRIPT_DIR/deploy.env"
SRC="$SCRIPT_DIR/mu-plugins/nr-seo.php"

usage() {
    awk 'NR>1 && !started && /^#/ { started=1 }
         started && /^#/ { sub(/^# ?/, ""); print; next }
         started { exit }' "$0"
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage; exit 0
fi

if [ ! -f "$SRC" ]; then
    echo "Ошибка: нет файла $SRC (сначала создайте/обновите MU-плагин)."
    exit 1
fi

if [ ! -f "$ENV_FILE" ]; then
    echo "Ошибка: нет $ENV_FILE. Скопируйте deploy.env.example -> deploy.env и заполните."
    exit 1
fi

# Загрузка конфигурации.
set -a; . "$ENV_FILE"; set +a

: "${WP_HOST:?не задан WP_HOST в deploy.env}"
: "${WP_USER:?не задан WP_USER в deploy.env}"
: "${WP_REMOTE_DIR:?не задан WP_REMOTE_DIR в deploy.env}"
WP_PORT="${WP_PORT:-21}"
WP_PROTO="${WP_PROTO:-ftp}"

echo "Деплой $SRC -> $WP_PROTO://$WP_USER@$WP_HOST:$WP_PORT$WP_REMOTE_DIR/nr-seo.php"

case "$WP_PROTO" in
    ftp|ftps)
        : "${WP_PASS:?для proto=$WP_PROTO нужен WP_PASS}"
        # Двойной слэш после порта => абсолютный путь на сервере (curl CWD-нет
        # относительно корня FTP-сессии, а не домашней папки).
        URL="$WP_PROTO://$WP_HOST:$WP_PORT/$WP_REMOTE_DIR/nr-seo.php"
        curl --ftp-create-dirs -sS -T "$SRC" --user "$WP_USER:$WP_PASS" "$URL"
        ;;
    sftp)
        SFTP_OPTS=(-P "$WP_PORT" "$WP_USER@$WP_HOST")
        if [ -n "${WP_SSH_KEY:-}" ]; then
            SFTP_OPTS=(-i "$WP_SSH_KEY" "${SFTP_OPTS[@]}")
        elif [ -z "${WP_PASS:-}" ]; then
            echo "Для sftp нужен WP_SSH_KEY (ключ) либо WP_PASS + установленный sshpass."
            exit 1
        else
            if ! command -v sshpass >/dev/null; then
                echo "Ошибка: для sftp по паролю нужен sshpass (или задайте WP_SSH_KEY)."
                exit 1
            fi
            SFTP_OPTS=(sshpass -p "$WP_PASS" sftp "${SFTP_OPTS[@]}")
        fi
        printf 'cd %s\nput %s nr-seo.php\nbye\n' "$WP_REMOTE_DIR" "$SRC" \
            | "${SFTP_OPTS[@]}" -b -
        ;;
    *)
        echo "Неизвестный WP_PROTO=$WP_PROTO (ожидается ftp|ftps|sftp)."
        exit 1
        ;;
esac

echo "Готово: nr-seo.php залит на сервер."

# wp_setup — настройка окружения для WordPress

## Порядок действий

1. Сначала вызови `wp_check_deps` — покажет что установлено, а что нет.
2. Если есть ошибки — предложи пользователю установить зависимости.
3. Если всё ОК — предложи создать сайт через `wp_create_site`.

## Зависимости WordPress

| Компонент | Пакеты | Зачем |
|-----------|--------|-------|
| PHP CLI | php-cli php-mysql php-xml php-mbstring php-curl php-zip php-gd | Ядро WP + плагины |
| БД | mariadb-server (или mysql-server) | Хранение данных |
| Веб-сервер | apache2 (или nginx) | Обработка HTTP |
| WP-CLI | wp-cli.phar → /usr/local/bin/wp | Управление WP из консоли |
| Git | git | Контроль версий |
| curl | curl | Скачивание файлов |

## Установка зависимостей (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install php-cli php-mysql php-xml php-mbstring php-curl php-zip php-gd mariadb-server apache2 git curl
```

## Установка WP-CLI

```bash
curl -O https://raw.githubusercontent.com/wp-cli/builds/gh-pages/phar/wp-cli.phar
sudo mv wp-cli.phar /usr/local/bin/wp
sudo chmod +x /usr/local/bin/wp
```

## Создание сайта

Вызови `wp_create_site` с параметрами:
- `QUERY`: имя сайта (обязательно, напр. `my-site`)
- `PATTERN`: имя БД (опц., по умолчанию `wp_<имя_сайта>`)
- `CONTENT`: пользователь БД (опц., по умолчанию `wp_<имя_сайта>`)
- `CLI`: пароль БД (опц., по умолчанию `pass_<имя_сайта>`)
- `URL`: URL сайта (опц., по умолчанию `http://<имя_сайта>.localhost`)

Инструмент создаст:
- Директорию `/var/www/<имя_сайта>`
- Базу данных
- wp-config.php
- Установит WordPress
- Настроит Apache VirtualHost
- Добавит запись в /etc/hosts

## После создания сайта

Настрой wp_coder:
1. Открой «WordPress → Проект»
2. Укажи корень WP: `/var/www/<имя_сайта>`
3. Укажи URL: `http://<имя_сайта>.localhost`
4. Сохрани

Админка: `http://<имя_сайта>.localhost/wp-admin/`
Логин: `admin` / Пароль: `admin`

#pragma once

#include <string>

namespace news_rewriter {

// Имена ключей в .env плагина для учётных данных WordPress (Application Password).
constexpr const char* kNewsRewriterWpUser = "NEWS_REWRITER_WP_USERNAME";
constexpr const char* kNewsRewriterWpPass = "NEWS_REWRITER_WP_APP_PASSWORD";

// Минимальная работа с .env-файлом (KEY=VALUE, одна пара на строку).
// Используется, чтобы держать секреты (логин/пароль WP) вне settings.ini,
// по конвенции проекта (см. EnvManager приложения).
std::string dotenv_read(const std::string& path, const std::string& key);

// Создаёт/обновляет ключ, сохраняя прочие строки файла. Создаёт каталог при
// необходимости.
void dotenv_write(const std::string& path, const std::string& key,
                  const std::string& value);

// Удаляет ключ из файла (если есть).
void dotenv_remove(const std::string& path, const std::string& key);

} // namespace news_rewriter

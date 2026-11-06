#pragma once

/*
 * wp_sql.h — SQL для wp_create_site: значения уходят ПАРАМЕТРАМИ (D8).
 *
 * ЧТО БЫЛО И ЧЕМ ЭТО БЫЛО НА САМОМ ДЕЛЕ. Скрипт создания БД собирался
 * строкой, а пароль вставлялся так: у shell::shell_quote(db_pass) срезались
 * внешние кавычки (substr(1, size-2)) — то есть из ЭКРАНИРОВАННОГО
 * значения вырезалась сама экранировка, и остаток клался внутрь
 * двойных кавычек bash. Дефект был записан как «пароль с кавычкой ломает
 * выход», и это неверно: пароль вида `x"; echo …; echo "` закрывал
 * двойную кавычку и становился ОТДЕЛЬНОЙ КОМАНДОЙ оболочки — то есть
 * выполнялся произвольный код от пользователя плагина. Проверено на живой
 * bash и разбором команды (три стадии вместо одной). Снимать экранировку
 * нельзя было и «больше аккуратно»: экранирование shell и экранирование
 * SQL — разные языки, и правила пересечения тут нет.
 *
 * ЧТО ЗДЕСЬ. Значение попадает в SQL ровно одной формой — шестнадцатеричным
 * литералом 0x…: у него нет ни кавычек, ни экранировки, ни синтаксиса,
 * который можно было бы закрыть. Дальше сервер сам строит литерал (QUOTE),
 * текст оператора склеивается CONCAT, а выполняется он подготовленным
 * запросом (PREPARE/EXECUTE) — по одному оператору, потому что PREPARE
 * берёт ровно один. Экранирования, которое можно снять, здесь просто нет:
 * снимать нечего.
 *
 * ПРОВЕРЕНО НА ЖИВОМ СЕРВЕРЕ (не «должно работать»): одноразовый
 * MariaDB 11.8 (клиент 15.2), скрипт из provision_sql() с паролем
 * `a'\ "$K; id -u0,` и отдельно с кириллицей — оба раза создали
 * пользователя, вход под которым проходит ровно с этим паролем, а с
 * неверным — отказ. Команда выполнена в той же форме, что и run_command(),
 * но без `sudo`: пароля root на машине нет, и это названо здесь, чтобы
 * граница проверки не выглядела шире, чем она есть.
 *
 * ГРАНИЦЫ ЧЕСТНОСТИ, которые обязаны быть здесь, а не в журнале:
 *   - ИМЯ БД — идентификатор, а не значение: подставить его в backticks
 *     можно только текстом. Поэтому оно ПРОВЕРЯЕТСЯ целиком
 *     (is_safe_identifier), а не экранируется, и проверка живёт здесь, а
 *     не в вызывающем коде: функция обязана быть безопасна сама по себе.
 *   - Имя пользователя проверять не надо и нельзя: оно уходит параметром
 *     и литерал строит сервер. Проверка была бы второй копией правила для
 *     значения, которое до SQL не доходит текстом.
 *   - SQL уходит на stdin, а не аргументом: в строке команды остаётся
 *     только путь (run_command), поэтому политика команд видит ровно то,
 *     что действительно выполнится, и пароль в команде не появляется ни
 *     при каких символах.
 */

#include "../../core/security.h"
#include "../../core/shell.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace coder {
namespace wp {
namespace sql {

/* Шестнадцатеричный литерал 0x… — единственная форма, в которой значение
 * уходит в SQL. Пустая строка отдаётся обычным пустым литералом: «0x» без
 * цифр невалиден. */
inline std::string hex_literal(const std::string& value) {
    if (value.empty()) return "''";
    static const char kHex[] = "0123456789abcdef";
    std::string out = "0x";
    out.reserve(value.size() * 2 + 2);
    for (unsigned char c : value) {
        out += kHex[c >> 4];
        out += kHex[c & 0x0F];
    }
    return out;
}

/* Идентификатор годен, если sanitize_ident его не меняет: правило берётся
 * у core/security.cpp, а не выписывается здесь вторым, — две копии одного
 * правила разъедутся при первой же правке (D12). */
inline bool is_safe_identifier(const std::string& ident) {
    return !ident.empty() && security::sanitize_ident(ident) == ident;
}

/* Скрипт создания БД, пользователя и прав. Возвращает false с причиной,
 * если имя БД не годится: молчаливый отказ здесь означал бы, что человек
 * увидел пустой вывод mariadb и не понял, где причина. */
inline bool provision_sql(const std::string& db_name, const std::string& db_user,
                          const std::string& db_pass, std::string& sql,
                          std::string& err) {
    if (!is_safe_identifier(db_name)) {
        err = "небезопасное имя базы данных: допустимы латинские буквы, "
              "цифры, «_», «-» и «.»";
        return false;
    }
    if (db_user.empty()) {
        err = "пустое имя пользователя БД";
        return false;
    }
    /* Учётная запись — «пользователь@localhost». Часть с @localhost пишется
     * текстом в обоих операторах, поэтому она удвоена ('@''localhost'''):
     * это кавычки SQL-литерала, а не экранирование значения. */
    const std::string at_localhost = "@''localhost''";

    sql =
        "SET NAMES utf8mb4;\n"
        "SET @wp_db = " + hex_literal(db_name) + ";\n"
        "SET @wp_user = " + hex_literal(db_user) + ";\n"
        "SET @wp_pass = " + hex_literal(db_pass) + ";\n";

    /* По одному оператору на PREPARE: подготовленный запрос берёт ровно
     * один оператор, а склейка нескольких через «;» в строке — синтаксис
     * (проверено на MariaDB 11.8: ошибка 1064). */
    auto append = [&sql](const std::string& stmt) {
        sql += "SET @wp_sql = " + stmt + ";\n"
               "PREPARE wp_stmt FROM @wp_sql;\n"
               "EXECUTE wp_stmt;\n"
               "DEALLOCATE PREPARE wp_stmt;\n";
    };
    /* Обратные кавычки — обычные символы внутри строкового литерала
     * (проверено на MariaDB 11.8, в том числе при sql_mode с
     * NO_BACKSLASH_ESCAPES): «\`» внутри литерала — уже экранирование,
     * зависящее от режима, а сырой обратный апостроф в строке не значит
     * ничего. */
    append("CONCAT('CREATE DATABASE IF NOT EXISTS `', @wp_db, '`')");
    append("CONCAT('CREATE USER IF NOT EXISTS ', QUOTE(@wp_user), '"
           + at_localhost + " IDENTIFIED BY ', QUOTE(@wp_pass))");
    append("CONCAT('GRANT ALL ON `', @wp_db, '`.* TO ', QUOTE(@wp_user), '"
           + at_localhost + "')");
    sql += "FLUSH PRIVILEGES;\n";
    return true;
}

/* Команда шага БД. SQL уходит на stdin, поэтому в строке команды нет ни
 * пароля, ни самого SQL — только путь к файлу. */
inline std::string run_command(const std::string& sql_path) {
    return "sudo mariadb < " + shell::shell_quote(sql_path);
}

/* Файл скрипта на время шага.
 *
 * Пароль БД лежит в нём доли секунды, поэтому 0600 и O_EXCL: имя не должно
 * достануться другому процессу, а файл не должен появиться с правами по
 * умолчанию. Снимается деструктором на ЛЮБОМ выходе, включая ранний
 * return. Чего класс НЕ делает: если процесс убить между созданием и
 * работой деструктора, файл останется — лечится только тем, что каталог
 * временных файлов убирает его сам (и об этом сказано здесь, а не
 * спрятано в реализации). */
class TempScript {
public:
    explicit TempScript(const std::string& sql) {
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec);
        if (ec) {
            error_ = "не найден каталог для временных файлов";
            return;
        }
        static std::atomic<unsigned> counter{0};
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::filesystem::path p =
                dir / ("wp_coder_sql_" + std::to_string(::getpid()) + "_" +
                       std::to_string(counter.fetch_add(1)) + ".sql");
            const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0) continue;   /* имя занято — следующее */
            const ssize_t n = ::write(fd, sql.data(), sql.size());
            ::close(fd);
            if (n != static_cast<ssize_t>(sql.size())) {
                std::filesystem::remove(p, ec);
                error_ = "не удалось записать SQL во временный файл";
                return;
            }
            path_ = p.string();
            return;
        }
        error_ = "не удалось создать временный файл SQL";
    }
    ~TempScript() {
        std::error_code ec;
        if (!path_.empty()) std::filesystem::remove(path_, ec);
    }
    TempScript(const TempScript&) = delete;
    TempScript& operator=(const TempScript&) = delete;

    bool ok() const { return !path_.empty(); }
    const std::string& path() const { return path_; }
    const std::string& error() const { return error_; }

private:
    std::string path_;
    std::string error_;
};

} // namespace sql
} // namespace wp
} // namespace coder
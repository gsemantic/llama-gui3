#pragma once

/*
 * base64.h — кодирование байтов в base64 (RFC 4648) одним заголовком.
 *
 * Зачем отдельный файл, если в проекте уже есть base64 в
 * plugins/user_plugins/news_rewriter/src/sink_wordpress.cpp: та функция
 * живёт ВНУТРИ плагина и нужна ему для HTTP Basic. Хосту (core) она
 * недоступна, а вложение-картинка кодируется именно в хосте — там, где
 * файл превращается в реплику для модели. Заводить вторую копию в core
 * значило бы ровно то, от чего предостерегает D12: два кодека разъедутся
 * на первом же изменении алфавита или переносах строк.
 *
 * Кодируется БЕЗ переносов строк: base64 с \r\n каждые 76 символов
 * (так делают старые MIME-кодеки) внутри JSON-строки превращается в
 * \\r\\n, и провайдер получает data-URL, в котором переносы — часть
 * данных. Изображение при этом не декодируется.
 *
 * Никаких внешних зависимостей: алгоритм на 20 строк, и тянуть ради него
 * OpenSSL в ядро — цена больше пользы.
 */

#include <cstddef>
#include <string>

namespace llama_gui {
namespace core {

/* Кодирует произвольные байты (в т.ч. с NUL) в base64. Пустая строка —
 * пустая строка: это не ошибка, а «нечего кодировать». */
inline std::string base64_encode(const std::string& input) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const size_t n = input.size();
    std::string out;
    out.reserve(((n + 2) / 3) * 4);

    size_t i = 0;
    /* Тройки байтов → четыре символа. */
    for (; i + 2 < n; i += 3) {
        const unsigned b0 = static_cast<unsigned char>(input[i]);
        const unsigned b1 = static_cast<unsigned char>(input[i + 1]);
        const unsigned b2 = static_cast<unsigned char>(input[i + 2]);
        out += kAlphabet[(b0 >> 2) & 0x3F];
        out += kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
        out += kAlphabet[((b1 & 0x0F) << 2) | (b2 >> 6)];
        out += kAlphabet[b2 & 0x3F];
    }

    /* Хвост из одного или двух байтов: «=» — не данные, а наполнитель,
     * и без него половина шлюзов отвергает data-URL как обрезанный. */
    const size_t rest = n - i;
    if (rest == 1) {
        const unsigned b0 = static_cast<unsigned char>(input[i]);
        out += kAlphabet[(b0 >> 2) & 0x3F];
        out += kAlphabet[(b0 & 0x03) << 4];
        out += "==";
    } else if (rest == 2) {
        const unsigned b0 = static_cast<unsigned char>(input[i]);
        const unsigned b1 = static_cast<unsigned char>(input[i + 1]);
        out += kAlphabet[(b0 >> 2) & 0x3F];
        out += kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
        out += kAlphabet[(b1 & 0x0F) << 2];
        out += '=';
    }

    return out;
}

/* Обратное преобразование. Нужно не для отправки, а для проверок: «то, что
 * ушло, — это indeed картинка» иначе проверяется сравнением с исходником,
 * то есть тем же кодом, который кодировал.
 *
 * Мусор (символ вне алфавита, «=» в середине) — ошибка: возвращается
 * false, а не молча испорченная строка.data-URL приходит из файла, то
 * есть от посторонних, и «разобрать как получится» здесь означало бы
 * отдать модели произвольные байты. */
inline bool base64_decode(const std::string& input, std::string& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    out.clear();
    out.reserve((input.size() / 4) * 3);

    int quad[4];
    int qn = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '=') {
            /* Наполнитель допустим только в последней quadruple. */
            if (i + 1 != input.size() && input[i + 1] != '=') return false;
            quad[qn++] = -2;
        } else {
            const int v = value(c);
            if (v < 0) return false;
            quad[qn++] = v;
        }
        if (qn < 4) continue;
        qn = 0;

        const int pad = (quad[2] == -2 ? 2 : (quad[3] == -2 ? 1 : 0));
        const int b0 = (quad[0] << 2) | (quad[1] >> 4);
        out += static_cast<char>(b0 & 0xFF);
        if (pad < 2) {
            const int b1 = ((quad[1] & 0x0F) << 4) | (quad[2] >> 2);
            out += static_cast<char>(b1 & 0xFF);
        }
        if (pad < 1) {
            const int b2 = ((quad[2] & 0x03) << 6) | quad[3];
            out += static_cast<char>(b2 & 0xFF);
        }
    }
    /* Остаток 1–3 символа — обрезанный base64. */
    return qn == 0;
}

} // namespace core
} // namespace llama_gui
#pragma once

/*
 * image_mime.h — определение MIME изображения ПО СОДЕРЖИМОМУ.
 *
 * Почему не по расширению: расширение — это обещание файловой системы, а
 * содержимое — факт. `.png`, внутри которого HTML (так выглядит страница
 * ошибки, сохранённая как картинка), при доверии расширению уехал бы
 * модели как base64 от HTML с `"type":"image_url"` — то есть модель
 * получила бы «изображение», которого нет, и ответила бы «на картинке
 * ничего нет». Верное поведение — не считать это картинкой вовсе.
 *
 * Почему не через libmagic: его нет в зависимостях проекта, а тянуть
 * libmagic в ядро ради распознавания десятка форматов — цена больше
 * пользы. Проверяются МАГИЧЕСКИЕ СИГНАТУРЫ, то есть те же байты, по
 * которым libmagic и опознаёт: формат задаётся первыми байтами файла, и
 * это не эвристика, а стандарт.
 *
 * Проверяются только форматы, которые умеют провайдеры и llama.cpp:
 * PNG, JPEG, GIF, WebP, BMP. TIFF и HEIC не умеет никто из них, и
 * объявить их картинкой значило бы отправить тело, которое шлюз отвергнет
 * ошибкой разбора — причём ошибка пришла бы от провайдера, а причина была
 * бы здесь.
 */

#include <string>

namespace llama_gui {
namespace core {

/* Возвращает MIME ("image/png") или пустую строку, если это не
 * распознанное изображение. Пустая строка — не ошибка: «не картинка»
 * это нормальный ответ для текстового файла. */
inline std::string detect_image_mime(const std::string& data) {
    const auto starts_with = [&data](const char* sig, size_t n) {
        return data.size() >= n && data.compare(0, n, sig, n) == 0;
    };

    /* PNG: 89 50 4E 47 0D 0A 1A 0A */
    static const char kPng[] = "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A";
    if (starts_with(kPng, sizeof(kPng) - 1)) return "image/png";

    /* JPEG: FF D8 FF — три байта, и этого достаточно: SOI-маркер. */
    static const char kJpg[] = "\xFF\xD8\xFF";
    if (starts_with(kJpg, sizeof(kJpg) - 1)) return "image/jpeg";

    /* GIF: "GIF87a" или "GIF89a" */
    if (starts_with("GIF87a", 6) || starts_with("GIF89a", 6)) return "image/gif";

    /* WebP: "RIFF" + 4 байта размера + "WEBP". Размер не проверяется:
     * он усечён в заголовке намеренно и на тип не влияет. */
    if (data.size() >= 12 && starts_with("RIFF", 4) &&
        data.compare(8, 4, "WEBP") == 0) {
        return "image/webp";
    }

    /* BMP: "BM" + зарезервированные 4 байта. Двух байт мало, поэтому
     * проверяется длина: настоящий BMP — минимум 26 байт заголовка. */
    if (starts_with("BM", 2) && data.size() >= 26) return "image/bmp";

    return std::string();
}

} // namespace core
} // namespace llama_gui
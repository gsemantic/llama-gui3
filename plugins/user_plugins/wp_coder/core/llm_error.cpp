/*
 * llm_error.cpp — тело классификатора отказов провайдера (И12.5).
 * Обоснование — в llm_error.h; здесь только код и разбор по спискам.
 */

#include "llm_error.h"

#include <cctype>

namespace coder {
namespace {

/* Приведение регистра: ASCII и кириллица, по кодам символов UTF-8.
 *
 * Кириллица здесь — не украшение. Текст ошибки наполовину НАШ: плагин
 * сам пишет «Таймаут LLM-вызова (30 с)», «Прервано пользователем»,
 * «Хост не поддерживает блокирующий вызов LLM» (src/plugin_main.cpp), и
 * именно они доходят до классификатора. Версия, своя ASCII (буквы выше
 * 127 не трогались), оставляла «Таймаут» как есть, и признак «таймаут»
 * не находился никогда: русский таймаут молча уходил в «не повторять».
 * Поймал тест — и это ровно тот случай, который проверка на одной строке
 * не увидела бы.
 *
 * Свой код, а не локаль: ::tolower зависит от LC_ALL, а классификация
 * обязана давать один и тот же ответ на любой машине. Раскладка, кроме
 * ASCII и кириллицы, сознательно не трогается — иначе пришлось бы тащить
 * таблицы Unicode в проект, где их быть не должно. */
std::string lower_case(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        /* ASCII: однобайтовый, сдвиг на 32. */
        if (c < 0x80) {
            out.push_back(static_cast<char>(c >= 'A' && c <= 'Z'
                                                ? c + 32
                                                : c));
            ++i;
            continue;
        }
        /* Двухбайтный UTF-8 с ведущим 0xD0/0xD1 — это кириллица. */
        if ((c == 0xD0u || c == 0xD1u) && i + 1 < in.size()) {
            const unsigned char lo = static_cast<unsigned char>(in[i + 1]);
            const unsigned int cp =
                (static_cast<unsigned int>(c & 0x1Fu) << 6) |
                static_cast<unsigned int>(lo & 0x3Fu);
            /* А-Я → а-я. Только этот диапазон, и граница названа: Ё
             * (U+0401) лежит ВНЕ его, и пара Ё↔ё намеренно не
             * обрабатывается — ни один признак ниже не содержит «ё», а
             * ветка под непроверяемый признак была бы кодом, который
             * выглядит как забота и ничего не делает. Если признак с «ё»
             * понадобится, пара добавляется здесь же и проверяется
             * таблицей. */
            unsigned int mapped =
                (cp >= 0x0410u && cp <= 0x042Fu) ? cp + 0x20u : cp;
            out.push_back(static_cast<char>(0xC0u | (mapped >> 6)));
            out.push_back(static_cast<char>(0x80u | (mapped & 0x3Fu)));
            i += 2;
            continue;
        }
        /* Остальное — как есть, байт за байтом. */
        out.push_back(static_cast<char>(c));
        ++i;
    }
    return out;
}

bool contains_any(const std::string& hay, const char* const* needles,
                  size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (hay.find(needles[i]) != std::string::npos) return true;
    }
    return false;
}

/* Отмена хода.
 *
 * Это НЕ ошибка провайдера, и на блокирующем пути она приходит именно как
 * текст: `src/plugin_main.cpp` пишет в `LlmReply::error` строку
 * «Прервано пользователем», а `llm_source::fetch` (core/llm_source.cpp)
 * превращает непустой error в provider_error. То есть на этом пути отмена
 * классифицировалась как Provider — то есть «стоп» выглядел как падение
 * сети, ровно то, что И11.14 закрыл для стриминга.
 *
 * Наш собственный текст, а не чужой: «прервано пользователем» не может
 * прийти от провайдера, поэтому признак не опасен для чужих сообщений. */
const char* const kAbortedNeedles[] = {
    "прервано пользователем", "остановлено пользователем",
};

/* Признак доступа: ключ недействителен, подписки нет, прав не хватает.
 * Ни один из них не проходит с повтором — ключ сам собой не станет
 * действительным, и три попытки лишь утроят время ожидания человека. */
const char* const kAuthNeedles[] = {
    "401", "403", "unauthorized", "authentication", "invalid api key",
    "invalid_api_key", "no api key", "api key not", "forbidden",
    "permission denied", "нет ключа", "ключ недействителен",
    "нет доступа", "доступ запрещ",
};

/* Сработавший фильтр содержимого. Отказ провайдера, а не наш: модель
 * ответила, но ответ не доведён. Повтор с тем же запросом вернёт то же
 * самое. */
const char* const kContentFilterNeedles[] = {
    "content_filter", "content filter", "content policy", "safety",
    "moderation", "flagged", "filtered by", "prohibited content",
    "заблокирован", "фильтр содержимого", "нарушает политику",
};

/* Контекст не поместился. Лечится сжатием истории (И7), а не ожиданием:
 * те же входные данные на следующей попытке дадут тот же отказ. */
const char* const kContextOverflowNeedles[] = {
    "context length", "context_length", "exceeds context", "maximum context",
    "context window", "too many tokens", "prompt is too long",
    "исчерпан", "превыш", "не помещается", "контекст",
};

/* Транзиентность сетевого отказа: повтор имеет смысл. Всё остальное
 * (400, 404, 422, пустое «ok=0») повторением не лечится. */
const char* const kTransientNeedles[] = {
    "429", "500", "502", "503", "504", "5xx",
    "timeout", "timed out", "connection", "temporarily", "reset by peer",
    "таймаут", "временно", "соединение", "сеть недоступна",
};

} // namespace

ProviderFailure classify_provider_failure(const std::string& error) {
    ProviderFailure out;
    const std::string low = lower_case(error);

    /* Отмена — ПЕРВОЙ, раньше всего остального: это наш собственный текст,
     * самый точный из всех и ни с одним другим не пересекается. */
    if (contains_any(low, kAbortedNeedles, sizeof(kAbortedNeedles) /
                                               sizeof(*kAbortedNeedles))) {
        out.kind = FailureKind::Aborted;
        out.retryable = false;
        return out;
    }
    if (contains_any(low, kAuthNeedles, sizeof(kAuthNeedles) /
                                               sizeof(*kAuthNeedles))) {
        out.kind = FailureKind::Auth;
        out.retryable = false;
        return out;
    }
    if (contains_any(low, kContentFilterNeedles,
                     sizeof(kContentFilterNeedles) /
                         sizeof(*kContentFilterNeedles))) {
        out.kind = FailureKind::ContentFilter;
        out.retryable = false;
        return out;
    }
    /* Контекст ПЕРЕД транзиентностью, и это не порядок «по важности», а
     * порядок «по специфичности»: «исчерпан» стоял в одном списке с «429»
     * в старом is_retryable_error и потому повторялся. */
    if (contains_any(low, kContextOverflowNeedles,
                     sizeof(kContextOverflowNeedles) /
                         sizeof(*kContextOverflowNeedles))) {
        out.kind = FailureKind::ContextOverflow;
        out.retryable = false;
        return out;
    }
    out.kind = FailureKind::Provider;
    out.retryable = contains_any(low, kTransientNeedles,
                                 sizeof(kTransientNeedles) /
                                     sizeof(*kTransientNeedles));
    return out;
}

FailureKind failure_for_finish_reason(const std::string& finish_reason) {
    if (finish_reason.empty()) return FailureKind::None;
    const std::string low = lower_case(finish_reason);
    if (low == "length" || low == "max_tokens") return FailureKind::OutputLength;
    if (low == "content_filter") return FailureKind::ContentFilter;
    return FailureKind::None;
}

} // namespace coder
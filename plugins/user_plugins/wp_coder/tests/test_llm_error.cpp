/*
 * test_llm_error.cpp — И12.5: таксономия ошибок провайдера.
 *
 * ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ И ПОЧЕМУ ИМЕННО ТАК
 * ------------------------------------------
 * Таксономия ценна ровно тем, что РАЗЛИЧАЕТ случаи, которые раньше были
 * одним. Поэтому главные проверки здесь — не «функция вернула непустое», а
 * таблица «текст → вид и повторяемость», где каждая строка обязана отличаться
 * от соседней.
 *
 * ГЛАВНОЕ УТВЕРЖДЕНИЕ ЗАДАЧИ, И ОНО ПРОТИВ СТАРОГО ПОВЕДЕНИЯ. До И12.5
 * «исчерпан» (контекст закончился) стоял в одном списке с «429» и считался
 * ПОВТОРЯЕМЫМ. Повтор запроса, который не помещается в контекст, не поместится
 * и в третий раз: те же входные данные дают тот же отказ. Проверка названа
 * отдельно и стоит первой по порядку чтения, потому что это единственное
 * место, где задача меняет поведение, а не только добавляет вид.
 *
 * ПОЧЕМУ ПРОВЕРЯЕТСЯ И СОБЫТИЕ, И ОТВЕТ. Классификатор — чистая функция, и
 * проверка только на ней была бы зелёной при коде, который решает правильно и
 * ничего с решением не делает: то же, что с мёртвым executeShortcut. Вид и
 * повторяемость обязаны ДОЕХАТЬ до LlmEvent и до LlmResponse, потому что их
 * читают ретраи и UI.
 *
 * ПОЧЕМУ ОТДЕЛЬНО ПРОВЕРЯЕТСЯ, ЧТО ОТМЕНА НЕ КЛАССИФИЦИРУЕТСЯ ПО ТЕКСТУ.
 * `aborted()` собирает событие напрямую, минуя классификатор. Если бы он
 * пошёл через provider_error, то «Прервано пользователем» попал бы под
 * проверку подстрок — и «стоп» стал бы либо «фильтром содержимого» (нет), либо
 * Provider, то есть «стоп» снова выглядел бы как падение сети. Это ровно тот
 * случай, который И11.14 уже закрывал и который легко открыть заново.
 */

#include "test_framework.h"
#include "test_printers.h"
#include "../core/llm_error.h"
#include "../core/llm_event.h"

#include <iostream>
#include <string>
#include <vector>

using namespace coder;

namespace {

struct Case {
    const char* error;
    FailureKind kind;
    bool retryable;
    const char* why;
};

/* Порядок строк — по убыванию специфичности, как в коде: так что если
 * классификатор переставит проверки местами, разойдутся именно эти строки,
 * а не случайные. */
const Case kCases[] = {
    /* --- доступ: не повторяется ни при каких условиях --- */
    {"401 Unauthorized",              FailureKind::Auth,          false, "нет ключа"},
    {"HTTP 403 Forbidden",            FailureKind::Auth,          false, "нет прав"},
    {"invalid api key",               FailureKind::Auth,          false, "ключ битый"},
    {"invalid_api_key",               FailureKind::Auth,          false, "то же snake_case"},
    {"UNAUTHORIZED: bad credentials", FailureKind::Auth,          false, "регистр не важен"},
    {"нет доступа к модели",          FailureKind::Auth,          false, "русский текст"},

    /* --- фильтр содержимого: ответ пришёл, но не доведён --- */
    {"content_filter",                FailureKind::ContentFilter, false, "прямое имя"},
    {"blocked by content policy",     FailureKind::ContentFilter, false, "политика"},
    {"flagged",                       FailureKind::ContentFilter, false, "модерация"},

    /* --- контекст: ГЛАВНОЕ, повтор бессмыслен --- */
    {"context length exceeded",       FailureKind::ContextOverflow, false, "не поместилось"},
    {"maximum context length is 8192",FailureKind::ContextOverflow, false, "окно"},
    {"контекст исчерпан",             FailureKind::ContextOverflow, false, "русский текст"},

    /* --- сеть: повторяется --- */
    {"429 Too Many Requests",         FailureKind::Provider,      true,  "лимит частоты"},
    {"500 Internal Server Error",     FailureKind::Provider,      true,  "сервер"},
    {"503 Service Unavailable",       FailureKind::Provider,      true,  "недоступен"},
    {"Таймаут LLM-вызова (30 с)",     FailureKind::Provider,      true, "русский таймаут"},
    {"connection reset by peer",      FailureKind::Provider,      true, "обрыв"},

    /* --- сеть без признака транзиентности: повтор не лечит --- */
    {"400 Bad Request",               FailureKind::Provider,      false, "плохой запрос"},
    {"404 model not found",           FailureKind::Provider,      false, "нет модели"},
    {"LLM-ошибка (ok=0)",             FailureKind::Provider,      false, "провайдера нет причины"},
    {"",                              FailureKind::Provider,      false, "пустой текст"},
    {"что-то непонятное",             FailureKind::Provider,      false, "не узнано"},
};

void check_kind_named(FailureKind kind) {
    const char* n = failure_kind_name(kind);
    ASSERT_TRUE(n != nullptr);
    ASSERT_TRUE(std::string(n).size() > 0);
}

} // anonymous namespace

TEST(контекст_исчерпан_больше_не_повторяется) {
    /* Отдельной проверкой и ПЕРВОЙ, потому что это единственное место, где
     * задача меняет поведение. Старый is_retryable_error держал «исчерпан»
     * рядом с «429» и повторял запрос, который не помещается в контекст;
     * повтор не мог дать другого результата — те же входные данные. */
    const ProviderFailure f = classify_provider_failure("контекст исчерпан");
    ASSERT_EQ(f.kind, FailureKind::ContextOverflow);
    ASSERT_FALSE(f.retryable);
    /* И латиницей: провайдеры пишут по-английски, а признак обязан
     * находиться в обоих написаниях. */
    ASSERT_FALSE(classify_provider_failure("context length exceeded").retryable);
    /* А соседний случай с той же сетью — повторяется. Иначе проверка выше
     * была бы зелёной и на коде, который не повторяет НИЧЕГО. */
    ASSERT_TRUE(classify_provider_failure("429 Too Many Requests").retryable);
}

TEST(текст_отказа_даёт_вид_и_повторяемость) {
    for (const Case& c : kCases) {
        const ProviderFailure f = classify_provider_failure(c.error);
        if (f.kind != c.kind) {
            std::cerr << "  «" << c.error << "»: вид "
                      << failure_kind_name(f.kind) << ", ожидался "
                      << failure_kind_name(c.kind) << " (" << c.why << ")"
                      << std::endl;
            ASSERT_TRUE(false);
        }
        if (f.retryable != c.retryable) {
            std::cerr << "  «" << c.error << "»: повторяемость "
                      << (f.retryable ? "да" : "нет") << ", ожидалось "
                      << (c.retryable ? "да" : "нет") << " (" << c.why << ")"
                      << std::endl;
            ASSERT_TRUE(false);
        }
    }
}

TEST(специфичное_проверяется_раньше_общего) {
    /* Порядок проверок — не вкус. «Контекст исчерпан» на 500-символьном
     * сообщении содержит и сетевой признак; если бы общий проверялся
     * первым, переполнение снова стало бы повторяемым. Проверка собирает
     * такой текст намеренно. */
    const ProviderFailure f = classify_provider_failure(
        "ошибка 500: контекст исчерпан, запрос не помещается");
    ASSERT_EQ(f.kind, FailureKind::ContextOverflow);
    ASSERT_FALSE(f.retryable);
}

TEST(вид_сбоя_выводится_из_finish_reason) {
    ASSERT_EQ(failure_for_finish_reason("length"), FailureKind::OutputLength);
    ASSERT_EQ(failure_for_finish_reason("max_tokens"), FailureKind::OutputLength);
    ASSERT_EQ(failure_for_finish_reason("content_filter"),
              FailureKind::ContentFilter);
    /* Обычные завершения — не сбой. Иначе «length» отличался бы от
     * «stop» только начинением, и проверка выше проходила бы на коде,
     * который путает их местами с обратной стороны. */
    ASSERT_EQ(failure_for_finish_reason("stop"), FailureKind::None);
    ASSERT_EQ(failure_for_finish_reason("tool_calls"), FailureKind::None);
    ASSERT_EQ(failure_for_finish_reason(""), FailureKind::None);
}

TEST(у_каждого_вида_сбоя_есть_имя) {
    /* Полнота перечисления. Компилятор предупредил о switch в UI, который
     * не знает одного вида; здесь та же пустота ловится проверкой, а не
     * глазом. Имена обязаны быть РАЗНЫМИ: одинаковые сделали бы «ошибка
     * сети» на месте «доступ запрещён» — то есть ровно то, что задача
     * чинит. */
    const FailureKind kinds[] = {FailureKind::None,   FailureKind::Provider,
                                 FailureKind::Tool,    FailureKind::Aborted,
                                 FailureKind::ContextOverflow,
                                 FailureKind::Auth,    FailureKind::OutputLength,
                                 FailureKind::ContentFilter};
    std::vector<std::string> seen;
    for (FailureKind k : kinds) {
        check_kind_named(k);
        for (const std::string& s : seen) {
            ASSERT_TRUE(s != failure_kind_name(k));
        }
        seen.push_back(failure_kind_name(k));
    }
    ASSERT_EQ(seen.size(), (size_t)8);
}

TEST(событие_несёт_вид_и_повторяемость_из_текста) {
    /* Привязка: классификатор решил — событие обязано нести. Иначе вид в
     * событии один, а повторяемость в ретраях другая, то есть D12. */
    const LlmEvent auth = LlmEvent::provider_error("401 Unauthorized");
    ASSERT_EQ(auth.failure(), FailureKind::Auth);
    ASSERT_FALSE(auth.retryable());

    const LlmEvent net = LlmEvent::provider_error("503 Service Unavailable");
    ASSERT_EQ(net.failure(), FailureKind::Provider);
    ASSERT_TRUE(net.retryable());

    const LlmEvent over = LlmEvent::provider_error("context length exceeded");
    ASSERT_EQ(over.failure(), FailureKind::ContextOverflow);
    ASSERT_FALSE(over.retryable());
}

TEST(отмена_пользователем_не_классифицируется_по_тексту) {
    /* `aborted()` идёт мимо классификатора. Если бы он пошёл через
     * provider_error, «Прервано пользователем» проверялось бы подстроки —
     * и «стоп» снова стал бы выглядеть как падение сети. Это тот случай,
     * который И11.14 закрывал и который легко открыть заново. */
    const LlmEvent e = LlmEvent::aborted("Прервано пользователем");
    ASSERT_EQ(e.failure(), FailureKind::Aborted);
    ASSERT_FALSE(e.retryable());
    /* Вид СОБЫТИЯ остаётся прежним: разбор протокола не знает про кнопку,
     * различает вид СБОЯ (И11.14). */
    ASSERT_EQ(e.kind(), LlmEventKind::ProviderError);
}

TEST(свёртка_переносит_вид_и_повторяемость_в_ответ) {
    LlmResponse r;
    LlmResponse::reduce(r, LlmEvent::provider_error("429 Too Many Requests"));
    ASSERT_EQ(r.failure(), FailureKind::Provider);
    ASSERT_TRUE(r.retryable());
    ASSERT_FALSE(r.ok());

    LlmResponse auth;
    LlmResponse::reduce(auth, LlmEvent::provider_error("invalid api key"));
    ASSERT_EQ(auth.failure(), FailureKind::Auth);
    ASSERT_FALSE(auth.retryable());
}

TEST(обрезанный_ответ_это_сбой_вида_длина_но_ход_состоялся) {
    /* `length` приходит на УСПЕШНОМ ответе, где error пуст: искать его в
     * тексте ошибки бессмысленно, поэтому у него отдельный вход. И ok()
     * остаётся истинным — ответ пришёл, просто обрезан, и объявлять ход
     * несостоявшимся было бы неправдой. */
    LlmResponse r;
    LlmResponse::reduce(r, LlmEvent::text_delta("начало"));
    LlmResponse::reduce(r, LlmEvent::finish("length"));
    ASSERT_EQ(r.failure(), FailureKind::OutputLength);
    ASSERT_FALSE(r.retryable());
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.text(), std::string("начало"));

    /* Обычное завершение сбоя не ставит. */
    LlmResponse ok_r;
    LlmResponse::reduce(ok_r, LlmEvent::finish("stop"));
    ASSERT_EQ(ok_r.failure(), FailureKind::None);
}

TEST(фильтр_содержимого_приходит_и_по_причине_и_по_finish_reason) {
    /* Один и тот же вид двумя путями: провайдер, который не доведёт ответ,
     * может сказать это и в error, и в finish_reason. Свести к разным
     * видам значило бы показать пользователю два разных сообщения об одном
     * и том же отказе. */
    ASSERT_EQ(classify_provider_failure("content_filter").kind,
              FailureKind::ContentFilter);
    ASSERT_EQ(failure_for_finish_reason("content_filter"),
              FailureKind::ContentFilter);
}
TEST(русские_сообщения_плагина_классифицируются_так_же_как_латинские) {
    /* Текст ошибки наполовину НАШ: плагин сам пишет «Таймаут LLM-вызова
     * (30 с)» и «Прервано пользователем» в src/plugin_main.cpp, и именно
     * они доходят до классификатора.
     *
     * Первая версия приведения регистра трогала только ASCII, поэтому
     * «Таймаут» оставался с заглавной и признак «таймаут» не находился
     * НИКОГДА: русский таймаут молча уходил в «не повторять», то есть
     * переставал повторяться. Проверка на одной латинской строке этого бы
     * не увидела — и увидела только потому, что в таблице есть и русские
     * случаи. */
    ASSERT_EQ(classify_provider_failure("Таймаут LLM-вызова (30 с)").kind,
              FailureKind::Provider);
    ASSERT_TRUE(classify_provider_failure("Таймаут LLM-вызова (30 с)").retryable);
    /* Регистр латиницы не сломался вместе с кириллицей — приведение
     * стало сложнее, и прежняя однобайтовая ветка могла пропасть. */
    ASSERT_TRUE(classify_provider_failure("TIMEOUT LLM").retryable);
    ASSERT_EQ(classify_provider_failure("UNAUTHORIZED").kind,
              FailureKind::Auth);
}

TEST(отмена_на_блокирующем_пути_это_отмена_а_не_сбой_провайдера) {
    /* Дыра, найденная при написании таблицы: на блокирующем пути отмена
     * приходит ТЕКСТОМ. `src/plugin_main.cpp` пишет «Прервано
     * пользователем» в LlmReply::error, а `llm_source::fetch` превращает
     * непустой error в provider_error. То есть отмена классифицировалась
     * как Provider — «стоп» выглядел как падение сети, ровно то, что
     * И11.14 закрыл для стриминга, но не для этого пути. */
    const ProviderFailure f =
        classify_provider_failure("Прервано пользователем");
    ASSERT_EQ(f.kind, FailureKind::Aborted);
    ASSERT_FALSE(f.retryable);
    /* И тем же видом, что даёт aborted(): один факт — один вид, иначе
     * два пути отмены разошлись бы прямо здесь. */
    ASSERT_EQ(f.kind, LlmEvent::aborted("Прервано пользователем").failure());
    /* Чужая формулировка не должна ловиться: признак наш собственный,
     * иначе провайдер с фразой про паузу выглядел бы отменой. */
    ASSERT_TRUE(classify_provider_failure("request aborted by server").kind
                != FailureKind::Aborted);
}

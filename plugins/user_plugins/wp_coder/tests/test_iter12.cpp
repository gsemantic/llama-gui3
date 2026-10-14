/*
 * test_iter12.cpp — И12.8: связка четырёх областей, закрытых в И12.
 *
 * ЧТО ЗДЕСЬ, А ЧТО УЖЕ ЕСТЬ
 * -------------------------
 * Поимённо у И12.1, И12.3, И12.5 и И12.6 свои файлы проверок: выбор промпта
 * по модели, маршрутизация починки, таксономия ошибок, задержки повторов.
 * Каждый проверяет свою функцию. Чего не было ни в одном — ПРОХОДА ПО ЦЕПОЧКЕ
 * целиком, где отказ провайдера превращается в решение «повторять», решение —
 * в задержку, а задержка — в строку, которую видит человек.
 *
 * ПОЧЕМУ ЭТО НЕ «ЕЩЁ ОДНА ПРОВЕРКА ТОГО ЖЕ»
 * -----------------------------------------
 * Проверки отдельных функций проходят при коде, в котором они не соединены.
 * Здесь соединение и есть предмет: классификатор И12.5 возвращает признак,
 * политика И12.6 его читает, уведомление называет вид. Расхождение между
 * ними — это ровно D12, и оно невидимо ни одной из проверок по отдельности.
 * Именно такое «всё зелёное, а повторов не происходит» и было находкой
 * И12.6 (отклонение 208).
 *
 * ПОЧЕМУ СИМУЛЯЦИЯ ЦИКЛА, А НЕ ВЫЗОВ КРОВЫ
 * ---------------------------------------
 * Настоящий цикл живёт в `src/plugin_main.cpp`, он спит и ходит в хост, и
 * проверить его можно только с живым провайдером. Здесь повторяется ровно
 * его АРИФМЕТИКА — тот же порядок вызовов, — чтобы последовательность
 * задержек и момент остановки были проверены, а не объявлены.
 */

#include "test_framework.h"
#include "test_printers.h"
#include "../core/llm_error.h"
#include "../core/prompts.h"
#include "../core/retry.h"
#include "../core/tool_repair.h"

#include <iostream>
#include <string>
#include <vector>

using namespace coder;

namespace {

/* Политика из плана с выключенным джиттером: иначе последовательность
 * недостижима по определению и проверять её нечем. */
RetryPolicy no_jitter_policy() {
    RetryPolicy p;
    p.jitter = 0.0;
    return p;
}

} // anonymous namespace

TEST(последовательность_задержек_идёт_по_политике_и_останавливается) {
    const RetryPolicy p = no_jitter_policy();
    const std::string err = "503 Service Unavailable";

    /* Ровно тот порядок вызовов, что в цикле plugin_main.cpp: классифицировать,
     * решить, повторять ли, посчитать задержку, построить уведомление. */
    std::vector<int> delays;
    std::vector<std::string> notices;
    for (int attempt = 0; attempt < p.max_attempts; ++attempt) {
        const ProviderFailure f = classify_provider_failure(err);
        if (!should_retry(p, f.retryable, attempt)) break;
        const int d = retry_delay_ms(p, attempt, 0.5, -1);
        delays.push_back(d);
        notices.push_back(
            retry_notice(err, f.kind, attempt, p.max_attempts, d));
    }

    /* 2000, 4000, 8000, 16000 — и ВСЁ. Ровно четыре повтора при
     * max_attempts = 5: пятая попытка это уже пятое ОБРАЩЕНИЕ, а не пятый
     * повтор, и седьмой мутацией снимался именно этот лимит. */
    ASSERT_EQ(delays.size(), (size_t)4);
    ASSERT_EQ(delays[0], 2000);
    ASSERT_EQ(delays[1], 4000);
    ASSERT_EQ(delays[2], 8000);
    ASSERT_EQ(delays[3], 16000);
    /* Монотонный рост: задержка, которая уменьшилась, означала бы, что мы
     * повторяем раньше, чем обещали. */
    for (size_t i = 1; i < delays.size(); ++i) {
        ASSERT_TRUE(delays[i] > delays[i - 1]);
    }
    /* Уведомление на каждый повтор — их столько же, сколько повторов. */
    ASSERT_EQ(notices.size(), delays.size());
}

TEST(последовательность_прерывается_на_неповторяемом_отказе) {
    const RetryPolicy p = no_jitter_policy();
    /* Тот же цикл, но отказ меняет вид на третьем повторе. Это ровно тот
     * случай, где вид из И12.5 и решение из И12.6 обязаны согласоваться:
     * если бы цикл спрашивал только «повторяемо» у Provider без разбора
     * вида, он продолжал бы ждать после «ключ недействителен». */
    const std::vector<std::string> errors = {
        "503 Service Unavailable",
        "503 Service Unavailable",
        "invalid api key",       /* вид меняется на неповторяемый */
        "503 Service Unavailable",   /* уже не должно быть reached */
    };
    std::vector<int> delays;
    FailureKind last_kind = FailureKind::None;
    for (size_t attempt = 0; attempt < errors.size(); ++attempt) {
        const ProviderFailure f = classify_provider_failure(errors[attempt]);
        last_kind = f.kind;
        if (!should_retry(p, f.retryable, static_cast<int>(attempt))) break;
        delays.push_back(retry_delay_ms(p, static_cast<int>(attempt), 0.5, -1));
    }
    ASSERT_EQ(delays.size(), (size_t)2);
    ASSERT_EQ(last_kind, FailureKind::Auth);
}

TEST(контекст_исчерпан_останавливает_последовательность_сразу) {
    /* Тот же путь, вид ContextOverflow: не повторяем с ПЕРВОГО раза. Раньше
     * «исчерпан» стоял рядом с «429» и повторялся, то есть на четырёх
     * задержках до 16 секунд цикл ждал бы запрос, который не поместится и
     * на пятой попытке. */
    const RetryPolicy p = no_jitter_policy();
    int made = 0;
    for (int attempt = 0; attempt < p.max_attempts; ++attempt) {
        const ProviderFailure f =
            classify_provider_failure("context length exceeded");
        if (!should_retry(p, f.retryable, attempt)) break;
        ++made;
    }
    ASSERT_EQ(made, 0);
}

TEST(отказ_из_починки_и_повтор_из_таксономии_говорят_одним_языком) {
    /* Связка двух задач: починка И12.3 даёт модели результат вызова вместо
     * отказа, а таксономия И12.5 даёт тот же отказ вид и повторяемость. Если
     * бы починка превращала несуществующий инструмент в отказ ПРОВАЙДЕРА,
     * вид был бы Provider, и витрина ретраев решила бы, что такой вызов
     * надо повторить — то есть пять попыток позвать инструмент, которого
     * нет. */
    const ToolRepair repair = repair_tool_call("frobnicate");
    ASSERT_TRUE(repair.routed_to_invalid);

    /* Тот же текст, каким починка сообщает модели о ненайденном инструменте,
     * проходит через классификатор как Provider и не повторяется. */
    const ProviderFailure f =
        classify_provider_failure("Инструмент «frobnicate» не существует.");
    ASSERT_EQ(f.kind, FailureKind::Provider);
    ASSERT_FALSE(f.retryable);
    ASSERT_FALSE(should_retry(RetryPolicy(), f.retryable, 0));
}

TEST(уведомление_о_повторе_называет_тот_вид_который_вернул_классификатор) {
    /* Вид в строке берётся из ТОГО ЖЕ разбора, что и решение о повторе. Если
     * бы уведомление называло вид по-своему, человек читал бы «повтор» там,
     * где на самом деле отказ по режиму или по доступу, — и ждал бы зря. */
    struct Case { const char* err; FailureKind kind; bool retryable; };
    const Case cases[] = {
        {"503 Service Unavailable",  FailureKind::Provider,          true},
        {"invalid api key",           FailureKind::Auth,             false},
        {"content_filter",            FailureKind::ContentFilter,    false},
        {"context length exceeded",   FailureKind::ContextOverflow,  false},
    };
    for (const Case& c : cases) {
        const ProviderFailure f = classify_provider_failure(c.err);
        ASSERT_EQ(f.kind, c.kind);
        ASSERT_EQ(f.retryable, c.retryable);
        const std::string n = retry_notice(c.err, f.kind, 0, 5, 2000);
        if (n.find(failure_kind_name(f.kind)) == std::string::npos) {
            std::cerr << "  «" << c.err << "»: уведомление не называет вид "
                      << failure_kind_name(f.kind) << std::endl;
            ASSERT_TRUE(false);
        }
    }
}

TEST(у_локальной_семейства_нет_совета_про_патч_а_у_gpt_есть) {
    /* Связка И12.1 и И12.4: подмена инструментов для gpt-* (12.4) и акцент
     * семейства (12.1) обязаны решаться ОДНИМ условием, иначе модель
     * получит в промпте совет про инструмент, которого у неё в наборе нет.
     * Проверка здесь, а не в своём файле, потому что предмет — именно
     * согласованность двух решений. */
    struct Case { const char* model; bool advice; };
    const Case cases[] = {
        {"gpt-5",        true},
        {"gpt-4o-mini",  false},
        {"qwen3-coder",  false},
        {"llama-3",      false},
    };
    for (const Case& c : cases) {
        const std::string prompt = SystemPrompt::provider(c.model);
        const bool advice =
            prompt.find(SystemPrompt::kGptPatchEditingPrompt) != std::string::npos;
        if (advice != c.advice) {
            std::cerr << "  " << c.model << ": совет про apply_patch "
                      << (advice ? "есть" : "нет") << std::endl;
            ASSERT_TRUE(false);
        }
    }
}
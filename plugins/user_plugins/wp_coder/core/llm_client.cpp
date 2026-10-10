// llm_client.cpp — единый вход к LLM с живой доставкой событий (И6.5).

#include "llm_client.h"

#include "llm_source.h"
#include "json_utils.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>

namespace coder {
namespace {

/*
 * Живое состояние одного хода.
 *
 * Владеется разделяемым указателем, потому что на него смотрят колбэки
 * хоста, а они переживают вызов: если ждать on_done по condition_variable,
 * то после возврата из fetch состояние ещё нужно живому потоку (см. путь
 * по таймауту — он продолжает присылать дельты в никуда).
 */
struct LiveTurn {
    std::mutex mtx;
    std::condition_variable done_cv;
    bool done = false;

    /* После таймаута ход брошен: колбэки обязаны перестать что-либо
     * отдавать, иначе модель допишет текст в UI уже после того, как цикл
     * ушёл на следующий шаг, и пользователь увидит два ответа вперемешку. */
    std::atomic<bool> abandoned{false};

    /* Предпросмотр: что доставлять в on_event. */
    bool text_open = false;
    bool reasoning_open = false;
    std::map<std::string, bool> tool_open;   /* call_id → блок открыт */
    /* Финальный JSON приходит в on_done, который зовут уже после нашего
     * ожидания; он лежит здесь, а не в локальной переменной fetch: колбэк
     * живёт дольше вызова, и запись в локальную переменную была бы записью
     * по висячей ссылке. */
    std::string result_json;

    std::function<void(const LlmEvent&)> on_event;
    /* handle того же вызова, который передан хосту: отмена придёт ровно с
     * ним, и второй идентификатор разошёлся бы с первым. */
    void* host_handle = nullptr;
};

void emit(LiveTurn& turn, const LlmEvent& event)
{
    if (turn.abandoned.load()) return;
    if (turn.on_event) turn.on_event(event);
}

/* Живой текст. Пока блок печатается, он открыт; закрывается он вместе с
 * ходом, а не по последней дельте: провайдер может прислать пустую. */
void on_text_delta(LiveTurn& turn, const std::string& text)
{
    if (turn.abandoned.load() || text.empty()) return;
    if (!turn.text_open) {
        turn.text_open = true;
        emit(turn, LlmEvent::text_start());
    }
    emit(turn, LlmEvent::text_delta(text));
}

void on_reasoning_delta(LiveTurn& turn, const std::string& text)
{
    if (turn.abandoned.load() || text.empty()) return;
    if (!turn.reasoning_open) {
        turn.reasoning_open = true;
        emit(turn, LlmEvent::reasoning_start());
    }
    emit(turn, LlmEvent::reasoning_delta(text));
}

void on_tool_delta(LiveTurn& turn, const char* call_id, const char* tool_name,
                   const char* fragment)
{
    if (turn.abandoned.load() || !call_id || !fragment || !*fragment) return;
    const std::string id(call_id);
    /* Имя приходит только в первом фрагменте — так его и присылает
     * провайдер, и блок ToolInputStart его несёт (И5.1). Запоминать имя «на
     * потом» незачем: позже оно прийти не может, а событие дельты имени не
     * имеет вообще, так что сохранять было бы некуда. */
    if (!turn.tool_open[id]) {
        turn.tool_open[id] = true;
        emit(turn, LlmEvent::tool_input_start(
                        id, tool_name ? std::string(tool_name) : std::string()));
    }
    emit(turn, LlmEvent::tool_input_delta(id, std::string(fragment)));
}

/* Закрытие открытых блоков перед концом хода. Порядок тот же, что и при
 * разборе целого ответа: сначала текст, потом ввод вызовов. */
void close_open_blocks(LiveTurn& turn)
{
    if (turn.reasoning_open) {
        turn.reasoning_open = false;
        emit(turn, LlmEvent::reasoning_end());
    }
    if (turn.text_open) {
        turn.text_open = false;
        emit(turn, LlmEvent::text_end());
    }
    for (const auto& kv : turn.tool_open) {
        if (!kv.second) continue;
        emit(turn, LlmEvent::tool_input_end(kv.first));
    }
    turn.tool_open.clear();
}

/* Финальный JSON хоста → LlmReply. Та же раскладка, что у llm_chat_messages,
 * и разбирается теми же json::str / json::int_. */
LlmReply reply_from_result(const std::string& json_text)
{
    LlmReply reply;
    if (json::int_(json_text, "ok") != 1) {
        std::string err = json::str(json_text, "error");
        reply.error = err.empty() ? "поток закрылся без результата" : err;
        return reply;
    }
    reply.content = json::str(json_text, "content");
    reply.finish_reason = json::str(json_text, "finish_reason");
    reply.prompt_tokens = json::int_(json_text, "prompt_tokens");
    reply.completion_tokens = json::int_(json_text, "completion_tokens");
    return reply;
}

} // namespace

bool LlmClient::fetch(const HostCallbacks& cb, const std::string& sys_prompt,
                      const std::vector<ModelMessage>& history,
                      std::vector<LlmEvent>& out_events,
                      const std::function<void(const LlmEvent&)>& on_event,
int timeout_ms,
                       AbortToken* abort)
{
    /* И11.14: токен отменён ДО зова — ход не начинается вовсе.
     *
     * Без этой проверки отменённый ход всё равно уходил бы провайдеру:
     * ожидание начиналось бы со срезом в 50 мс, за это время хост отвечал
     * (или почти отвечал), и `done` побеждал `aborted` — то есть
     * «стоп» тихо превращался в успешный ход. Проверка найдена
     * проверкой, а не рассуждением: на исходном коде падал
     * `abort_before_the_call_stops_the_turn`.
     *
     * Стоит ли он того: «стоп» нажали, а ход всё равно оплачен провайдеру
     * и разобран циклом как обычный ответ. */
    if (abort && abort->aborted()) {
        out_events.push_back(LlmEvent::aborted("Прервано пользователем"));
        if (on_event) on_event(out_events.back());
        return false;
    }

    /* Нет стриминга (старый хост) — блокирующий путь. Колбэк при этом всё
     * равно получает события, иначе вызывающему пришлось бы держать две
     * ветки поведения, и одна со временем была бы забыта. */
    if (!cb.llm_chat_stream) {
        if (!llm_source::fetch(cb, sys_prompt, history, out_events)) return false;
        if (on_event) {
            for (const LlmEvent& e : out_events) on_event(e);
        }
        return true;
    }

    /* Пустой on_event — НЕ повод отказаться от стриминга. Иначе хост,
     * умеющий только поток, оказался бы непригоден для вызывающего, который
     * событий не хочет (например, в тесте, которому нужен результат): молча
     * уйдя на блокирующий путь, мы бы не заметили, что хост его не умеет.
     *
     * deliver — копия: параметр объявлен как const&, и «подменить колбэк»
     * через него нельзя, а писать вызывающему в двух местах значило бы
     * разойтись при следующей правке. */
    std::function<void(const LlmEvent&)> deliver = on_event;
    if (!deliver) deliver = [](const LlmEvent&) {};

    auto turn = std::make_shared<LiveTurn>();
    turn->on_event = deliver;
    /*
     * Лок берут ТОЛЬКО колбэки, а ожидание — только для wait_for. Держать
     * лок на время вызова хоста нельзя: хост зовёт on_done синхронно из
     * этого вызова, и on_done, берущий лок, повесил бы ход на себе же.
     * Гонку «on_done успел между вызовом и ожиданием» снимает предикат
     * wait_for, а не удержание лока.
     */
    const bool started = cb.llm_chat_stream(
        sys_prompt, history, std::string(),
        /* handle приходит до входа в хоста: отменять можно только живой
         * поток, и узнать его handle иначе неоткуда. */
        [turn](void* handle) { turn->host_handle = handle; },
        /* Захват turn ПО ЗНАЧЕНИЮ (копия разделяемого указателя), а не по
         * ссылке. Колбэки живут дольше вызова — это и есть смысл
         * разделяемого владения, и захват по ссылке свёл бы его на нет:
         * после таймаута «поздняя» дельта писала бы в локальную переменную
         * ушедшего кадра. */
        [turn](const char* text, int kind) {
            const std::string s = text ? text : "";
            std::lock_guard<std::mutex> lk(turn->mtx);
            if (kind == 1) on_reasoning_delta(*turn, s);
            else on_text_delta(*turn, s);
        },
        [turn](const char* call_id, const char* tool, const char* fragment) {
            std::lock_guard<std::mutex> lk(turn->mtx);
            on_tool_delta(*turn, call_id, tool, fragment);
        },
        [turn](const std::string& json_text) {
            std::lock_guard<std::mutex> lk(turn->mtx);
            turn->result_json = json_text;
            close_open_blocks(*turn);
            turn->done = true;
            turn->done_cv.notify_all();
        });

    {
        if (!started) {
            /* Хост не запустил поток: on_done не будет. Откатываемся на
             * блокирующий путь, чтобы ход не потерялся из-за того, что
             * хост отказал. */
            std::vector<LlmEvent> blocking;
            const bool ok = llm_source::fetch(cb, sys_prompt, history, blocking);
            for (const LlmEvent& e : blocking) deliver(e);
            out_events = std::move(blocking);
            return ok;
        }

        const int wait_ms = timeout_ms > 0 ? timeout_ms : engine().state().llm_timeout_ms;
        std::unique_lock<std::mutex> guard(turn->mtx);
        /* И11.14: ждём ТРИ условия, а не одно.
         *
         * Прежде здесь был `wait_for(..., предикат done)`, а параметр
         * `abort` принимался и не читался нигде: отмена, описанная в
         * шапке llm_client.h, не существовала. «Стоп» посреди хода ждал
         * ответа провайдера до конца — то есть выглядел как «он думает».
         *
         * Почему срез, а не предикат с abort->aborted(): у токена СВОЙ
         * condition_variable, и разбудить чужое ожидание из него нельзя
         * — а ждать приходится именно на done_cv. Поэтому ожидание
         * нарезается срезами и между ними спрашивается токен.
         *
         * Срез — 50 мс: отмена ощущается мгновенно, а ложных
         * пробуждений за минуту ожидания около тысячи, и каждая —
         * lock/unlock на мьютексе токена. Обратная сторона названа прямо:
         * это опрос, и его НЕЛЬЗЯ применять там, где отмена должна
         * будить работу (инструмент ждёт kill по токену — И6.7). Здесь
         * ждать и нечего: ход уже идёт, его прерывание делает хост. */
        constexpr int kAbortSliceMs = 50;
        bool finished = false;
        bool aborted = false;
        if (abort) {
            int waited = 0;
            while (waited < wait_ms) {
                const int slice = (wait_ms - waited) < kAbortSliceMs
                                      ? (wait_ms - waited) : kAbortSliceMs;
                if (turn->done_cv.wait_for(guard, std::chrono::milliseconds(slice),
                                           [&] { return turn->done; })) {
                    finished = true;
                    break;
                }
                if (abort->aborted()) {
                    aborted = true;
                    break;
                }
                waited += slice;
            }
        } else {
            finished = turn->done_cv.wait_for(
                guard, std::chrono::milliseconds(wait_ms),
                [&] { return turn->done; });
        }

        if (aborted) {
            /* Порядок «done важнее отмены» — не из вежливости, а из
             * безопасности хоста. `aborted` выставляется только в ветке,
             * где `done` ложно: если on_done уже пришёл, поток ЗАКРЫТ и
             * handle освобождён host'ом, а `llm_chat_cancel` по нему
             * обратился бы к освобождённому объекту. Поэтому отменяется
             * только живой поток — ровно то, что значит «отменять
             * запрос», и ровно то, чего нельзя сделать с уже пришедшим
             * ответом.
             *
             * Плата названа: если ответ успел прийти в то же окно, что и
             * «стоп», ход будет обработан как успешный. Это безопасно —
             * цикл всё равно проверяет abort_requested между шагами и
             * остановится, то есть лишнего действия агента не будет. */
            /* Ход рвётся НАСТОЯЩИМ отменением запроса, а не «перестаём
             * ждать»: иначе провайдер продолжал бы жечь деньги и держать
             * слот, а пользователь — смотреть на «печать», которой уже
             * никто не слушает.
             *
             * handle берётся ПОД локом и обнуляется: хост зовёт on_done
             * уже после отмены, и второй отменяющий вызов по тому же
             * handle был бы вызовом освобождённого объекта.
             *
             * Порядок «сначала флаг, потом unlock» обязателен: пока
             * ждёт on_done, он берёт тот же лок, и отмена, сделанная
             * после unlock, оставила бы колбэк дописывать в UI. */
            void* handle = turn->host_handle;
            turn->host_handle = nullptr;
            turn->abandoned.store(true);
            guard.unlock();
            if (handle && cb.llm_chat_cancel) cb.llm_chat_cancel(handle);
            /* Вид сбоя — Aborted, а не Provider: «стоп» не повторяют, а
             * «провайдер не ответил» повторяют (И6.8). */
            out_events.push_back(LlmEvent::aborted("Прервано пользователем"));
            return false;
        }

        if (!finished) {
            /* Хост не закрыл поток. Поток может прийти и позже, поэтому он
             * помечается брошенным: иначе «печать» продолжилась бы в UI
             * после того, как цикл ушёл дальше. */
            turn->abandoned.store(true);
            out_events.push_back(LlmEvent::provider_error(
                "таймаут LLM-потока: ответ не закрыт за " + std::to_string(wait_ms) + " мс"));
            return false;
        }
    }

    /* Отказ провайдера разбирается ЗДЕСЬ, а не через reply_to_events: тот
     * про поле error не знает вовсе — он раскладывает успешный ответ. На
     * блокирующем пути это делает llm_source::fetch, и если бы стриминг
     * молчал, провайдерская ошибка превратилась бы в пустой успешный ход:
     * ровно тот дефект, из-за которого И6.1 ввёл ok в on_done. */
    const LlmReply reply = reply_from_result(turn->result_json);
    if (!reply.error.empty()) {
        out_events.push_back(LlmEvent::provider_error(reply.error));
        return false;
    }

    /* Авторитетная последовательность — из финального ответа тем же
     * разбором, что и на блокирующем пути. Разбор один, поэтому разойтись
     * эти пути не могут; предпросмотр в on_event для этого не годится
     * (блок вызова в нём ещё не вырезан). */
    llm_source::reply_to_events(reply, out_events);
    return true;
}

} // namespace coder

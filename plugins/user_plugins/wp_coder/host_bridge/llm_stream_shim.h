#pragma once

/*
 * llm_stream_shim.h — мост от C-ABI хоста к std::function плагина (И6.4).
 *
 * Ядро плагина (core/) от хоста не зависит (D-7): там живут только
 * std::function. Мост — здесь, в host_bridge/, потому что только здесь
 * законно знать про LlamaHostApi. То же, что и probe: в core/ живёт лишь
 * слот в HostCallbacks, и он не упоминает ABI.
 *
 * Смысл моста в том, что вызывающий не различает хосты: тот же вызов
 * работает и на хосте со стримингом, и на хосте без него, возвращая false в
 * последнем случае. Иначе каждое место зова LLM спрашивало бы «а есть ли
 * поле?» — и промах в одном таком месте дал бы «молча работает на старых
 * хостах, падает на новых», то есть наоборот.
 */

#include "llm_stream_probe.h"
#include "core/engine.h"

#include <functional>
#include <string>
#include <vector>

namespace coder {
namespace host {

/*
 * Прервать поток по handle (И6.6). handle — тот же указатель, который был
 * передан в call_llm_chat_stream как user_data. Пустой api или отсутствие
 * поля — не ошибка, а «хост не умеет»: генерация дойдёт до конца, и об этом
 * сказано в заголовке LlmClient, а не спрятано.
 */
bool cancel_llm_chat_stream(const LlamaHostApi* api, void* handle);

/*
 * Состояние одного стриминг-вызова, разделяемое с колбэками хоста.
 *
 * Живёт в shared_ptr, а не в стеке: колбэки приходят из потока хоста уже
 * после того, как вызов вернулся, и живут дольше вызова (см. требование в
 * plugin_api.h). Утечка возможна только если хост нарушит свой контракт и
 * не вызовет on_done вовсе — тогда вызывающий всё равно зависнет на
 * ожидании, и утечка станет наименьшей из двух проблем.
 */
struct StreamCallContext;

/*
 * Стриминговый запрос к хосту.
 *
 * on_delta(text, kind), on_tool_delta(call_id, tool_name, fragment) —
 * по мере поступления. on_done(result_json) — ровно один раз, когда поток
 * закрылся; result_json имеет раскладку ответа llm_chat_messages.
 *
 * Возвращает false, если поле у хоста отсутствует или поток не запустился.
 * В этом случае on_done НЕ вызывается — иначе вызывающий получил бы
 * два закрытия на один вызов.
 */
bool call_llm_chat_stream(const LlamaHostApi* api, LlamaPluginHost* host,
                          const std::string& sys_prompt,
                          const std::vector<ModelMessage>& messages,
                          const std::string& request_json,
                          const std::function<void(void* handle)>& on_started,
                          const std::function<void(const char* text, int kind)>& on_delta,
                          const std::function<void(const char* call_id,
                                                    const char* tool_name,
                                                    const char* fragment)>& on_tool_delta,
                          const std::function<void(const std::string& result_json)>& on_done);

/*
 * История диалога в раскладке messages_json.
 *
 * Одна реализация на оба вызова: стриминговый и блокирующий. Две копии
 * этой сборки разошлись бы при первом же новом поле реплики, и модель
 * получила бы разную историю в зависимости от того, какой путь выбрал
 * вызывающий.
 */
std::string messages_to_json(const std::vector<ModelMessage>& messages);

} // namespace host
} // namespace coder

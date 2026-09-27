#pragma once

/*
 * llm_stream_bridge.h — раскладка чанков провайдера на колбэки плагина.
 *
 * Зачем отдельный файл, если «позвать колбэк» — одна строка. Стриминг
 * приносит из сети протокол: SSE-чанк, у которого есть delta.content,
 * delta.reasoning_content, delta.tool_calls (аргументы приходят кусками),
 * finish_reason и usage. Разбор этого протокола нужен в двух местах —
 * локальному пути (LlamaInterface::create_chat_completion_streaming отдаёт
 * сырой JSON чанка) и облачному (OpenRouterClient отдаёт готовый токен).
 * Если разбор живёт в plugin_manager.cpp, он там и размножается, а
 * проверить его без сети и без PluginManager нельзя.
 *
 * Здесь он pure: сети нет, состояния приложения нет, поток один. Поэтому
 * tests/core/test_llm_stream_bridge.cpp проверяет его, не поднимая ни
 * сервера, ни UI.
 *
 * Что здесь НЕ живёт и почему:
 *   - что делать с дельтой (писать в UI, строить текст): это плагин;
 *   - событийная модель LlmEvent: это ядро плагина (core/llm_event.h),
 *     хост о ней не знает и знать не должен — иначе ABI поехал бы
 *     вместе с моделью событий плагина;
 *   - отмена: хост не умеет останавливать поток по требованию одного
 *     плагина (это И6.6), здесь её нет намеренно.
 */

#include <map>
#include <string>

namespace llama_gui {
namespace plugin {

/*
 * Аккумулятор одного стриминг-вызова хоста.
 *
 * Экземпляр живёт дольше, чем один вызов: он переживает и поток чанков,
 * и вызов on_done, поэтому хоста он держит в shared_ptr, а в колбэки
 * отдаёт сырой указатель. Колбэки приходят из потока провайдера
 * последовательно, гонки за состояние нет.
 */
class StreamBridge {
public:
    using DeltaFn = void (*)(void* user_data, const char* text, int kind);
    using ToolDeltaFn = void (*)(void* user_data, const char* call_id,
                                 const char* tool_name, const char* json_fragment);

    StreamBridge(void* user_data, DeltaFn on_delta, ToolDeltaFn on_tool_delta);

    /*
     * Один чанк провайдера — содержимое поля "data:" без префикса.
     * Мусор в чанке не роняет поток, но и не остаётся незамеченным:
     * число пропущенных чанков уходит в done_json, потому что дырка в
     * середине ответа иначе выглядит как связный текст.
     */
    void feed(const char* chunk_json);

    /* Готовый кусок текста (облачный путь отдаёт токен, а не чанк). */
    void push_text(const std::string& text);

    /* Метрики из финального чанка. */
    void set_usage(int prompt_tokens, int completion_tokens);

    /* Ответ закрыт провайдером. */
    void set_finish_reason(const std::string& reason);

    /* Провайдер сообщил об ошибке в конце потока. */
    void set_error(const std::string& error);

    /* Поток дошёл до закрывающего finish_reason. */
    bool closed() const { return closed_; }

    /* JSON для on_done: та же раскладка, что у ответа llm_chat_messages.
     * Ошибка (явная или записанная set_error) — ok=0; поток без закрывающего
     * finish_reason — тоже ok=0, потому что такой ответ неполон, и плагин
     * обязан сказать об этом, а не принять обрыв за пустой итог. */
    std::string done_json(const std::string& error) const;

    /* Идентификатор вызова по индексу провайдера.
     *
     * Провайдеры нумеруют вызовы индексом, а не строкой: без синтеза
     * "call_0"/"call_1" две параллельные сборки аргументов склеились бы в
     * один вызов, и результат второго достался бы не тому. */
    static std::string call_id_for_index(int index);

private:
    struct ToolCallSlot {
        std::string call_id;
        std::string name;
    };

    void emit_text(const std::string& text, int kind);
    void emit_tool_delta(const ToolCallSlot& slot, const std::string& fragment);

    void* user_data_;
    DeltaFn on_delta_;
    ToolDeltaFn on_tool_delta_;

    std::string content_;
    std::string finish_reason_;
    std::string error_;
    int prompt_tokens_ = 0;
    int completion_tokens_ = 0;
    int skipped_chunks_ = 0;
    bool closed_ = false;
    /* Индекс вызова → его идентификатор и имя. */
    std::map<int, ToolCallSlot> tool_calls_;
};

} // namespace plugin
} // namespace llama_gui

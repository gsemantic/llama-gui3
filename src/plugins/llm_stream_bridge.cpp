// llm_stream_bridge.cpp — раскладка чанков провайдера на колбэки плагина.

#include "llm_stream_bridge.h"

#include "plugins/plugin_api.h" // LLAMA_STREAM_DELTA_TEXT / _REASONING

#include <nlohmann/json.hpp>

namespace llama_gui {
namespace plugin {
namespace {

using nlohmann::json;

} // namespace

StreamBridge::StreamBridge(void* user_data, DeltaFn on_delta,
                           ToolDeltaFn on_tool_delta)
    : user_data_(user_data), on_delta_(on_delta), on_tool_delta_(on_tool_delta) {}

std::string StreamBridge::call_id_for_index(int index) {
    return "call_" + std::to_string(index);
}

void StreamBridge::emit_text(const std::string& text, int kind) {
    if (text.empty()) return;
    if (kind == LLAMA_STREAM_DELTA_TEXT) content_ += text;
    if (on_delta_) on_delta_(user_data_, text.c_str(), kind);
}

void StreamBridge::emit_tool_delta(const ToolCallSlot& slot,
                                   const std::string& fragment) {
    if (fragment.empty()) return;
    if (on_tool_delta_) {
        on_tool_delta_(user_data_, slot.call_id.c_str(), slot.name.c_str(),
                       fragment.c_str());
    }
}

void StreamBridge::push_text(const std::string& text) {
    emit_text(text, LLAMA_STREAM_DELTA_TEXT);
}

void StreamBridge::set_usage(int prompt_tokens, int completion_tokens) {
    if (prompt_tokens > 0) prompt_tokens_ = prompt_tokens;
    if (completion_tokens > 0) completion_tokens_ = completion_tokens;
}

void StreamBridge::set_finish_reason(const std::string& reason) {
    if (reason.empty()) return;
    finish_reason_ = reason;
    closed_ = true;
}

void StreamBridge::set_error(const std::string& error) {
    if (error.empty()) return;
    error_ = error;
    /* Ошибка — тоже закрытие потока: иначе done_json сообщил бы сначала про
     * обрыв, а текст ошибки потерялся бы. */
    closed_ = true;
}

void StreamBridge::feed(const char* chunk_json) {
    if (!chunk_json || !*chunk_json) return;

    json chunk;
    try {
        chunk = json::parse(chunk_json);
    } catch (const std::exception&) {
        // Повреждённый чанк посреди потока — это дырка в ответе. Молчать
        // нельзя (плагин сочтёт текст связным), ронять поток тоже нельзя
        // (остаток ответа ещё полезен), поэтому считаем и отдаём число.
        ++skipped_chunks_;
        return;
    }
    if (!chunk.is_object()) {
        ++skipped_chunks_;
        return;
    }

    if (chunk.contains("usage") && chunk["usage"].is_object()) {
        const json& u = chunk["usage"];
        set_usage(u.value("prompt_tokens", 0), u.value("completion_tokens", 0));
    }

    if (!chunk.contains("choices") || !chunk["choices"].is_array() ||
        chunk["choices"].empty()) {
        return;
    }
    const json& choice = chunk["choices"][0];
    if (!choice.is_object()) return;

    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
        set_finish_reason(choice["finish_reason"].get<std::string>());
    }

    if (!choice.contains("delta") || !choice["delta"].is_object()) return;
    const json& delta = choice["delta"];

    /* Порядок: текст, «размышление», аргументы вызова. Внутри чанка их
     * порядок не влияет на результат — каждый попадает в свой колбэк, —
     * но фиксируем его, чтобы поведение не «плавало» между провайдерами. */
    if (delta.contains("content") && delta["content"].is_string()) {
        emit_text(delta["content"].get<std::string>(), LLAMA_STREAM_DELTA_TEXT);
    }

    /* Размышление приходит двумя разными именами: llama-server кладёт его
     * в reasoning_content, OpenRouter — в reasoning. Склеивать их с текстом
     * нельзя (находка И5): тогда reasoning попал бы в ответ и в UI как
     * обычный текст, а в историю — как слова модели. */
    for (const char* key : {"reasoning_content", "reasoning"}) {
        if (delta.contains(key) && delta[key].is_string()) {
            emit_text(delta[key].get<std::string>(),
                      LLAMA_STREAM_DELTA_REASONING);
        }
    }

    if (!delta.contains("tool_calls") || !delta["tool_calls"].is_array()) return;
    for (const json& call : delta["tool_calls"]) {
        if (!call.is_object()) continue;
        const int index = call.value("index", 0);

        ToolCallSlot& slot = tool_calls_[index];
        if (slot.call_id.empty()) {
            /* Провайдер с идентификаторами отдаёт свой, остальные нумеруют
             * вызовы индексом — его и превращаем в call_N. */
            slot.call_id = (call.contains("id") && call["id"].is_string())
                               ? call["id"].get<std::string>()
                               : call_id_for_index(index);
        }
        if (call.contains("function") && call["function"].is_object()) {
            /* Имя приходит только в первом фрагменте вызова, а куски
             * аргументов идут следующими: без запоминания имени второй
             * фрагмент пришёл бы с пустым tool_name, и плагин не понял бы,
             * чьи это аргументы. */
            const std::string name = call["function"].value("name", "");
            if (!name.empty()) slot.name = name;
            if (call["function"].contains("arguments") &&
                call["function"]["arguments"].is_string()) {
                emit_tool_delta(
                    slot, call["function"]["arguments"].get<std::string>());
            }
        }
    }
}

std::string StreamBridge::done_json(const std::string& error) const {
    json out;
    const std::string err = error.empty() ? error_ : error;
    if (!err.empty()) {
        out["ok"] = 0;
        out["error"] = err;
        if (!content_.empty()) out["content"] = content_;
        return out.dump();
    }
    if (!closed_) {
        /* Поток оборвался до закрывающего finish_reason. Такой ответ
         * неполон: принять его за пустой итог хода — значит показать
         * пользователю «готово» там, где модель просто не дописала ответ. */
        out["ok"] = 0;
        out["error"] =
            "поток прерван до конца ответа: закрывающий finish_reason не получен";
        if (!content_.empty()) out["content"] = content_;
        return out.dump();
    }

    out["ok"] = 1;
    out["content"] = content_;
    out["finish_reason"] = finish_reason_.empty() ? "stop" : finish_reason_;
    out["prompt_tokens"] = prompt_tokens_;
    out["completion_tokens"] = completion_tokens_;
    if (skipped_chunks_ > 0) out["skipped_chunks"] = skipped_chunks_;
    return out.dump();
}

} // namespace plugin
} // namespace llama_gui

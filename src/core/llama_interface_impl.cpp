#include "../include/core/llama_interface_impl.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace llama_gui {
namespace core {

ToolsSupport classify_tools_response(long http_code, const std::string& body)
{
    const bool ok = (http_code >= 200 && http_code < 300);
    if (ok) return ToolsSupport::Supported;

    /* Главное правило функции: код 0 (не получили ответ вовсе) и любой код
     * вне списка ниже — это Unknown, а НЕ Rejected. Один сетевой сбой не
     * должен навсегда выключить нативный вызов: агент поехал бы по
     * текстовому протоколу и не сказал бы об этом. Поэтому маркеры
     * ищутся ТОЛЬКО в кодах, где сервер осмысленно отказал, и никогда —
     * в теле, которое пришло без кода. */
    if (http_code == 400 || http_code == 422 || http_code == 501) {
        /* Маркеры обязаны быть в нижнем регистре: тело приводится к нему
         * для сравнения, и маркер в исходном виде не нашёлся бы НИКОГДА —
         * то есть отказ сервера молча считался бы неизвестным, и tools
         * слались бы снова и снова. */
        static const char* kMarkers[] = {
            "tools param", "tool_choice", "tool_calls",
            "unsupported tool", "invalid tool", "tools are not", "no tools"
        };
        std::string lower = body;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        for (const char* marker : kMarkers) {
            if (lower.find(marker) != std::string::npos) return ToolsSupport::Rejected;
        }
    }
    return ToolsSupport::Unknown;
}

std::vector<ToolCall> parse_tool_calls(const json& array)
{
    std::vector<ToolCall> out;
    if (!array.is_array()) return out;
    for (const auto& c : array) {
        if (!c.is_object()) continue;
        ToolCall call;
        call.id = c.value("id", "");
        call.type = c.value("type", "function");
        if (c.contains("function") && c["function"].is_object()) {
            call.name = c["function"].value("name", "");
            /* arguments приходит строкой и может быть неполным — поэтому
             * как есть, без попытки разобрать. */
            call.arguments = c["function"].value("arguments", "");
        }
        if (call.name.empty()) continue;
        out.push_back(std::move(call));
    }
    return out;
}

json build_chat_body(const ChatCompletionRequest& request, bool stream)
{
    json body;
    body["model"] = request.model;
    body["stream"] = stream;
    body["max_tokens"] = request.max_tokens;
    body["temperature"] = request.temperature;
    body["top_p"] = request.top_p;

    json messages = json::array();
    for (const auto& msg : request.messages) {
        json m;
        switch (msg.role) {
            case MessageRole::User:      m["role"] = "user"; break;
            case MessageRole::Assistant: m["role"] = "assistant"; break;
            case MessageRole::System:    m["role"] = "system"; break;
        }
        m["content"] = msg.content;
        /* Вызовы едут в теле реплики ассистента: без них модель не видит,
         * что она уже вызывала инструмент, и на следующем шаге вызовет
         * его снова. */
        if (!msg.tool_calls.empty()) {
            json calls = json::array();
            for (const auto& call : msg.tool_calls) {
                json c;
                c["id"] = call.id;
                c["type"] = call.type.empty() ? "function" : call.type;
                c["function"]["name"] = call.name;
                c["function"]["arguments"] = call.arguments;
                calls.push_back(std::move(c));
            }
            m["tool_calls"] = std::move(calls);
        }
        messages.push_back(std::move(m));
    }
    body["messages"] = std::move(messages);

    /* Дальше — только то, что отличается от нейтрального. Пробел в
     * значении здесь не «мы не знаем», а «здесь нужны настройки сервера»,
     * и его ставят явно (см. объявление). */
    if (request.top_k != 0)          body["top_k"] = request.top_k;
    if (request.min_p != 0.0f)       body["min_p"] = request.min_p;
    if (request.repeat_penalty != 1.0f) body["repeat_penalty"] = request.repeat_penalty;
    if (request.presence_penalty != 0.0f)  body["presence_penalty"] = request.presence_penalty;
    if (request.frequency_penalty != 0.0f) body["frequency_penalty"] = request.frequency_penalty;
    if (request.mirostat_mode != 0) {
        body["mirostat"] = request.mirostat_mode;
        body["mirostat_tau"] = request.mirostat_tau;
        body["mirostat_eta"] = request.mirostat_eta;
    }
    if (!request.stop.empty()) body["stop"] = request.stop;
    if (!request.grammar.empty()) body["grammar"] = request.grammar;

    /* И6.2: поля уходят только когда их наполнили. Пустой tools означает
     * «сервер ничего не узнает», а не «сервер не умеет» — иначе каждый
     * запрос без инструментов рисковал бы получить 400. */
    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& t : request.tools) {
            json fn;
            fn["name"] = t.name;
            if (!t.description.empty()) fn["description"] = t.description;
            fn["parameters"] = t.parameters.is_null()
                ? json{{"type", "object"}, {"properties", json::object()}}
                : t.parameters;
            json entry;
            entry["type"] = t.type.empty() ? "function" : t.type;
            entry["function"] = std::move(fn);
            tools.push_back(std::move(entry));
        }
        body["tools"] = std::move(tools);
        if (!request.tool_choice.empty()) body["tool_choice"] = request.tool_choice;
    }
    if (!request.response_format.type.empty()) {
        if (request.response_format.schema.is_null()) {
            body["response_format"] = json{{"type", request.response_format.type}};
        } else {
            body["response_format"] = json{
                {"type", request.response_format.type},
                {"json_schema", request.response_format.schema}};
        }
    }
    return body;
}

} // namespace core
} // namespace llama_gui

namespace llama_gui {
namespace core {
namespace impl {

using llama_gui::core::build_chat_body;
using llama_gui::core::classify_tools_response;
using llama_gui::core::parse_tool_calls;

namespace {

/* Текст ошибки из тела ответа: у llama-server это {"error":{"message":...}},
 * у облака — {"error":"..."}. Оба приходят на 4xx/5xx, и раньше код
 * ответа отбрасывался целиком. */
std::string error_text_from_body(const std::string& body, long http_code)
{
    if (body.empty()) return "HTTP " + std::to_string(http_code);
    try {
        const json j = json::parse(body);
        if (j.contains("error")) {
            if (j["error"].is_string()) return j["error"].get<std::string>();
            if (j["error"].is_object() && j["error"].contains("message")) {
                return j["error"]["message"].get<std::string>();
            }
        }
    } catch (const std::exception&) {
        /* Не JSON — отдаём как есть, ограничив длину: тело на 500 может
         * быть простынёй, и в ошибку агента она не должна уходить целиком. */
    }
    return body.size() > 500 ? body.substr(0, 500) : body;
}

} // namespace

using json = nlohmann::json;

// SlotOperationResult is already defined in header

LlamaInterfaceImpl::LlamaInterfaceImpl(const std::string& server_url)
    : server_url_(server_url)
    , api_key_("")
    , timeout_seconds_(120)  // 120s for local CPU models
    , curl_handle_(nullptr)
    , curl_initialized_(false)
    , streaming_active_(false)
    , next_slot_id_(0)
{
    initialize_curl();
}

LlamaInterfaceImpl::~LlamaInterfaceImpl()
{
    cleanup_curl();
    stop_streaming_requests();

    if (async_processing_thread_.joinable()) {
        async_processing_thread_.join();
    }
}

bool impl::LlamaInterfaceImpl::initialize(const std::string& server_url)
{
    /* Смена адреса — смена сервера, а значит и его возможностей. Оставленный
     * прошлый вывод «tools не умеет» выключил бы нативный вызов у нового
     * сервера, который их умеет, и это молча уехало бы в текстовый
     * протокол. */
    if (server_url_ != server_url) {
        tools_support_.store(static_cast<int>(ToolsSupport::Unknown));
    }
    server_url_ = server_url;
    return is_server_healthy();
}

void impl::LlamaInterfaceImpl::remember_tools_support(ToolsSupport support)
{
    /* Unknown не запоминается намеренно: сетевой сбой или нечитаемый ответ
     * — это отсутствие данных, а не доказательство неумения. Запомнив его,
     * мы одним сбоем навсегда перевели бы агента на текстовый протокол. */
    if (support == ToolsSupport::Unknown) return;
    tools_support_.store(static_cast<int>(support));
}

void impl::LlamaInterfaceImpl::apply_ssl_options(CURL* curl) const
{
    // verify_ssl=false (локальный сценарий по умолчанию) — самоподписанные
    // сертификаты и https-прокси без CA не блокируют подключение.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, ssl_verify_ ? 2L : 0L);
}

bool impl::LlamaInterfaceImpl::is_server_healthy() const
{
    if (!curl_initialized_) {
        return false;
    }

    // Use a separate, short-lived handle for health checks to avoid blocking the UI
    CURL* health_handle = curl_easy_init();
    if (!health_handle) {
        return false;
    }

    std::string health_url = server_url_ + "/health";
    long http_code = 0;

    curl_easy_setopt(health_handle, CURLOPT_URL, health_url.c_str());
    curl_easy_setopt(health_handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(health_handle, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(health_handle, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(health_handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(health_handle, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(health_handle, CURLOPT_WRITEFUNCTION, +[](char*, size_t, size_t, void*) -> size_t { return 0; });
    apply_ssl_options(health_handle);

    CURLcode res = curl_easy_perform(health_handle);

    if (res == CURLE_OK) {
        curl_easy_getinfo(health_handle, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_easy_cleanup(health_handle);

    // Server is healthy only if it responds with 200 OK
    return (res == CURLE_OK && http_code == 200);
}

llama_gui::core::json llama_gui::core::impl::LlamaInterfaceImpl::get_server_info() const
{
    json info;
    info["server_url"] = server_url_;
    info["timeout"] = timeout_seconds_;
    info["api_key_set"] = !api_key_.empty();
    return info;
}

llama_gui::core::json llama_gui::core::impl::LlamaInterfaceImpl::get_models() const
{
    // TODO: Implement actual model listing
    json result;
    result["models"] = json::array();
    return result;
}

llama_gui::core::json llama_gui::core::impl::LlamaInterfaceImpl::get_slots_status() const
{
    // TODO: Implement actual slot status
    json result;
    result["slots"] = json::array();
    result["next_slot_id"] = next_slot_id_.load();
    return result;
}

void impl::LlamaInterfaceImpl::create_chat_completion_streaming(
    const ChatCompletionRequest& request,
    StreamCallback callback,
    StreamCancel* cancel)
{
    /* И6.2. Известный отказ в tools не отправляется вовсе: один пробный
     * запрос без них ушёл бы, а каждый следующий — уже знал бы. Неизвестное
     * состояние отправляет как есть: иначе сервер, умеющий tools, никогда
     * не был бы проверен. */
    ChatCompletionRequest effective = request;
    if (!request.tools.empty() &&
        tools_support_.load() == static_cast<int>(ToolsSupport::Rejected)) {
        effective.tools.clear();
        effective.tool_choice.clear();
    }

    std::string post_fields = build_chat_body(effective, true).dump();
    std::string url = server_url_ + "/v1/chat/completions";

    streaming_active_ = true;

    // Зарегистрируем поток, чтобы stop_streaming_requests мог его прервать
    auto stream_data = std::make_shared<StreamingData>();
    stream_data->callback = callback;
    stream_data->completed = false;
    {
        std::lock_guard<std::mutex> lock(streaming_mutex_);
        active_streams_.push_back(stream_data);
    }

    // Run streaming request in a separate thread to not block UI
    std::thread([this, url, post_fields, stream_data, callback, effective, cancel]() {
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "[LlamaInterface] Failed to init curl for streaming" << std::endl;
            callback("", true);
            streaming_active_ = false;
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields.c_str());
        // Streaming: no timeout at all - connection lives until [DONE] or server closes it
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        apply_ssl_options(curl);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!api_key_.empty()) {
            std::string auth = "Authorization: Bearer " + api_key_;
            headers = curl_slist_append(headers, auth.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Streaming write callback: parse SSE lines and call the StreamCallback
        struct StreamContext {
            StreamCallback callback;
            std::string line_buffer;
            std::shared_ptr<StreamingData> stream_data;
            /* И6.2: сколько чанков реально дошло. Ноль означает, что ответ
             * не начался, и потому отказ сервера можно отличить от потока,
             * который уже идёт, и повторить запрос без tools. */
            int chunks_delivered = 0;
            /* Тело ответа при отказе: код 400 приходит с JSON, а не с SSE,
             * и без него причина отказа не читается. */
            std::string raw_body;
            /* И6.6: отмена по требованию. Живой указатель: он действует, пока
             * идёт поток, и владелец (плагин) держит флаг дольше вызова. */
            StreamCancel* cancel = nullptr;
        };

        StreamContext ctx;
        ctx.callback = callback;
        ctx.stream_data = stream_data;
        ctx.cancel = cancel;

        // Progress-callback прерывает запрос ещё на этапе соединения (до данных)
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
            +[](void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                auto* sc = static_cast<StreamContext*>(clientp);
                if (sc == nullptr) return 0;
                if (sc->stream_data->completed.load()) {
                    return 1; // Немедленно прервать transfer
                }
                /* И6.6: отмена прерывает и попытку соединения — иначе поток,
                 * который ещё не начал передаваться, дождался бы её начала,
                 * и пользователь увидел бы «Стоп не сработал». */
                if (sc->cancel && sc->cancel->requested()) return 1;
                return 0;
            });
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                size_t total = size * nmemb;
                auto* sc = static_cast<StreamContext*>(userdata);

                // Если запрос остановлен пользователем ИЛИ пришла отмена по
                // требованию — прерываем передачу, чтобы сервер перестал
                // генерировать. Возврат значения, отличного от total,
                // заставляет curl завершить transfer, и llama-server теряет
                // клиента и освобождает слот. Без этого «Стоп» останавливал
                // бы только чтение, а генерация продолжала жечь токены.
                if (sc->stream_data->completed.load() ||
                    (sc->cancel && sc->cancel->requested())) {
                    return 0;
                }

                sc->line_buffer.append(ptr, total);
                /* Копия тела держится только до конца запроса и нужна лишь
                 * для чтения причины отказа, поэтому ограничена: на 500
                 * сервер может прислать простыню. */
                if (sc->raw_body.size() < 4096) sc->raw_body.append(ptr, total);

                // Process complete SSE lines
                while (true) {
                    size_t pos = sc->line_buffer.find('\n');
                    if (pos == std::string::npos) break;

                    std::string line = sc->line_buffer.substr(0, pos);
                    sc->line_buffer.erase(0, pos + 1);

                    // Trim carriage return
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }

                    // Skip empty lines and non-data lines
                    if (line.empty()) continue;
                    if (line.find("data: ") != 0) continue;

                    std::string data = line.substr(6);
                    if (data == "[DONE]") {
                        return total;
                    }

                    // Pass the raw JSON data to the callback
                    // (the caller's callback expects to parse it itself)
                    sc->callback(data, false);
                    ++sc->chunks_delivered;

                    // Повторно проверяем флаг остановки после обработки чанка
                    if (sc->stream_data->completed.load()) {
                        return 0;
                    }
                }
                return total;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
            std::cerr << "[LlamaInterface] Streaming curl error: "
                      << curl_easy_strerror(res) << std::endl;
        }

        /* И6.2: отказ именно из-за tools и ни одного доставленного чанка —
         * повторяем запрос без них и запоминаем. Повтор делается здесь, а не
         * отдельным запросом-зондом: зонд стоил бы токены на ровно том же
         * сервере, который и так должен ответить. Ограничение «ни одного
         * чанка» не формальность: начатый поток отказом быть не может, а
         * повтор после доставки продублировал бы текст пользователю. */
        const ToolsSupport support = classify_tools_response(http_code, ctx.raw_body);
        if (support == ToolsSupport::Rejected && ctx.chunks_delivered == 0 &&
            !effective.tools.empty() &&
            tools_support_.load() != static_cast<int>(ToolsSupport::Rejected)) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            /* Снимаем регистрацию до повтора: иначе список потоков рос бы на
             * каждом отказе, и следующая остановка гасила бы вчерашний. */
            {
                std::lock_guard<std::mutex> lock(streaming_mutex_);
                for (auto it = active_streams_.begin(); it != active_streams_.end(); ++it) {
                    if (it->get() == stream_data.get()) {
                        active_streams_.erase(it);
                        break;
                    }
                }
            }
            streaming_active_ = false;
            std::cerr << "[LlamaInterface] Server rejected tools ("
                      << error_text_from_body(ctx.raw_body, http_code)
                      << ") - retrying without them" << std::endl;
            create_chat_completion_streaming(effective, callback);
            return;
        }
        if (!effective.tools.empty()) remember_tools_support(support);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        // Снимаем регистрацию потока
        {
            std::lock_guard<std::mutex> lock(streaming_mutex_);
            for (auto it = active_streams_.begin(); it != active_streams_.end(); ++it) {
                if (it->get() == stream_data.get()) {
                    active_streams_.erase(it);
                    break;
                }
            }
        }

        /* Отказ сервера виден в логе, а не выглядит пустым ответом: мост
         * отличит его от короткого ответа по отсутствию закрывающего
         * finish_reason, но текст причины нужен и ему. */
        if (http_code != 0 && (http_code < 200 || http_code >= 300)) {
            std::cerr << "[LlamaInterface] Streaming HTTP " << http_code << ": "
                      << error_text_from_body(ctx.raw_body, http_code) << std::endl;
        }

        // Signal completion (is_final). Если остановлено пользователем,
        // ChatInterface сам добавит частичный ответ — здесь не дублируем.
        callback("", true);
        streaming_active_ = false;
    }).detach();
}

std::future<ChatCompletionResponse> impl::LlamaInterfaceImpl::create_chat_completion_async(
    const ChatCompletionRequest& request)
{
    auto promise = std::make_shared<std::promise<ChatCompletionResponse>>();

    /* И6.2: тот же отказ, что и в потоковом пути, и то же решение — убрать
     * tools и повторить один раз, а не падать и не «успешно» вернуть
     * пустой ответ. */
    ChatCompletionRequest effective = request;
    if (!request.tools.empty() &&
        tools_support_.load() == static_cast<int>(ToolsSupport::Rejected)) {
        effective.tools.clear();
        effective.tool_choice.clear();
    }

    std::string post_fields = build_chat_body(effective, false).dump();
    std::string url = server_url_ + "/v1/chat/completions";

    std::thread([this, url, post_fields, promise, effective]() {
        CURL* curl = curl_easy_init();
        ChatCompletionResponse response;

        if (!curl) {
            response.ok = false;
            response.error = "не удалось создать curl-соединение";
            promise->set_value(response);
            return;
        }

        std::string response_body;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_seconds_);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        apply_ssl_options(curl);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!api_key_.empty()) {
            headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key_).c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                auto* body = static_cast<std::string*>(userdata);
                body->append(ptr, size * nmemb);
                return size * nmemb;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        response.http_code = http_code;

        if (!effective.tools.empty()) {
            const ToolsSupport support = classify_tools_response(http_code, response_body);
            if (support == ToolsSupport::Rejected &&
                tools_support_.load() != static_cast<int>(ToolsSupport::Rejected)) {
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                std::cerr << "[LlamaInterface] Server rejected tools ("
                          << error_text_from_body(response_body, http_code)
                          << ") - retrying without them" << std::endl;
                /* Повтор уходит отдельным запросом, а не циклом: вложенность
                 * дала бы вторую попытку с tools, а она уже доказала своё. */
                promise->set_value(create_chat_completion_async(effective).get());
                return;
            }
            remember_tools_support(support);
        }

        const bool http_ok = (http_code >= 200 && http_code < 300);

        if (res == CURLE_OK && http_ok) {
            try {
                auto j = nlohmann::json::parse(response_body);
                if (j.contains("choices") && !j["choices"].empty()) {
                    auto& choice = j["choices"][0];
                    response.id = j.value("id", "");
                    response.model = j.value("model", "");
                    if (choice.contains("message")) {
                        response.choices.push_back({});
                        response.choices[0].message.content = choice["message"].value("content", "");
                        response.choices[0].message.role = MessageRole::Assistant;
                        /* И6.2: нативный вызов. Пока его не читать, ход с
                         * вызовом выглядел бы пустым ответом, и цикл закрыл
                         * бы задачу, не выполнив инструмент. */
                        if (choice["message"].contains("tool_calls")) {
                            response.choices[0].message.tool_calls =
                                parse_tool_calls(choice["message"]["tool_calls"]);
                        }
                        response.choices[0].finish_reason = choice.value("finish_reason", "stop");
                    }
                }
                /* usage: {"prompt_tokens":N,"completion_tokens":N} — для честных метрик. */
                if (j.contains("usage") && j["usage"].is_object()) {
                    response.usage = j["usage"];
                }
            } catch (const std::exception& e) {
                std::cerr << "[LlamaInterface] JSON parse error: " << e.what() << std::endl;
                response.ok = false;
                response.error = std::string("не удалось разобрать ответ сервера: ") + e.what();
            }
        } else {
            /* И6.2: отказ сервера больше не выглядит как пустой ответ. Раньше
             * здесь ставился finish_reason="error" при пустом content, и
             * вызывающий не мог отличить «сервер отказал» от «модель молчит»
             * — а на этом выводе строилась бы неверная догадка про tools. */
            if (res != CURLE_OK) {
                response.error = std::string("ошибка соединения: ") + curl_easy_strerror(res);
                std::cerr << "[LlamaInterface] Async request failed: "
                          << curl_easy_strerror(res) << std::endl;
            } else {
                response.error = error_text_from_body(response_body, http_code);
                std::cerr << "[LlamaInterface] Async HTTP " << http_code << ": "
                          << response.error << std::endl;
            }
            response.ok = false;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        promise->set_value(response);
    }).detach();

    return promise->get_future();
}

EmbeddingResponse impl::LlamaInterfaceImpl::create_embedding(const EmbeddingRequest& request)
{
    // TODO: Implement embedding generation
    std::cerr << "Embedding generation not yet implemented" << std::endl;

    EmbeddingResponse response;
    response.object = "embedding";
    response.data = {llama_gui::core::EmbeddingResponse::EmbeddingData{}};
    return response;
}

bool impl::LlamaInterfaceImpl::save_slot_kv_cache(int slot_id, const std::string& filename)
{
    return save_slot_kv_cache_detailed(slot_id, filename).success;
}

bool impl::LlamaInterfaceImpl::restore_slot_kv_cache(int slot_id, const std::string& filename)
{
    return restore_slot_kv_cache_detailed(slot_id, filename).success;
}

impl::SlotOperationResult impl::LlamaInterfaceImpl::save_slot_kv_cache_detailed(
    int slot_id, const std::string& filename)
{
    impl::SlotOperationResult result;
    result.slot_id = slot_id;
    result.filename = filename;
    
    try {
        if (!save_kv_cache_file(slot_id, filename)) {
            result.success = false;
            result.error_message = "Failed to save KV-cache file";
            return result;
        }
        
        result.success = true;
        result.n_bytes = std::filesystem::file_size(filename);
        result.processing_ms = 0.0; // TODO: Measure actual time
        return result;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
        return result;
    }
}

impl::SlotOperationResult impl::LlamaInterfaceImpl::restore_slot_kv_cache_detailed(
    int slot_id, const std::string& filename)
{
    impl::SlotOperationResult result;
    result.slot_id = slot_id;
    result.filename = filename;
    
    try {
        if (!load_kv_cache_file(slot_id, filename)) {
            result.success = false;
            result.error_message = "Failed to load KV-cache file";
            return result;
        }
        
        result.success = true;
        result.n_bytes = std::filesystem::file_size(filename);
        result.processing_ms = 0.0; // TODO: Measure actual time
        return result;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
        return result;
    }
}

bool impl::LlamaInterfaceImpl::reset_slot(int slot_id)
{
    return reset_slot_impl(slot_id);
}

bool impl::LlamaInterfaceImpl::erase_slot(int slot_id)
{
    return erase_slot_impl(slot_id);
}

impl::SlotOperationResult impl::LlamaInterfaceImpl::tokenize_text_in_slot(
    int slot_id, const std::string& text)
{
    return tokenize_text_impl(slot_id, text);
}

void impl::LlamaInterfaceImpl::make_async_http_request(
    const std::string& endpoint,
    const std::string& method,
    const json& data,
    HttpResponseCallback callback)
{
    // TODO: Implement async HTTP request
    std::cerr << "Async HTTP request not yet implemented" << std::endl;
    
    if (callback) {
        callback("");
    }
}

void impl::LlamaInterfaceImpl::make_streaming_http_request(
    const std::string& endpoint,
    const std::string& method,
    const json& data,
    StreamCallback callback)
{
    // TODO: Implement streaming HTTP request
    std::cerr << "Streaming HTTP request not yet implemented" << std::endl;
}

void impl::LlamaInterfaceImpl::stop_streaming_requests()
{
    streaming_active_ = false;
    
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    for (auto& stream : active_streams_) {
        stream->completed = true;
    }
    active_streams_.clear();
}

void impl::LlamaInterfaceImpl::create_chat_completion_async_callback(
    const ChatCompletionRequest& request,
    ChatCompletionCallback callback)
{
    // TODO: Implement async callback version
    // For now, use the async version
    auto future = create_chat_completion_async(request);

    std::thread([future = std::move(future), callback]() mutable {
        try {
            auto response = future.get();
            callback(response);
        } catch (const std::exception& e) {
            ChatCompletionResponse error_response;
            error_response.choices.push_back({});
            error_response.choices[0].finish_reason = "error";
            error_response.choices[0].message.content = e.what();
            callback(error_response);
        }
    }).detach();
}

std::string llama_gui::core::impl::LlamaInterfaceImpl::make_http_request(
    const std::string& endpoint,
    const std::string& method,
    const json& data) const
{
    // TODO: Implement HTTP request
    std::cerr << "HTTP request not yet implemented" << std::endl;
    return "";
}

bool impl::LlamaInterfaceImpl::parse_streaming_response(
    const std::string& response,
    StreamCallback callback) const
{
    // TODO: Parse streaming response
    return false;
}

void impl::LlamaInterfaceImpl::process_async_requests()
{
    // TODO: Implement async request processing
}

void impl::LlamaInterfaceImpl::initialize_curl()
{
    if (curl_initialized_) {
        return;
    }

    curl_handle_ = curl_easy_init();
    if (!curl_handle_) {
        std::cerr << "Failed to initialize CURL" << std::endl;
        return;
    }
    
    // Set default options
    curl_easy_setopt(curl_handle_, CURLOPT_TIMEOUT, timeout_seconds_);
    curl_easy_setopt(curl_handle_, CURLOPT_NOSIGNAL, 1L);
    apply_ssl_options(curl_handle_);
    if (!api_key_.empty()) {
        curl_easy_setopt(curl_handle_, CURLOPT_HTTPHEADER, 
            curl_slist_append(nullptr, 
                std::string("Authorization: Bearer " + api_key_).c_str()));
    }
    
    curl_initialized_ = true;
}

void impl::LlamaInterfaceImpl::cleanup_curl()
{
    if (curl_handle_) {
        curl_easy_cleanup(curl_handle_);
        curl_handle_ = nullptr;
    }
    curl_initialized_ = false;
}

bool impl::LlamaInterfaceImpl::save_kv_cache_file(int slot_id, const std::string& filename)
{
    try {
        std::string kv_cache_dir = get_kv_cache_dir();
        if (!fs::exists(kv_cache_dir)) {
            fs::create_directories(kv_cache_dir);
        }

        std::string filepath = kv_cache_dir + "/" + filename;

        // TODO: Actually save KV-cache data
        std::ofstream ofs(filepath, std::ios::binary);
        if (!ofs) {
            return false;
        }

        // Write placeholder data
        ofs.write("KV-CACHE PLACEHOLDER", 18);

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving KV-cache file: " << e.what() << std::endl;
        return false;
    }
}

bool impl::LlamaInterfaceImpl::load_kv_cache_file(int slot_id, const std::string& filename)
{
    try {
        std::string kv_cache_dir = get_kv_cache_dir();
        std::string filepath = kv_cache_dir + "/" + filename;

        if (!fs::exists(filepath)) {
            return false;
        }

        // TODO: Actually load KV-cache data
        std::ifstream ifs(filepath, std::ios::binary);
        if (!ifs) {
            return false;
        }

        // Read placeholder data
        char buffer[20];
        ifs.read(buffer, sizeof(buffer));

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error loading KV-cache file: " << e.what() << std::endl;
        return false;
    }
}

std::string llama_gui::core::impl::LlamaInterfaceImpl::get_kv_cache_dir() const
{
    // TODO: Make this configurable
    return "./kv_cache";
}

bool impl::LlamaInterfaceImpl::reset_slot_impl(int slot_id)
{
    // TODO: Implement slot reset
    return true;
}

bool impl::LlamaInterfaceImpl::erase_slot_impl(int slot_id)
{
    // TODO: Implement slot erase
    return true;
}

impl::SlotOperationResult impl::LlamaInterfaceImpl::tokenize_text_impl(int slot_id, const std::string& text)
{
    SlotOperationResult result;
    result.slot_id = slot_id;

    try {
        // TODO: Implement actual tokenization
        result.n_tokens = static_cast<int>(text.size() / 4); // Placeholder
        result.n_bytes = text.size();
        result.success = true;
        return result;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
        return result;
    }
}

} // namespace impl
} // namespace core
} // namespace llama_gui

// ============================================================================
// Static utility methods
// ============================================================================

std::string llama_gui::core::impl::LlamaInterfaceImpl::validate_and_clean_utf8(const std::string& input)
{
    // TODO: Implement UTF-8 validation and cleaning
    return input;
}

llama_gui::core::json llama_gui::core::impl::LlamaInterfaceImpl::extract_json_from_response(const std::string& response)
{
    // TODO: Implement JSON extraction
    try {
        return llama_gui::core::json::parse(response);
    } catch (const llama_gui::core::json::exception& e) {
        return llama_gui::core::json::object();
    }
}

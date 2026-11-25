#pragma once

/*
 * plugin_api.h — C-интерфейс (SDK) системы плагинов llama-gui.
 *
 * Плагины компилируются как разделяемые библиотеки (.so/.dll/.dylib)
 * и разрабатываются ПОЛНОСТЬЮ независимо от кода приложения — они
 * подключают только этот заголовок.
 *
 * При загрузке хост вызывает ll_plugin_init(host, api) и передаёт
 * таблицу функций LlamaHostApi. Через неё плагин получает доступ к
 * возможностям приложения: меню, командам, окнам, диалогам, настройкам,
 * состоянию, чату, LLM и RAG.
 *
 * Каждый кадр приложение вызывает ll_plugin_render(), в котором плагин
 * может рисовать свои окна через Dear ImGui (заголовки imgui.h приложение
 * предоставляет отдельно, символы резолвятся из исполняемого файла).
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Версия API. Плагин обязан совпадать с хостом по мажорной/минорной. */
#define LLAMA_PLUGIN_API_VERSION_MAJOR 1
#define LLAMA_PLUGIN_API_VERSION_MINOR 0
#define LLAMA_PLUGIN_API_VERSION_PATCH 0
#define LLAMA_PLUGIN_API_VERSION "1.0.0"

/* Экспорт функций из плагина */
#if defined(_WIN32) || defined(_WIN64)
    #define LLAMA_PLUGIN_EXPORT __declspec(dllexport)
#else
    #define LLAMA_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* Непрозрачные хендлы */
typedef struct LlamaPluginHost LlamaPluginHost;       /* идентичность плагина в хосте */
typedef struct LlamaPlugin LlamaPlugin;
typedef struct LlamaPluginMenu LlamaPluginMenu;       /* хендл добавленного меню */
typedef struct LlamaPluginCommand LlamaPluginCommand; /* хендл зарегистрированной команды */
typedef struct LlamaPluginWindow LlamaPluginWindow;   /* хендл зарегистрированного окна */
typedef struct LlamaPluginAgentMode LlamaPluginAgentMode; /* хендл зарегистрированного режима агента */

/* Уровни логирования */
enum {
    LLAMA_LOG_DEBUG = 0,
    LLAMA_LOG_INFO = 1,
    LLAMA_LOG_WARNING = 2,
    LLAMA_LOG_ERROR = 3
};

/* Колбэк плагина (команды, подтверждения диалогов) */
typedef void (*LlamaPluginCallback)(void* user_data);

/* Вид дельты в llm_chat_stream: текст ответа или «размышление» модели.
 * Разведены потому, что размышление не должно попадать в текст ответа:
 * именно их склейка была одной из находок И5 (reasoning_content и content
 * приходят разными полями, и провайдеры их смешивают по-разному). */
enum {
    LLAMA_STREAM_DELTA_TEXT = 0,
    LLAMA_STREAM_DELTA_REASONING = 1
};

typedef struct LlamaHostApi LlamaHostApi;

/*
 * Режим агента плагина. Плагин регистрирует режим, и он появляется
 * в селекторе режимов основного чата. Когда режим активен, сообщения
 * пользователя перенаправляются в on_message вместо LLM.
 */
typedef struct LlamaPluginAgentMode {
    const char* name;           /* уникальное имя [a-z0-9_-] */
    const char* display_name;   /* отображаемое имя в UI */
    /*
     * Обработчик сообщений пользователя.
     * Возвращает текст-ответ (выделяется хостом, освобождается через free_string).
     * Может вызываться из фонового потока — не рисовать ImGui внутри.
     */
    char* (*on_message)(LlamaPluginHost* host, const char* user_message, void* user_data);
    /*
     * То же, но с вложениями (мультимодальность).
     *
     * Зачем второй обработчик, а не расширение on_message: on_message —
     * СИГНАТУРА, и её изменение сломало бы все уже собранные плагины. Все
     * остальные расширения ABI в этом файле сделаны добавлением поля в
     * КОНЕЦ структуры, и здесь то же правило: старый плагин оставляет
     * указатель нулевым (структура инициализируется `= {}`), и хост
     * зовёт on_message, как звал.
     *
     * attachments_json — JSON-массив OpenAI-формата:
     *   [{"type":"image_url","image_url":{"url":"data:image/png;base64,…"}}]
     * или nullptr, если вложений нет. Хост зовёт on_message_ex, когда
     * указатель непуст; тот же контракт, что у llm_chat_stream (проверка
     * наличия ДО вызова), и по той же причине: вызов несуществующего поля
     * на старом хосте — это вызов по мусорному указателю.
     *
     * Возвращает то же, что on_message: malloc'd строку, освобождается
     * хостом через free_string().
     */
    char* (*on_message_ex)(LlamaPluginHost* host, const char* user_message,
                           const char* attachments_json, void* user_data);
    /*
     * Рендер дополнительного UI под лентой сообщений.
     * Вызывается каждый кадр из render() чата когда режим активен.
     * Рисовать ImGui внутри.
     */
    void (*render_extras)(LlamaPluginHost* host, void* user_data);
    void* user_data;
} LlamaPluginAgentMode;

/*
 * Таблица функций хоста. Плагин получает её в ll_plugin_init и может
 * сохранить указатель для использования в своих колбэках.
 *
 * Все строки, возвращаемые как char* (settings_get, state_get, rag_search,
 * rag_build_prompt, llm_complete) выделяются хостом и должны быть освобождены
 * плагином через free_string(). Массивы float — через free_float_array().
 */
struct LlamaHostApi {
    /* Размер структуры — для обратной совместимости. */
    uint32_t size;
    /* Версия приложения-хоста. */
    const char* app_version;

    /* --- Логирование --- */
    void (*log)(LlamaPluginHost* host, int level, const char* message);

    /* --- Меню --- */
    LlamaPluginMenu* (*menu_add)(LlamaPluginHost* host, const char* menu_name);
    void (*menu_add_item)(LlamaPluginHost* host, LlamaPluginMenu* menu,
                          const char* item_name, const char* command_name,
                          const char* shortcut);
    void (*menu_add_separator)(LlamaPluginHost* host, LlamaPluginMenu* menu);

    /* --- Команды и горячие клавиши --- */
    LlamaPluginCommand* (*command_register)(LlamaPluginHost* host,
                                            const char* name,
                                            LlamaPluginCallback callback,
                                            void* user_data,
                                            const char* description,
                                            const char* shortcut);
    int (*command_execute)(LlamaPluginHost* host, const char* name);

    /* --- Окна (видимость управляется WindowManager'ом приложения) --- */
    LlamaPluginWindow* (*window_register)(LlamaPluginHost* host,
                                          const char* wm_name,
                                          const char* imgui_title);
    void (*window_set_visible)(LlamaPluginHost* host, LlamaPluginWindow* window,
                               int visible);
    int  (*window_is_visible)(LlamaPluginHost* host, LlamaPluginWindow* window);

    /* --- Диалоги --- */
    void (*dialog_info)(LlamaPluginHost* host, const char* title, const char* message);
    void (*dialog_warning)(LlamaPluginHost* host, const char* title, const char* message);
    void (*dialog_error)(LlamaPluginHost* host, const char* title, const char* message);
    void (*dialog_confirmation)(LlamaPluginHost* host, const char* title,
                                const char* message,
                                LlamaPluginCallback callback, void* user_data);

    /* --- Настройки (persist в settings.ini, значения — JSON-строки) --- */
    char* (*settings_get)(LlamaPluginHost* host, const char* key);
    int   (*settings_set)(LlamaPluginHost* host, const char* key, const char* json_value);

    /* --- Состояние (in-memory хранилище плагинов) --- */
    char* (*state_get)(LlamaPluginHost* host, const char* key);
    int   (*state_set)(LlamaPluginHost* host, const char* key, const char* json_value);

    /* --- Чат / LLM --- */
    int (*chat_send_message)(LlamaPluginHost* host, const char* message);
    int (*chat_add_message)(LlamaPluginHost* host, const char* role, const char* content);
    int (*llm_is_connected)(LlamaPluginHost* host);
    int (*llm_complete)(LlamaPluginHost* host, const char* prompt, char** out_response);

    /* --- RAG --- */
    /* rag_search возвращает JSON-массив чанков: [{content, document_id,
       chunk_index, file_path, symbol_name, start_line, end_line}, ...] */
    char* (*rag_search)(LlamaPluginHost* host, const char* query, int k,
                        const char* path_filter);
    int   (*rag_process_document)(LlamaPluginHost* host, const char* path);
    int   (*rag_embedding)(LlamaPluginHost* host, const char* text,
                           float* out_vec, int max_dim, int* out_dim);
    int   (*rag_index_count)(LlamaPluginHost* host);
    char* (*rag_build_prompt)(LlamaPluginHost* host, const char* query,
                              int k, const char* path_filter);

    /* --- Пути --- */
    const char* (*path_config_dir)(LlamaPluginHost* host);
    const char* (*path_data_dir)(LlamaPluginHost* host);
    const char* (*path_plugins_dir)(LlamaPluginHost* host);

    /* --- Освобождение памяти хоста --- */
    void (*free_string)(LlamaPluginHost* host, char* str);
    void (*free_float_array)(LlamaPluginHost* host, float* arr);

    /* llm_complete_ex — как llm_complete, но с явным системным промптом (ролью).
       Позволяет плагину отправлять статичные инструкции/роль РОВНО ОДИН раз, а
       затем для каждого элемента данных слать лишь пользовательское сообщение.
       Это не перегружает модель одинаковыми инструкциями при обходе списка
       (облако кэширует префикс системного промпта, локальный сервер — KV-префикс).
       system_prompt может быть nullptr/пустым — тогда поведение == llm_complete.
       Поле добавлено в КОНЕЦ структуры, чтобы не смещать существующие указатели
       (обратная совместимость по offsetof при старом хосте). */
    int (*llm_complete_ex)(LlamaPluginHost* host, const char* system_prompt,
                           const char* user_prompt, char** out_response);

    /* --- Режимы агентов (плагин-интеграция в основной чат) --- */
    /* Регистрация режима агента. Возвращает хендл или NULL при ошибке.
       Режим появляется в селекторе режимов чата. */
    LlamaPluginAgentMode* (*agent_mode_register)(LlamaPluginHost* host,
                                                 const LlamaPluginAgentMode* mode);
    /* Удаление режима. */
    void (*agent_mode_unregister)(LlamaPluginHost* host, LlamaPluginAgentMode* mode);
    /* Отправка события из плагина в чат (отображается как сообщение ассистента). */
    void (*agent_mode_push_event)(LlamaPluginHost* host, const char* event_text);

    /*
     * Multi-turn chat completion. Принимает системный промпт и историю диалога
     * в виде JSON-массива: [{"role":"system"|"user"|"assistant","content":"..."}, ...].
     * Возвращает malloc'd JSON: {"ok":1,"content":"...","finish_reason":"stop",
     * "prompt_tokens":N,"completion_tokens":N} или nullptr при ошибке.
     * Поля добавлены в КОНЕЦ структуры — обратная совместимость по offsetof.
     */
    char* (*llm_chat_messages)(LlamaPluginHost* host, const char* system_prompt,
                               const char* messages_json);

    /* --- Стриминг ответа LLM ---
     *
     * Поле добавлено в КОНЕЦ структуры (append-only): плагин проверяет его
     * наличие через offsetof(LlamaHostApi, llm_chat_stream) ДО вызова, и
     * поле, вставленное в середину, сдвинуло бы всё после себя — старый .so
     * читал бы вместо указателя мусор.
     *
     * llm_chat_messages отдаёт ответ целиком, поэтому пользователь видит
     * токены только после конца генерации: для агента на несколько минут
     * это выглядит как зависание. Здесь дельты приходят по мере поступления.
     *
     * system_prompt — тот же явный системный промпт, что у llm_chat_messages.
     * messages_json — тот же JSON-массив [{"role":...,"content":...}, ...].
     * request_json — параметры генерации; любые поля можно не указывать,
     *   {"max_tokens":N,"temperature":x,"top_p":x,"stop":["..."]}.
     *   max_tokens <= 0 или отсутствие поля означает «как у llm_chat_messages».
     *
     * on_delta приходит на каждый кусок текста, on_tool_delta — на кусок
     * аргументов вызова инструмента (native tool calling). Оба вызываются
     * из потока хоста, последовательно и до on_done.
     *
     * on_done вызывается РОВНО ОДИН РАЗ, когда поток закрылся — успехом
     * или ошибкой; result_json имеет ту же раскладку, что и ответ
     * llm_chat_messages: {"ok":1,"content":"...","finish_reason":"...",
     * "prompt_tokens":N,"completion_tokens":N} либо {"ok":0,"error":"..."}.
     * ok=0 означает, что ответ НЕ доведён: плагин обязан показать ошибку, а
     * не считать пустой ответ итогом хода.
     *
     * Возвращает 1, если поток запущен (тогда on_done будет вызван), и 0,
     * если вызов отклонён на аргументах (тогда on_done НЕ вызывается).
     *
     * ВНИМАНИЕ, время жизни: колбэки приходят из потока хоста, поэтому
     * после возврата 1 они будут звать код плагина ещё какое-то время.
     * Плагин обязан дождаться on_done ДО возврата из ll_plugin_shutdown:
     * хост выгружает библиотеку сразу после вызова, и колбэк в
     * выгруженный код — это вызов по освобождённой памяти без единого
     * сообщения об ошибке.
     */
    int (*llm_chat_stream)(LlamaPluginHost* host, const char* system_prompt,
                           const char* messages_json, const char* request_json,
                           void* user_data,
                           void (*on_delta)(void* user_data, const char* text, int kind),
                           void (*on_tool_delta)(void* user_data, const char* call_id,
                                                 const char* tool_name,
                                                 const char* json_fragment),
                           void (*on_done)(void* user_data, const char* result_json));

    /*
     * Символы одного файла через tree-sitter (хост уже собрал грамматики,
     * плагину они недоступны — статическая библиотека без зависимости от
     * хоста). Поле добавлено в КОНЕЦ структуры; проверка — через offsetof,
     * как у llm_chat_stream.
     *
     * path — путь к файлу. language может быть пустым: тогда ход определяется
     * по расширению. language != "" приоритетнее: файл без расширения
     * (stdin-подобный) разбирается им.
     *
     * Возвращает malloc'd JSON (освободить через free_string):
     *   {"ok":1,"ast":true,"language":"cpp","symbols":[
     *       {"kind":"function","name":"foo","parent":"","line":10,"end_line":42}]}
     *   {"ok":1,"ast":false,"language":"php","symbols":[],"reason":"..."}
     *   {"ok":0,"error":"..."}
     *
     * ast:false — грамматики для языка нет (в llama-gui web-грамматики
     * слабые и могут не быть собраны). Это НЕ ошибка и НЕ пустой список:
     * поле ast обязано быть проверено, иначе вызывающий решит, что файл
     * разобран и символов нет, и молча потеряет обзор проекта. Именно так
     * деградация до текста и выглядит снаружи — молча.
     *
     * Список файлов не принимается: обход дерева — дело вызывающего
     * (у него свои правила пропуска каталогов и лимиты), и второй обход
     * внутри хоста разошёлся бы с первым при первом же новом каталоге.
     */
    char* (*llm_ast_symbols)(LlamaPluginHost* host, const char* path,
                             const char* language);

    /* Отмена одного потока llm_chat_stream (И6.6).
     *
     * handle — ТОТ ЖЕ user_data, который плагин передал в llm_chat_stream.
     * Это ключ, а не новый непрозрачный тип: второй источник идентификаторов
     * разошёлся бы с первым, и отмена отменяла бы не тот поток.
     *
     * Отмена прерывает HTTP-передачу этого потока; локальный сервер теряет
     * клиента и освобождает слот генерации. Без этого поля «Стоп» обрывал
     * только ожидание плагина, а модель продолжала печатать впустую и жечь
     * токены — и пользователь платил за генерацию, которую никто не видел.
     *
     * Гарантии: возврат из llm_chat_stream означает, что поток запущен.
     * Если handle не соответствует живому потоку (поток уже закрылся, или
     * плагин передал чужой user_data) — ничего не происходит, и это НЕ
     * ошибка: гонка «отмена пришла после закрытия» нормальна. Ошибка
     * потока приходит через llm_chat_stream, а отмена её не подменяет.
     *
     * Поле в конце структуры; проверка — offsetof, как у остальных.
     */
    void (*llm_chat_cancel)(LlamaPluginHost* host, void* handle);

    /*
     * --- Справочник моделей (models.dev) ---
     *
     * Плагин не знает, какая модель реально обслуживает запрос: через ABI её
     * не спросить. Из-за этого лимиты берутся из ручных настроек
     * (`wp_coder.context_limit`), цены — из `wp_coder.price_input_per_1m`, а
     * поддержка инструментов определяется пробой, общей на все модели.
     *
     * model_info — проекция каталога для конкретной модели. model_id — id
     * модели (для облака — как в каталоге/провайдере, для локальной — имя
     * файла GGUF). Возвращает malloc'd JSON (освободить free_string):
     *   {"ok":1,"found":true,"source":"exact"|"reference"|"local"|"unknown",
     *    "provider":"alibaba","model_id":"...","canonical_model_id":"...",
     *    "family":"...","tools_supported":true,"reasoning":false,
     *    "context":N,"max_output":N,
     *    "price_in":x,"price_out":x,"cache_read":x,"cache_write":x}
     *
     * source: "exact" — найдено у провайдера с тем же endpoint; "reference" —
     * у чужого (цены — ориентир, не счёт); "local" — локальная модель;
     * "unknown" — не найдено (found=false, поля нулевые). Плагин обязан
     * смотреть на source/found, а не считать нули настоящими лимитами.
     *
     * model_current — та же раскладка для модели, выбранной в хосте сейчас,
     * плюс "kind":"local"|"cloud". null/пустой model_id = текущая модель.
     *
     * Поля добавлены в КОНЕЦ структуры (append-only), проверка наличия —
     * через offsetof(LlamaHostApi, model_info), как у llm_chat_stream.
     */
    char* (*model_info)(LlamaPluginHost* host, const char* model_id);
    char* (*model_current)(LlamaPluginHost* host);

    /* --- RAG v2: скоупы (append-only, проверка offsetof) ---
     *
     * Легаси-функции rag_* выше остаются и работают в скоупе `app` (профиль
     * пользователя): плагин, собранный под 1.0.0, обязан работать на новом
     * хосте без изменений. Это ДОПОЛНЕНИЕ, а не замена.
     *
     * Скоуп задаётся строкой, но построить его волен только ХОСТ: имя
     * сверяется с идентичностью плагина (plugin:<name>[:<suffix>]),
     * результат обязан лежать внутри `<data_dir>/rag_scopes/`, а плагин не
     * может ни выбрать чужой индекс, ни выйти из этого каталога. Пустая
     * строка или nullptr = скоуп плагина по умолчанию.
     *
     * Наличие определяется так же, как у llm_chat_stream/model_info:
     * `api->size >= offsetof(LlamaHostApi, rag_search_ex) + sizeof(...)`.
     * Версия LLAMA_PLUGIN_API_VERSION НЕ поднимается — проверка в
     * plugin_manager.cpp строгая (равенство строк), и bump выключил бы все
     * существующие плагины.
     *
     * Возвращаемые `char*` освобождаются `free_string`, как у остальных.
     * Все функции возвращают JSON-объект с полем "ok" и, при отказе,
     * "code": "denied" | "disabled" | "unavailable" | "quota" | "bad_scope" |
     * "bad_index". Отказ — это значение "ok":false, а не пустая строка:
     * «неfound» и «нет доступа» обязаны различаться (R0.2).
     */

    /* Поиск в скоупе. {"ok":true,"scope":"...","chunks":[{content,
       document_id, chunk_index, file_path, symbol_name, start_line,
       end_line}]}
       Поля score пока нет: search_hybrid() сливает keyword- и векторную
       выдачу и релевантность не считает, а ноль «потому что так надо»
       хуже отсутствующего поля. Появится вместе с ранжированием. */
    char* (*rag_search_ex)(LlamaPluginHost* host, const char* scope,
                           const char* query, int k, const char* path_filter);

    /* Индексация документа. opts_json: {"max_bytes":N,"max_chunks":N,
       "deadline_ms":N,"reindex":true} — все поля необязательны (R4.1;
       до R4 принимается и игнорируется).
       {"ok":true,"chunks":N,"documents":M,"truncated":bool,"reason":"..."} */
    char* (*rag_index_document_ex)(LlamaPluginHost* host, const char* scope,
                                   const char* path, const char* opts_json);

    /* Сведения о скоупе: {"ok":true,"scope":"...","documents":N,"chunks":N,
       "bytes":N,"embeddings":bool,"model":"...","ready":bool,"reason":"..."}
       created_at появится с R4 (scope.json); до него поля просто нет. */
    char* (*rag_scope_info)(LlamaPluginHost* host, const char* scope);

    /* Удаление документа из скоупа. {"ok":true,...} */
    char* (*rag_scope_remove_document)(LlamaPluginHost* host, const char* scope,
                                       const char* path);

    /* Полная очистка скоупа: 1 при успехе, 0 при отказе (причина — в логе
     * хоста; JSON тут не нужен, вызывающему важно «сделано или нет»). */
    int   (*rag_scope_clear)(LlamaPluginHost* host, const char* scope);

    /* Немедленная запись индекса скоупа на диск: 1/0. */
    int   (*rag_scope_flush)(LlamaPluginHost* host, const char* scope);

    /* Отмена идущей индексации по хендлу (R4.3). Хендл выдаёт
     * rag_index_start; отмена ставит флаг, проверяемый в цикле чанков. */
    void  (*rag_index_cancel)(LlamaPluginHost* host, void* handle);

    /* R4.3: асинхронная индексация (append-only). start возвращает
     * непрозрачный хендл (nullptr при отказе; причина — в логе хоста).
     * wait(timeout_ms<=0 — бесконечно) возвращает malloc-JSON
     * {"ok":true,"chunks":N,"truncated":bool,"reason":"..."} или
     * {"ok":false,"code":"timeout"}; при завершении освобождает хендл.
     * cancel — см. выше. */
    void* (*rag_index_start)(LlamaPluginHost* host, const char* scope,
                             const char* path, const char* opts_json);
    char* (*rag_index_wait)(LlamaPluginHost* host, void* handle, int timeout_ms);
};

/* Информация о плагине (возвращается ll_plugin_info, статична) */
typedef struct LlamaPluginInfo {
    const char* name;        /* уникальное имя, [a-z0-9_-] */
    const char* version;     /* semver */
    const char* description; /* краткое описание */
    const char* author;      /* автор */
} LlamaPluginInfo;

/* ============================================================================
 * Функции, которые обязан экспортировать каждый плагин.
 * ========================================================================== */

/* Версия API, с которой совместим плагин (должна совпадать с хостом). */
LLAMA_PLUGIN_EXPORT const char* ll_plugin_api_version(void);

/* Статическая информация о плагине. */
LLAMA_PLUGIN_EXPORT const LlamaPluginInfo* ll_plugin_info(void);

/* Инициализация плагина. Возвращает 0 при успехе, иначе код ошибки. */
LLAMA_PLUGIN_EXPORT int ll_plugin_init(LlamaPluginHost* host, const LlamaHostApi* api);

/* Опционально: вызывается каждый кадр, внутри активного ImGui-контекста.
   Здесь плагин рисует свои окна через Dear ImGui. */
LLAMA_PLUGIN_EXPORT void ll_plugin_render(void);

/* Опционально: освобождение ресурсов перед выгрузкой. */
LLAMA_PLUGIN_EXPORT void ll_plugin_shutdown(void);

#ifdef __cplusplus
}
#endif

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

/* Уровни логирования */
enum {
    LLAMA_LOG_DEBUG = 0,
    LLAMA_LOG_INFO = 1,
    LLAMA_LOG_WARNING = 2,
    LLAMA_LOG_ERROR = 3
};

/* Колбэк плагина (команды, подтверждения диалогов) */
typedef void (*LlamaPluginCallback)(void* user_data);

typedef struct LlamaHostApi LlamaHostApi;

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

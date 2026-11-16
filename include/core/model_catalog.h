#pragma once

/*
 * model_catalog — справочник моделей models.dev для хоста.
 *
 * Зачем: плагин не знает, какая модель реально обслуживает запрос, поэтому
 * лимиты, цены и поддержка инструментов берутся из ручных настроек и
 * угадываются по frontmatter агента. Каталог models.dev содержит всё это
 * (`capabilities.tools`, `capabilities.reasoning`, `limit.context/output`,
 * `cost.text.*`, `family`, `canonical_id`).
 *
 * Источник — HTTP `https://models.opencode.ai/experimental/v2.0/api.json`,
 * поддерживающий `If-None-Match` (304). Ответ кэшируется на диск в
 * `~/.llama-gui/models_dev.json` (только нужная проекция полей) вместе с
 * etag. При недоступности сети работаем из кэша.
 *
 * Сопоставление идёт по `endpoint_url`, а НЕ по имени провайдера: у хоста
 * провайдер зовётся «Qwen (DashScope)», в каталоге — `alibaba`. Ключ —
 * `base_url` API провайдера.
 */

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

namespace llama_gui {
namespace core {

/* Одна запись каталога — только нужные хосту поля (проекция). */
struct CatalogModel {
    std::string provider;        // id провайдера в каталоге (alibaba, openrouter...)
    std::string provider_name;   // человекочитаемое имя
    std::string model_id;        // id модели в каталоге
    std::string canonical_id;    // канонический id (vendor/model)
    std::string name;            // отображаемое имя
    std::string family;          // семейство
    std::string base_url;        // chat-completions base_url
    bool tools_supported = false;
    bool tools_known = false;    // поле capabilities.tools присутствовало
    bool reasoning = false;
    int context = 0;
    int max_output = 0;
    double price_in = 0.0;
    double price_out = 0.0;
    double cache_read = 0.0;
    double cache_write = 0.0;
    bool has_cost = false;
};

/* Результат сопоставления модели хоста с каталогом. */
struct CatalogModelInfo {
    bool found = false;
    /* exact     — найдено у провайдера, чей endpoint совпал;
     * reference — найдено у чужого провайдера (цены — ориентир, не счёт);
     * local     — локальная модель (подсказки из каталога, если нашлись);
     * unknown   — не найдено. */
    std::string source = "unknown";
    std::string provider;
    std::string model_id;
    std::string canonical_id;
    std::string family;
    bool tools_supported = false;
    bool tools_known = false;
    bool reasoning = false;
    int context = 0;
    int max_output = 0;
    double price_in = 0.0;
    double price_out = 0.0;
    double cache_read = 0.0;
    double cache_write = 0.0;
    /* Есть ли в каталоге тариф вообще. Цена 0 у бесплатной модели — это
     * настоящая цена, поэтому «0/0» нельзя отличить от «тарифа нет» по
     * значениям; флаг нужен, чтобы auto_price не принял отсутствие данных
     * за бесплатность. */
    bool has_cost = false;

    /* JSON для ABI-полей model_info / model_current. */
    std::string to_json() const;
};

/* --- Чистые помощники (доступны тестам без сети и диска) --- */

/* Нормализация endpoint для сравнения: нижний регистр, без хвостового `/`
 * и без хвоста `/chat/completions`. */
std::string normalize_endpoint(const std::string& url);

/* Нормализация имени модели: нижний регистр, не-буквенно-цифровые → `-`,
 * схлопывание повторов. */
std::string normalize_model_name(const std::string& name);

/* Разбор ответа каталога (v2) в проекцию. Возвращает пустой вектор при
 * ошибке разбора. */
std::vector<CatalogModel> parse_catalog(const std::string& raw_json);

/* Сопоставление по endpoint и id модели.
 * Порядок: (1) exact (endpoint совпал + model_id); (2) canonical по
 * суффиксу с приоритетом провайдера из (1); (3) любой провайдер с тем же
 * canonical/model_id; (4) нормализованное совпадение имени. */
CatalogModelInfo match_model(const std::vector<CatalogModel>& models,
                      const std::string& endpoint_url,
                      const std::string& model_id);

/* Сопоставление локальной модели по пути/имени GGUF (source = "local"). */
CatalogModelInfo match_local_model(const std::vector<CatalogModel>& models,
                            const std::string& model_path);

class ModelCatalog {
public:
    static ModelCatalog& instance();

    /* Загрузить кэш с диска (быстро) и запустить фоновый refresh, если он
     * ещё не запускался. Идемпотентно. */
    void ensure_loaded();

    /* Синхронный refresh: HTTP с If-None-Match, при 200 — перезапись кэша. */
    void refresh_now();

    /* Фоновый refresh (не блокирует вызывающего). */
    void refresh_async(std::function<void()> done = {});

    /* Сопоставить облачную модель (endpoint + id). */
    CatalogModelInfo lookup(const std::string& endpoint_url,
                     const std::string& model_id) const;

    /* Сопоставить локальную модель по пути. */
    CatalogModelInfo lookup_local(const std::string& model_path) const;

    /* Тариф для auto_price. Возвращает true, если модель найдена в каталоге
     * И у записи есть тариф (has_cost). Цены — USD за 1M токенов.
     * "reference"-совпадение тоже годится: это ориентир лучше ручного
     * пресета, но вызывающий видит источник через CatalogModelInfo. */
    bool lookup_price(const std::string& endpoint_url,
                      const std::string& model_id,
                      double& price_in, double& price_out) const;

    /* Полный JSON-снимок текущей облачной/локальной модели для ABI. */
    std::string current_json(bool cloud, const std::string& endpoint_url,
                             const std::string& model_id,
                             const std::string& local_path) const;

    bool loaded() const { return loaded_.load(); }
    size_t size() const;

private:
    ModelCatalog() = default;

    void load_cache_locked();
    void save_cache(const std::vector<CatalogModel>& models,
                    const std::string& etag);

    std::string cache_path() const;
    std::string etag_path() const;

    mutable std::mutex mutex_;
    std::vector<CatalogModel> models_;
    std::string etag_;
    std::atomic<bool> loaded_{false};
    std::atomic<bool> refreshing_{false};
    std::atomic<bool> refresh_started_{false};
};

} // namespace core
} // namespace llama_gui

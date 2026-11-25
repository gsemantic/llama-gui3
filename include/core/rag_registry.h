#pragma once

/*
 * rag_registry.h — реестр скоупов RAG (R1.2).
 *
 * ЗАЧЕМ. Сторы (R1.1) дают изоляцию индексов, но кто-то должен решать,
 * какому плагину какой стор достаётся, где он лежит и сколько ему можно.
 * Это и есть реестр: одна точка, где имя плагина превращается в
 * безопасный каталог, а флаг «выключено» — в отказ.
 *
 * ПОЧЕМУ ИМЯ СТРОИТ ХОСТ, А НЕ ПЛАГИН. Плагин мог бы попросить скоуп
 * `../../etc` и получить доступ к чужому индексу. Поэтому реестр принимает
 * имя плагина (от хоста, из `ll_plugin_info`) и сам выводит имя скоупа и
 * путь. Плагин может лишь задать СУФФИКС (например, `archive`), и тот
 * проверяется отдельно.
 *
 * ЧТО ЗАЩИЩЕНО.
 *   - normalize_scope: только `[a-z][a-z0-9_-]*`, длина 1..64. Пусто →
 *     отказ, а не «придумаем имя».
 *   - scope_dir: путь строится как `<root>/<scope>`, где scope уже
 *     нормализован. Дополнительно проверяется, что результат лежит
 *     ВНУТРИ root (на случай, если root сам="" или scope подменили).
 *   - parse_scope_options: квоты клампятся, отрицательные/нулевые
 *     значения трактуются как «не задано», а не как «ноль».
 *
 * ЧЕГО РЕЕСТР НЕ ДЕЛАЕТ. Не проверяет права (это хост, R3) и не решает,
 * что индексировать. Он выдаёт стор и следит за его жизненным циклом.
 *
 * ЖИЗНЕННЫЙ ЦИКЛ. Сторы создаются лениво при первом обращении.
 * Вытеснение — LRU по `max_loaded_` (по умолчанию 4): вытесненный стор
 * флашится в деструкторе, данные не теряются. Вытеснение, а не
 * «создали и держим»: N плагинов × FAISS-индекс в памяти — это
 * N × десятки мегабайт.
 *
 * ПОТОКОБЕЗОПАСНОСТЬ. `mtx_` вокруг карты и LRU. Порядок: реестр → стор
 * (стор не зовёт реестр).
 */

#include "core/rag_store.h"
#include "core/embedding_generator.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace llama_gui {
namespace core {

/* Параметры скоупа из настроек плагина (R1.2). Все нули означают «не
 * задано» — реестр подставит значения по умолчанию. */
struct RagScopeOptions {
    int max_documents = 0;       /* 0 = без предела (кроме диска) */
    long long max_index_bytes = 0; /* 0 = предел хоста */
    bool allow_index = true;     /* можно ли индексировать, или только читать */
    bool read_only = false;      /* синоним «только чтение», для UI */

    /* R4.1: опции одного вызова индексации (opts_json ABI). 0 = «как в
     * настройках скоупа» (то есть берётся max_documents/max_index_bytes). */
    long long max_bytes = 0;     /* предел байт одного документа; 0 = нет */
    int max_chunks = 0;          /* предел чанков одного документа; 0 = нет */
    long long deadline_ms = 0;   /* дедлайн индексации; 0 = без дедлайна */
    bool reindex = false;        /* переиндексировать изменённый документ */
};

/* Разбор JSON-настроек скоупа. Терпим к отсутствию полей и к мусору:
 * возвращает false только если это вообще не объект, а не «нет поля».
 *
 * Такое поведение выбрано потому, что настройки приходят из профиля
 * пользователя, который правится руками: «в profile.json опечатка в имени
 * поля» не должно означать «плагин не может искать». */
bool parse_scope_options(const std::string& json,
                         RagScopeOptions& out,
                         std::string* error = nullptr);

/* Привести имя плагина к имени скоупа.
 *
 * - пусто/невалидно → false (вызывающий обязан отказать, а не угадать);
 * - `suffix` добавляется как `:<suffix>`, если задан и сам валиден;
 * - результат пригоден как имя каталога.
 *
 * Возвращает имя вида `plugin:<name>` / `plugin:<name>:<suffix>`? Нет:
 * двоеточие — разделитель, и в имени каталога оно не нужно. Реестр
 * хранит ИМЯ СКОУПА (с двоеточиями, для диагностики) и ПУТЬ (без них,
 * см. scope_dir). */
bool normalize_scope(const std::string& plugin_name,
                     const std::string& suffix,
                     std::string& scope_out,
                     std::string* error = nullptr);

/* Путь каталога для скоупа внутри root.
 *
 * Проверяет, что результат лежит ВНУТРИ root: если scope после
 * нормализации каким-то образом содержит `..` или `/`, возвращает "".
 * Это последняя линия: normalize_scope уже не пропускает такие имена, но
 * путь склеивается в одном месте, и проверка там дешевле, чем аудит
 * всех вызывающих. */
std::string scope_dir(const std::string& root, const std::string& scope);

class RagRegistry {
public:
    /* root — каталог, где лежат индексы скоупов (`<data_dir>/rag_scopes`).
     * embedding — общий генератор (один на процесс, R1.3).
     * max_loaded — сколько сторов держать в памяти; по умолчанию 4. */
    explicit RagRegistry(std::string root,
                         std::shared_ptr<EmbeddingGenerator> embedding = nullptr,
                         size_t max_loaded = 4);
    ~RagRegistry();

    RagRegistry(const RagRegistry&) = delete;
    RagRegistry& operator=(const RagRegistry&) = delete;

    /* Стор для плагина. Создаётся лениво. Возвращает nullptr, если имя
     * невалидно; причина — в error. */
    std::shared_ptr<RagStore> get(const std::string& plugin_name,
                                  const std::string& suffix = "",
                                  std::string* error = nullptr);

    /* Сведения о скоупе без создания стора: name/path/существует. */
    RagStoreInfo info(const std::string& plugin_name,
                      const std::string& suffix = "") const;

    /* Список известных скоупов (по каталогам на диске, не только по
     * загруженным сторам): после перезапуска процесса это единственный
     * способ узнать, что индекс был. */
    std::vector<std::string> list() const;

    /* Полная очистка скоупа: индекс + файлы. */
    bool remove(const std::string& plugin_name, const std::string& suffix = "");

    /* Немедленная запись всех загруженных сторов. */
    void flush_all();

    /* Число загруженных сторов (для тестов и UI). */
    size_t loaded_count() const;

    const std::string& root() const { return root_; }

private:
    std::string root_;
    std::shared_ptr<EmbeddingGenerator> embedding_;
    size_t max_loaded_;

    mutable std::mutex mtx_;
    /* Порядок = LRU: спереди самые давно неиспользуемые. */
    std::vector<std::shared_ptr<RagStore>> lru_;

    /* Найти/создать стор под мьютексом. Вызывать только из get(). */
    std::shared_ptr<RagStore> get_locked(const std::string& scope,
                                         const std::string& path,
                                         std::string* error);
    void touch_locked(RagStore* store);
    void evict_locked();
};

} // namespace core
} // namespace llama_gui
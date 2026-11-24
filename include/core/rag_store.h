#pragma once

/*
 * rag_store.h — один изолированный индекс RAG (R1.1).
 *
 * ЗАЧЕМ. До этого RAG был глобальным: `RagManager` один на приложение,
 * его индекс и «текущий профиль» — тоже одни. Плагин, позвавший
 * `rag_process_document`, писал в индекс пользователя; плагин с другим
 * именем мог переключить профиль и ослепить первый. Это не теоретическое
 * ограничение: `wp_coder`'s `rag_index` перестраивает профиль, и после
 * него индекс другого плагина исчезал.
 *
 * ЧТО ТАКОЕ СТОР. Один индекс + его чанки + путь на диске + мьютекс.
 * Снаружи (для плагина через ABI) он известен как «скоуп»: имя, которое
 * строит хост из идентичности плагина, а не строка от плагина.
 *
 * ПОЧЕМУ КОМПОЗИЦИЯ, А НЕ НАСЛЕДОВАНИЕ/ВЫНОС. Физический вынос индекса
 * из RagManager затронул бы 94 обращения в пяти файлах
 * (`rag_manager_{documents,search,persistence,cache,core}.cpp`) и
 * потребовал бы переписать поиск, гибридный поиск и персистентность.
 * Стор владеет ОДНИМ RagManager, и тот владеет ОДНИМ индексом: правило
 * «один владелец индекса» соблюдено, а риск регрессии — нет.
 *
 * ЧТО СТОР НЕ ДЕЛАЕТ. Не решает, что индексировать (это плагин), не
 * определяет права (это реестр/хост) и не смешивает свои чанки с чужими:
 * у каждого стора свой RagManager со своим профилем и каталогом на диске.
 *
 * ПОТОКОБЕЗОПАСНОСТЬ. Все публичные методы берут `mtx_`: RagManager не
 * потокобезопасен (у него есть только `progress_mutex_`), а плагины зовут
 * RAG из фоновых потоков. Порядок блокировок: реестр → стор; стор
 * никогда не зовёт реестр, удерживая свой мьютекс.
 */

#include "core/rag_manager.h"
#include "core/embedding_generator.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace llama_gui {
namespace core {

/* Сведения о сторе для UI и диагностики. Считаются ПОД МЬЮТЕКСОМ и
 * отдаются копией: отдавать ссылки на внутренние поля значило бы
 * отдать数据, которые устаревают между кадрами. */
struct RagStoreInfo {
    std::string scope;          /* имя скоупа, как его знает реестр */
    std::string path;           /* каталог на диске */
    bool ready = false;         /* индекс загружен и пригоден */
    bool embeddings = false;    /* есть рабочий генератор эмбеддингов */
    size_t documents = 0;
    size_t chunks = 0;
    size_t bytes = 0;           /* размер файлов индекса на диске, если есть */
    std::string model;          /* модель эмбеддингов (basename) */
    std::string reason;         /* почему ready == false, words */
};

class RagStore {
public:
    /* path — каталог ЭТОГО стора (его создаёт реестр из имени скоупа).
     * embedding — общий генератор (может быть nullptr: тогда стор ищет
     *   только по BM25 и честно сообщает embeddings=false).
     * model_path — путь к модели для СВОЕГО генератора, если общего нет. */
    RagStore(std::string scope, std::string path,
             std::shared_ptr<EmbeddingGenerator> embedding,
             std::string model_path = "");
    ~RagStore();

    RagStore(const RagStore&) = delete;
    RagStore& operator=(const RagStore&) = delete;

    /* Индексация документа. Возвращает число чанков; 0 — не удалось
     * (причина в日志 и в info().reason для вызывающего). */
    int index_document(const std::string& file_path);

    /* Поиск по ИНДЕКСУ ЭТОГО СТОРА. Чужие чанки сюда не попадают по
     * построению: у стора свой RagManager. */
    std::vector<RagChunk> search(const std::string& query, int k,
                                 const std::string& path_filter = "");

    /* Немедленная запись индекса на диск. */
    bool flush();

    /* Полная очистка индекса (и файлов). */
    void clear();

    /* R2.2: удалить документ из индекса стора.
     *
     * Убирает чанки (RagManager::remove_document) И запись в профиле:
     * без второй части документ остался бы «проиндексированным», и
     * дедупликация R1.6 не дала бы вернуть его в индекс.
     *
     * Возвращает false, если чанков документа в индексе не было (это не
     * ошибка: повторное удаление — «ничего не делать»). */
    bool remove_document(const std::string& file_path);

    RagStoreInfo info() const;

    const std::string& scope() const { return scope_; }

private:
    std::string scope_;
    std::string path_;
    std::shared_ptr<EmbeddingGenerator> embedding_;
    std::string model_path_;
    std::unique_ptr<RagManager> manager_;
    mutable std::mutex mtx_;

    /* Создать RagManager с профилем в path_/profile.json и подключить
     * общий генератор. Вызывается лениво, под мьютексом. */
    RagManager& ensure_manager();
    /* Пересчитать info_ из состояния менеджера (под мьютексом). */
    void refresh_info_locked(RagStoreInfo& out) const;
};

} // namespace core
} // namespace llama_gui
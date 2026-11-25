/*
 * rag_store.cpp — один изолированный индекс RAG (R1.1).
 *
 * Обоснование и границы — в rag_store.h. Здесь только код.
 */

#include "core/rag_store.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>

namespace fs = std::filesystem;

namespace llama_gui {
namespace core {

namespace {

/* Размер файлов индекса на диске. 0, если каталога ещё нет. */
size_t dir_bytes(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return 0;
    size_t total = 0;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (!entry.is_regular_file()) continue;
        total += static_cast<size_t>(fs::file_size(entry.path(), ec));
    }
    return total;
}

} // namespace

RagStore::RagStore(std::string scope, std::string path,
                   std::shared_ptr<EmbeddingGenerator> embedding,
                   std::string model_path)
    : scope_(std::move(scope)), path_(std::move(path)),
      embedding_(std::move(embedding)), model_path_(std::move(model_path)) {
    /* Каталог создаётся сразу: лучше пустой каталог, чем «индекса нет»,
     * из-за которого вызывающий решит, что индексация не удалась. */
    std::error_code ec;
    if (!path_.empty() && !fs::exists(path_, ec)) {
        fs::create_directories(path_, ec);
        if (ec) {
            std::cerr << "[RagStore] " << scope_ << ": cannot create " << path_
                      << ": " << ec.message() << std::endl;
        }
    }
}

RagStore::~RagStore() {
    /* Флаш на разрушении: стор умирает при выгрузке плагина, и индекс,
     * не записанный на диск, пропал бы вместе с процессом. */
    std::lock_guard<std::mutex> lk(mtx_);
    if (manager_) {
        manager_->save_index();
    }
}

RagManager& RagStore::ensure_manager() {
    if (!manager_) {
        /* Пустой model_path означает «пусть RagManager создаст свой
         * генератор по умолчанию». Общий генератор подключается сразу
         * после构造: до первой индексации, иначе размерность индекса
         * зафиксировалась бы от одной модели, а векторы считались бы
         * другой. */
        manager_ = std::make_unique<RagManager>(model_path_, path_);
        if (embedding_ && embedding_->is_loaded()) {
            manager_->set_shared_embedding_generator(embedding_);
        }
        /* R1.1: профиль ОБЯЗАТЕЛЕН. Без него RagManager считает, что
         * «своего» индекса нет, и save_index()/process_document() уходят
         * в `~/.llama-gui/rag_indexes/` — то есть все скоупы пишут в один
         * файл пользователя, и изоляция существует только в памяти
         * процесса. Профиль создаётся в каталоге стора, поэтому путь
         * индекса получается `<scope>/default_index.faiss`.
         *
         * Имя `default` — потому что create_profile() строит путь как
         * `<profiles_dir>/<name>_index.faiss`, а пустое имя она
         * отвергает. Расширение скоупа в имя файла не добавляем: каталог
         * уже уникален. */
        if (manager_->get_index_profile_names().empty()) {
            if (!manager_->create_index_profile("default", /*source_directory=*/"")) {
                std::cerr << "[RagStore] " << scope_
                          << ": cannot create index profile in " << path_
                          << " — index will not persist" << std::endl;
            }
        }
    }
    return *manager_;
}

int RagStore::index_document(const std::string& file_path) {
    /* Совместимость: старый вызов = индексация без квот (как R1/R2). */
    return index_document(file_path, RagScopeOptions{}, nullptr).chunks;
}

RagStore::IndexResult RagStore::index_document(const std::string& file_path,
                                               const RagScopeOptions& opts) {
    return index_document(file_path, opts, nullptr);
}

RagStore::IndexResult RagStore::index_document(const std::string& file_path,
                                               const RagScopeOptions& opts,
                                               std::atomic<bool>* cancel) {
    IndexResult result;
    if (file_path.empty()) {
        result.reason = "empty path";
        return result;
    }
    /* Read-only скоуп: не индексируем (но и не «truncated» — это не квота,
     * а режим). Причина в reason, чанков 0. */
    if (!opts.allow_index) {
        result.reason = "scope is read-only";
        return result;
    }
    /* R4.3: отмена ещё до старта. */
    if (cancel && cancel->load()) {
        result.truncated = true;
        result.reason = "cancelled";
        return result;
    }

    /* R4.2, квота на документ (max_bytes): проверяем ДО чтения, чтобы не
     * читать гигантский файл целиком. 0 = без предела. */
    if (opts.max_bytes > 0) {
        std::error_code ec;
        const auto sz = fs::file_size(file_path, ec);
        if (!ec && static_cast<long long>(sz) > opts.max_bytes) {
            result.truncated = true;
            result.reason = "document exceeds max_bytes (" +
                            std::to_string(sz) + " > " +
                            std::to_string(opts.max_bytes) + ")";
            return result;
        }
    }

    std::lock_guard<std::mutex> lk(mtx_);
    RagManager& mgr = ensure_manager();

    /* R1.6: повторная индексация НЕ удваивает чанки. `reindex` (R4.1)
     * переиндексирует изменённый документ: сначала убираем старые чанки и
     * запись профиля, потом добавляем заново. */
    bool already = false;
    for (const auto& doc : mgr.get_current_profile_documents()) {
        if (doc == file_path) { already = true; break; }
    }
    if (already && !opts.reindex) {
        std::cout << "[RagStore] " << scope_
                  << ": document already indexed, skipping: " << file_path
                  << std::endl;
        return result;
    }
    if (already && opts.reindex) {
        mgr.remove_document(file_path);
        mgr.remove_document_from_current_profile(file_path);
    }

    /* R4.3: отмена/дедлайн — в цикле чанков RagManager. */
    mgr.set_index_cancel_flag(cancel);
    mgr.set_index_deadline_ms(opts.deadline_ms);
    const auto start = std::chrono::steady_clock::now();
    const size_t before = mgr.get_external_chunks_count();

    const bool ok = mgr.process_document(file_path, /*report_completion=*/false);

    const size_t added = mgr.get_external_chunks_count() - before;
    const bool was_cancelled = mgr.index_was_cancelled();
    const bool cancel_now = cancel && cancel->load();
    mgr.set_index_cancel_flag(nullptr);
    mgr.set_index_deadline_ms(0);

    /* R4.3: отмена/дедлайн — частичный результат БЕЗ отката: уже
     * добавленные чанки остаются, caller видит truncated и причину.
     * ВАЖНО: проверяем ДО `!ok`, иначе отмена, прервавшая до первого
     * чанка (total_chunks==0 → process_document вернул false), выглядела
     * бы как «process_document failed», а не «cancelled». */
    if (was_cancelled || cancel_now) {
        result.truncated = true;
        result.chunks = static_cast<int>(added);
        result.reason = cancel_now ? "cancelled" : "deadline exceeded";
        return result;
    }

    if (!ok) {
        result.reason = "process_document failed";
        return result;
    }

    /* Откат: документ не должен остаться в индексе, если квота превышена.
     * Убираем и чанки, и запись профиля (иначе дедупликация R1.6 сочтёт
     * документ проиндексированным и не даст повторить). */
    auto rollback = [&]() {
        mgr.remove_document(file_path);
        mgr.remove_document_from_current_profile(file_path);
    };

    /* R4.2, квота на документ (max_chunks). */
    if (opts.max_chunks > 0 && static_cast<int>(added) > opts.max_chunks) {
        rollback();
        result.truncated = true;
        result.reason = "document exceeds max_chunks (" +
                        std::to_string(added) + " > " +
                        std::to_string(opts.max_chunks) + ")";
        return result;
    }
    /* R4.2, квота на индекс (max_index_bytes). */
    if (opts.max_index_bytes > 0 &&
        dir_bytes(path_) > static_cast<size_t>(opts.max_index_bytes)) {
        rollback();
        result.truncated = true;
        result.reason = "index exceeds max_index_bytes";
        return result;
    }

    (void)start;
    result.chunks = static_cast<int>(added);
    return result;
}

std::vector<RagChunk> RagStore::search(const std::string& query, int k,
                                       const std::string& path_filter) {
    if (query.empty() || k <= 0) return {};
    std::lock_guard<std::mutex> lk(mtx_);
    RagManager& mgr = ensure_manager();
    return mgr.search_hybrid(query, k, path_filter);
}

bool RagStore::flush() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!manager_) return false;
    return manager_->save_index();
}

bool RagStore::remove_document(const std::string& file_path) {
    if (file_path.empty()) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    RagManager& mgr = ensure_manager();

    const bool removed = mgr.remove_document(file_path);
    /* Профиль чистим ВСЕГДА, даже если чанков не было: расхождение
     * «в индексе нет, в профиле есть» — это состояние, в котором
     * дедупликация R1.6 молча отказывается индексировать документ
     * повторно. Лучше потратить один проход по списку, чем оставить
     * документ «вечно проиндексированным». */
    mgr.remove_document_from_current_profile(file_path);
    return removed;
}

void RagStore::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (manager_) {
        manager_->clear_all_indexes();
    }
    std::error_code ec;
    fs::remove_all(path_, ec);
    if (!path_.empty() && !fs::exists(path_, ec)) {
        fs::create_directories(path_, ec);
    }
    /* Профиль жил в каталоге стора и удалён вместе с ним: держать менеджер
     * с профилем, которого нет на диске, значит при следующем save_index()
     * получить отказ «нет профиля». Пусть следующий вызов создаст менеджер
     * и профиль заново — это и есть «чистый лист». */
    manager_.reset();
}

void RagStore::refresh_info_locked(RagStoreInfo& out) const {
    out.scope = scope_;
    out.path = path_;
    out.bytes = dir_bytes(path_);

    /* R1.3: если менеджер уже создан, правду о генераторе надо спрашивать
     * у НЕГО: у него может быть общий генератор (embedding_ == nullptr),
         * и тогда «embeddings=false» было бы ложью — стор ищет, а info()
 * говорит, что не может. */
    if (manager_) {
        out.embeddings = manager_->has_embedding_generator();
    } else {
        out.embeddings = embedding_ ? embedding_->is_loaded() : false;
    }
    out.model = embedding_ ? embedding_->get_model_path() : std::string();

    if (!manager_) {
        out.ready = false;
        out.reason = out.embeddings ? "индекс ещё не открыт" : "нет эмбеддингов";
        return;
    }
    out.documents = manager_->get_current_profile_documents().size();
    out.chunks = manager_->get_external_chunks_count();
    out.ready = out.embeddings;
    if (!out.embeddings) out.reason = "нет эмбеддингов";
}

RagStoreInfo RagStore::info() const {
    std::lock_guard<std::mutex> lk(mtx_);
    RagStoreInfo out;
    refresh_info_locked(out);
    return out;
}

} // namespace core
} // namespace llama_gui
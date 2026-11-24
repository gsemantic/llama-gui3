/*
 * rag_store.cpp — один изолированный индекс RAG (R1.1).
 *
 * Обоснование и границы — в rag_store.h. Здесь только код.
 */

#include "core/rag_store.h"

#include <filesystem>
#include <fstream>
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
    if (file_path.empty()) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    RagManager& mgr = ensure_manager();

    /* R1.6: повторная индексация НЕ удваивает чанки.
     *
     * `RagManager::process_document()` всегда ДОБАВЛЯЕТ чанки — дедупликации
     * в нём нет (проверено: ни `documents_index_`, ни проверки «документ уже
     * в профиле»). Для легаси-чата это незаметно, а для стора это означало
     * бы, что второй вызов `rag_index` с тем же путём удваивает индекс и
     * поиск возвращает один и тот же кусок дважды.
     *
     * Считаем документ уже проиндексированным, если он есть в профиле стора.
     * Переиндексация (изменившийся файл) — задача R4.1 с флагом
     * `reindex`; до неё «уже индексировали» — это «ничего не делаем», а не
     * «строим вторую копию». */
    for (const auto& doc : mgr.get_current_profile_documents()) {
        if (doc == file_path) {
            std::cout << "[RagStore] " << scope_
                      << ": document already indexed, skipping: " << file_path
                      << std::endl;
            return 0;
        }
    }

    const size_t before = mgr.get_external_chunks_count();
    if (!mgr.process_document(file_path, /*report_completion=*/false)) {
        return 0;
    }
    const size_t after = mgr.get_external_chunks_count();
    /* Число чанков, а не «успех»: файл мог быть пустым или не содержать
     * текста — это не отказ индексации, и вызывающему полезнее знать
     * «ноль чанков», чем «ошибка». */
    return static_cast<int>(after - before);
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
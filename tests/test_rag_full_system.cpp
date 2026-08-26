/**
 * @file test_rag_full_system.cpp
 * @brief Полная система RAG с AST chunking и гибридным поиском
 * 
 * Компиляция:
 *   g++ -std=c++17 -O2 -DUSE_CURL \
 *       -I../../include/core \
 *       -I/usr/include/x86_64-linux-gnu \
 *       -o test_rag_full_system test_rag_full_system.cpp \
 *       ../../build/src/core/libcore.a \
 *       ../../build/libtree-sitter-grammar-*.a \
 *       -ltree-sitter -lcurl
 * 
 * Запуск:
 *   ./test_rag_full_system
 * 
 * Требования:
 *   - llama-server с granite-embedding-107m-multilingual на порту 8081
 *   - libtree-sitter-dev
 *   - libcurl-dev
 */

#include "rag_manager.h"
#include "document_parser.h"
#include "embedding_generator.h"
#include "ast_parser.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <cmath>

using namespace llama_gui::core;
using namespace std::chrono;

// ============================================================================
// Конфигурация системы
// ============================================================================

struct RAGConfig {
    // Chunking
    int max_tokens_per_chunk = 200;        // Оптимально для granite-embedding (512 limit)
    int embedding_max_chars = 1500;        // Лимит перед отправкой на сервер
    
    // Поиск
    float bm25_weight = 0.4f;              // BM25 вес в гибридном поиске
    float vector_weight = 0.6f;            // Vector вес в гибридном поиске
    
    // Сервер эмбеддингов
    std::string embedding_server = "http://localhost:8081";
    
    // Модель
    std::string embedding_model = "granite-embedding-107m-multilingual";
    int embedding_dimension = 384;
};

// ============================================================================
// Индексированный чанк
// ============================================================================

struct IndexedChunk {
    std::string content;
    std::string file_path;
    std::string chunk_type;    // "code", "text", "preamble"
    std::vector<float> embedding;
};

// ============================================================================
// Результат поиска
// ============================================================================

struct SearchResult {
    float score;
    std::string content;
    std::string file_path;
    std::string chunk_type;
    int chunk_index;
};

// ============================================================================
// Основной класс RAG системы
// ============================================================================

class RAGSystem {
public:
    RAGSystem(const RAGConfig& config = RAGConfig()) : config_(config) {
        embedder_.set_server_url(config.embedding_server);
        embedder_.load_model();
    }
    
    ~RAGSystem() = default;

    // --- Индексирование ---
    
    /**
     * @brief Проиндексировать файл (код или текст)
     * @param file_path Путь к файлу
     * @return Количество добавленных чанков
     */
    int index_file(const std::string& file_path) {
        std::string language = DocumentParser::get_language(file_path);
        bool is_code = !language.empty();
        
        std::vector<std::string> chunks;
        
        if (is_code) {
            chunks = index_code_file(file_path, language);
        } else {
            chunks = index_text_file(file_path);
        }
        
        // Генерируем эмбеддинги
        int added = 0;
        for (const auto& chunk : chunks) {
            auto emb = embedder_.generate_embedding(chunk);
            if (!emb.empty() && emb[0] != 0.0f) {
                IndexedChunk indexed;
                indexed.content = chunk;
                indexed.file_path = file_path;
                indexed.chunk_type = is_code ? "code" : "text";
                indexed.embedding = std::move(emb);
                indexed_chunks_.push_back(std::move(indexed));
                added++;
            }
        }
        
        return added;
    }
    
    /**
     * @brief Проиндексировать директорию рекурсивно
     * @param dir_path Путь к директории
     * @param extensions Расширения файлов для индексации
     * @return Общее количество чанков
     */
    int index_directory(const std::string& dir_path, 
                        const std::vector<std::string>& extensions = {".cpp", ".h", ".py", ".rs", ".txt", ".md"}) {
        int total = 0;
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
            if (!entry.is_regular_file()) continue;
            
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            bool should_index = false;
            for (const auto& e : extensions) {
                if (ext == e) { should_index = true; break; }
            }
            
            if (should_index) {
                int added = index_file(entry.path().string());
                total += added;
            }
        }
        
        return total;
    }

    // --- Поиск ---
    
    /**
     * @brief Поиск по проиндексированным документам
     * @param query Запрос
     * @param top_k Количество результатов
     * @return Вектор результатов поиска
     */
    std::vector<SearchResult> search(const std::string& query, int top_k = 5) {
        auto query_emb = embedder_.generate_embedding(query);
        
        std::vector<SearchResult> results;
        
        for (size_t i = 0; i < indexed_chunks_.size(); ++i) {
            const auto& chunk = indexed_chunks_[i];
            
            // Векторная схожесть
            float vector_score = cosine_similarity(query_emb, chunk.embedding);
            
            // Ключевое слово совпадение (BM25 приближение)
            float keyword_score = keyword_match(query, chunk.content);
            
            // Гибридный скор
            float hybrid_score = keyword_score * config_.bm25_weight + 
                               vector_score * config_.vector_weight;
            
            results.push_back({
                hybrid_score,
                chunk.content,
                chunk.file_path,
                chunk.chunk_type,
                static_cast<int>(i)
            });
        }
        
        // Сортировка по убыванию
        std::sort(results.begin(), results.end(),
            [](const SearchResult& a, const SearchResult& b) {
                return a.score > b.score;
            });
        
        if (static_cast<int>(results.size()) > top_k) {
            results.resize(top_k);
        }
        
        return results;
    }

    // --- Статистика ---
    
    void print_stats() const {
        std::cout << "\n=== СТАТИСТИКА RAG СИСТЕМЫ ===" << std::endl;
        std::cout << "Всего чанков: " << indexed_chunks_.size() << std::endl;
        
        std::map<std::string, int> type_counts;
        std::map<std::string, int> file_counts;
        
        for (const auto& chunk : indexed_chunks_) {
            type_counts[chunk.chunk_type]++;
            std::string filename = std::filesystem::path(chunk.file_path).filename().string();
            file_counts[filename]++;
        }
        
        std::cout << "\nПо типу:" << std::endl;
        for (const auto& [type, count] : type_counts) {
            std::cout << "  " << type << ": " << count << std::endl;
        }
        
        std::cout << "\nПо файлам:" << std::endl;
        for (const auto& [file, count] : file_counts) {
            std::cout << "  " << file << ": " << count << std::endl;
        }
    }
    
    size_t get_chunks_count() const { return indexed_chunks_.size(); }
    const RAGConfig& get_config() const { return config_; }

private:
    RAGConfig config_;
    EmbeddingGenerator embedder_;
    std::vector<IndexedChunk> indexed_chunks_;
    
    // Индексация кодовых файлов (AST)
    std::vector<std::string> index_code_file(const std::string& file_path, const std::string& language) {
        std::vector<std::string> chunks;
        
        AstParser parser;
        AstNode root = parser.parse_file(file_path, language);
        
        // Preamble (includes/imports)
        AstNode preamble = parser.extract_preamble(root, root.content, language);
        if (!preamble.content.empty()) {
            chunks.push_back("[[preamble]]" + preamble.content);
        }
        
        // Code nodes
        auto nodes = parser.extract_top_level_nodes(root);
        std::vector<AstNode> final_nodes;
        
        for (const auto& node : nodes) {
            auto split = parser.split_large_node(node, config_.max_tokens_per_chunk);
            for (auto& s : split) {
                final_nodes.push_back(std::move(s));
            }
        }
        
        auto code_chunks = parser.nodes_to_chunks(final_nodes, file_path);
        for (const auto& c : code_chunks) {
            chunks.push_back(c);
        }
        
        return chunks;
    }
    
    // Индексация текстовых файлов
    std::vector<std::string> index_text_file(const std::string& file_path) {
        std::vector<std::string> chunks;
        
        auto parts = DocumentParser::parse_document(file_path);
        RagManager rag;
        
        for (const auto& part : parts) {
            auto text_chunks = rag.split_into_chunks(part, config_.max_tokens_per_chunk);
            for (const auto& c : text_chunks) {
                chunks.push_back(c);
            }
        }
        
        return chunks;
    }
    
    // Косинусное сходство
    static float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
        if (a.size() != b.size() || a.empty()) return 0.0f;
        float dot = 0, norm_a = 0, norm_b = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            dot += a[i] * b[i];
            norm_a += a[i] * a[i];
            norm_b += b[i] * b[i];
        }
        return dot / (std::sqrt(norm_a) * std::sqrt(norm_b) + 1e-8f);
    }
    
    // Ключевое слово совпадение
    static float keyword_match(const std::string& query, const std::string& text) {
        std::string lower_query = query;
        std::string lower_text = text;
        std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);
        std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);
        
        int matches = 0;
        int total = 0;
        std::istringstream iss(lower_query);
        std::string word;
        
        while (iss >> word) {
            total++;
            if (lower_text.find(word) != std::string::npos) {
                matches++;
            }
        }
        
        return total > 0 ? static_cast<float>(matches) / total : 0.0f;
    }
};

// ============================================================================
// Демонстрация
// ============================================================================

void print_search_results(const std::string& query, const std::vector<SearchResult>& results) {
    std::cout << "\n--- Запрос: \"" << query << "\" ---" << std::endl;
    
    for (size_t i = 0; i < results.size(); ++i) {
        std::string file = std::filesystem::path(results[i].file_path).filename().string();
        std::string snippet = results[i].content;
        if (snippet.size() > 100) snippet = snippet.substr(0, 100) + "...";
        std::replace(snippet.begin(), snippet.end(), '\n', ' ');
        
        std::cout << "  " << (i+1) << ". [" << std::fixed << std::setprecision(3) << results[i].score 
                  << "] " << file << " (" << results[i].chunk_type << ")" << std::endl;
        std::cout << "     " << snippet << std::endl;
    }
}

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "  RAG SYSTEM - Full Demo" << std::endl;
    std::cout << "  granite-embedding-107m-multilingual" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Конфигурация
    RAGConfig config;
    
    // Проверка аргументов
    if (argc > 1) {
        config.embedding_server = argv[1];
    }
    
    std::cout << "\nКонфигурация:" << std::endl;
    std::cout << "  Chunk size: " << config.max_tokens_per_chunk << " tokens" << std::endl;
    std::cout << "  Embedding server: " << config.embedding_server << std::endl;
    std::cout << "  Weights: BM25=" << config.bm25_weight << " Vector=" << config.vector_weight << std::endl;
    
    // Создание RAG системы
    RAGSystem rag(config);
    
    // Индексация файлов проекта
    std::cout << "\n=== ИНДЕКСАЦИЯ ===" << std::endl;
    auto t_start = high_resolution_clock::now();
    
    std::string base = "/home/Alex/projects/llama-b7472-bin-ubuntu-x64/llama-gui";
    
    // Код
    rag.index_file(base + "/include/core/ast_parser.h");
    rag.index_file(base + "/src/core/ast_parser.cpp");
    rag.index_file(base + "/include/core/rag_manager.h");
    rag.index_file(base + "/src/core/rag_manager_documents.cpp");
    rag.index_file(base + "/src/core/rag_manager_search.cpp");
    rag.index_file(base + "/include/core/document_parser.h");
    rag.index_file(base + "/src/core/document_parser.cpp");
    
    // Текст
    rag.index_file("/tmp/test_en_large.txt");
    rag.index_file("/tmp/test_ru_text.txt");
    
    auto t_end = high_resolution_clock::now();
    double index_ms = duration_cast<milliseconds>(t_end - t_start).count();
    
    rag.print_stats();
    std::cout << "\nИндексация: " << std::fixed << std::setprecision(0) << index_ms << " ms" << std::endl;
    
    // Тестовые запросы
    std::cout << "\n=== ПОИСК ===" << std::endl;
    
    // Кодовые запросы
    print_search_results("AST parsing tree-sitter functions", rag.search("AST parsing tree-sitter functions", 3));
    print_search_results("RagChunk structure metadata", rag.search("RagChunk structure metadata", 3));
    
    // Текстовые запросы
    print_search_results("transformer attention mechanism", rag.search("transformer attention mechanism", 3));
    print_search_results("retrieval augmented generation", rag.search("retrieval augmented generation", 3));
    
    // Русские запросы
    print_search_results("машинное обучение нейронные сети", rag.search("машинное обучение нейронные сети", 3));
    
    // Кросс-языковой запрос
    print_search_results("vector search embedding", rag.search("vector search embedding", 3));
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "  Готово!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}

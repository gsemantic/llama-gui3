# RAG System Documentation

## Обзор

RAG (Retrieval-Augmented Generation) система для индексации и поиска по документам и коду. Поддерживает мультиязычные запросы (русский, английский) и гибридный поиск (BM25 + векторный).

## Архитектура

```
┌─────────────────────────────────────────────────────────────┐
│                    RAG System                               │
├─────────────────────────────────────────────────────────────┤
│  Document Parser                                            │
│  ├── Text: parse_txt(), parse_md(), parse_pdf(), parse_docx()│
│  └── Code: AstParser (tree-sitter)                          │
│       ├── C/C++, Python, Rust                               │
│       └── AST chunking по функциям/классам                  │
├─────────────────────────────────────────────────────────────┤
│  Chunking                                                   │
│  ├── Text: split_into_chunks() (по предложениям)            │
│  └── Code: split_large_node() (по блокам кода)              │
├─────────────────────────────────────────────────────────────┤
│  Embeddings                                                 │
│  ├── granite-embedding-107m-multilingual (384 dim)          │
│  ├── llama-server /v1/embeddings endpoint                   │
│  └── Truncation >1500 chars                                 │
├─────────────────────────────────────────────────────────────┤
│  Search                                                     │
│  ├── BM25 (ключевые слова)                                  │
│  ├── Vector (косинусное сходство)                           │
│  └── Hybrid (BM25 40% + Vector 60%)                         │
└─────────────────────────────────────────────────────────────┘
```

## Требования

### Зависимости
- **libtree-sitter-dev** — AST парсинг кода
- **libcurl-dev** — HTTP запросы к серверу эмбеддингов
- **llama.cpp server** — для генерации эмбеддингов

### Модель эмбеддингов
- **granite-embedding-107m-multilingual** (117 MB)
- 384 измерения, 38 языков
- Лимит: 512 токенов (~1500 символов)

## Установка

### 1. Установка зависимостей

```bash
# Ubuntu/Debian
sudo apt install libtree-sitter-dev libcurl4-openssl-dev

# macOS
brew install tree-sitter curl
```

### 2. Запуск сервера эмбеддингов

```bash
# Скачать модель
wget https://huggingface.co/ibm-granite/granite-embedding-107m-multilingual/resolve/main/granite-embedding-107m-multilingual-Q4_K_M.gguf

# Запустить llama-server
llama-server \
    -m granite-embedding-107m-multilingual-Q4_K_M.gguf \
    --port 8081 \
    --embedding \
    -c 2048
```

### 3. Компиляция

```bash
g++ -std=c++17 -O2 -DUSE_CURL \
    -I../../include/core \
    -I/usr/include/x86_64-linux-gnu \
    -o test_rag_full_system test_rag_full_system.cpp \
    ../../build/src/core/libcore.a \
    ../../build/libtree-sitter-grammar-*.a \
    -ltree-sitter -lcurl
```

## Использование

### Базовый пример

```cpp
#include "rag_manager.h"
#include "document_parser.h"
#include "embedding_generator.h"
#include "ast_parser.h"

using namespace llama_gui::core;

int main() {
    // Создание RAG системы
    RAGConfig config;
    config.embedding_server = "http://localhost:8081";
    config.max_tokens_per_chunk = 200;
    
    RAGSystem rag(config);
    
    // Индексация файлов
    rag.index_file("path/to/document.txt");
    rag.index_file("path/to/code.cpp");
    
    // Поиск
    auto results = rag.search("ваш запрос", 5);
    
    for (const auto& result : results) {
        std::cout << result.score << " " << result.content << std::endl;
    }
    
    return 0;
}
```

### API Reference

#### RAGConfig

```cpp
struct RAGConfig {
    int max_tokens_per_chunk = 200;      // Размер чанка в токенах
    int embedding_max_chars = 1500;      // Лимит символов для эмбеддинга
    float bm25_weight = 0.4f;            // Вес BM25 в гибридном поиске
    float vector_weight = 0.6f;          // Вес vector в гибридном поиске
    std::string embedding_server = "http://localhost:8081";
};
```

#### RAGSystem

```cpp
class RAGSystem {
public:
    RAGSystem(const RAGConfig& config);
    
    // Индексация
    int index_file(const std::string& file_path);
    int index_directory(const std::string& dir_path, 
                        const std::vector<std::string>& extensions = {});
    
    // Поиск
    std::vector<SearchResult> search(const std::string& query, int top_k = 5);
    
    // Статистика
    void print_stats() const;
    size_t get_chunks_count() const;
};
```

#### SearchResult

```cpp
struct SearchResult {
    float score;              // Гибридный скор (0-1)
    std::string content;      // Текст чанка
    std::string file_path;    // Путь к файлу
    std::string chunk_type;   // "code", "text", "preamble"
    int chunk_index;          // Индекс чанка
};
```

## Поддерживаемые форматы

### Текст
| Формат | Расширения | Описание |
|--------|------------|----------|
| TXT | .txt | Обычный текст |
| Markdown | .md | Markdown документы |
| PDF | .pdf | PDF документы |
| DOCX | .docx | Microsoft Word |

### Код
| Язык | Расширения | Парсер |
|------|------------|--------|
| C | .c | tree-sitter-c |
| C++ | .cpp, .cc, .h, .hpp | tree-sitter-cpp |
| Python | .py | tree-sitter-python |
| Rust | .rs | tree-sitter-rust |

## Оптимальные настройки

| Параметр | Значение | Описание |
|----------|----------|----------|
| max_tokens_per_chunk | 200 | Баланс размера и качества |
| bm25_weight | 0.4 | Ключевые слова важны |
| vector_weight | 0.6 | Семантика важнее |
| embedding_server | localhost:8081 | Порт llama-server |
| embedding_model | granite-107m | Мультиязычный |

## Производительность

| Операция | Скорость | Зависимости |
|----------|----------|-------------|
| Индексация кода | 46 чанков/сек | CPU |
| Индексация текста | 67 чанков/сек | CPU |
| Эмбеддинги (CPU) | 3-8 emb/sec | llama-server |
| Поиск | 1-2 мс | brute-force |

## Примеры запросов

### Код
```
"AST parsing tree-sitter functions"
"RagChunk structure metadata fields"
"document parsing code files language"
```

### Текст (английский)
```
"transformer attention mechanism"
"retrieval augmented generation"
"training language models"
```

### Текст (русский)
```
"машинное обучение нейронные сети"
"применения ML в бизнесе"
"векторный поиск по документам"
```

## Архитектурные решения

### 1. AST Chunking
- Использует tree-sitter для точного парсинга кода
- Извлекает функции, классы, структуры, enum
- Рекурсивное разбиение больших блоков (>200 токенов)
- Метаданные: symbol_name, parent_name, start_line, end_line

### 2. Text Chunking
- Разбиение по границам предложений
- Учёт русских знаков препинания (точка, восклицательный, вопросительный)
- Максимальный размер: 200 токенов

### 3. Гибридный поиск
- BM25 (40%): точное совпадение ключевых слов
- Vector (60%): семантическое сходство
- Нормализация скоров перед комбинированием

### 4. Truncation
- Автоматическая обрезка текста >1500 символов
- Обрезка по границе предложения
- Предотвращает HTTP 500 ошибки

## Ограничения

1. **Размер модели**: 512 токенов max (granite-embedding)
2. **Скорость**: 3-8 emb/sec на CPU (нужен GPU для ускорения)
3. **Поиск**: brute-force (нужен FAISS для масштабирования)
4. **Языки**: 4 языка через tree-sitter (C, C++, Python, Rust)

## Расширение

### Добавление нового языка
1. Скачать tree-sitter grammar: `git clone https://github.com/tree-sitter/tree-sitter-{lang}`
2. Добавить в CMakeLists.txt
3. Добавить extern declaration в ast_parser.cpp
4. Добавить в language_grammars map

### Добавление нового формата
1. Реализовать парсер в document_parser.cpp
2. Добавить расширение в get_language()
3. Добавить в поддерживаемые форматы

## Лицензия

Apache 2.0 (как granite-embedding модель)

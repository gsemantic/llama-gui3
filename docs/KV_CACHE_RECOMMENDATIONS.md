# Рекомендации по внедрению KV-cache persistence в llama-gui

## Обзор

В версии **llama.cpp b7472** уже реализована полная поддержка сохранения и загрузки KV-cache через HTTP API. Этот документ описывает шаги по интеграции этой функциональности в llama-gui для улучшения работы RAG-системы.

---

## 1. Архитектурное решение

### Текущая архитектура
```
┌─────────────┐     HTTP API      ┌──────────────────────────┐
│  llama-gui  │ ─────────────────> │ llama.cpp server (b7472) │
│  (Qt + RAG) │   /completion     │                          │
│             │   /embedding      │  - Внутренний KV-cache   │
│             │                   │  - Освобождается после   │
└─────────────┘                   └──────────────────────────┘
```

### Целевая архитектура с KV-cache persistence
```
┌─────────────┐     HTTP API      ┌──────────────────────────┐
│  llama-gui  │ ─────────────────> │ llama.cpp server (b7472) │
│  (Qt + RAG) │   /completion     │                          │
│             │   /embedding      │  - KV-cache persistence  │
│             │   /slots/{id}     │  - Slot management       │
│             │   ?action=save    │  - Автоматический reuse  │
│             │   ?action=restore │                          │
└─────────────┘                   └──────────────────────────┘
```

---

## 2. Параметры запуска сервера

### Необходимые флаги для llama.cpp server

```bash
# Основной параметр: путь для сохранения KV-cache слотов
--slot-save-path /path/to/cache/

# Количество параллельных слотов (для одновременной работы с документами)
-np, --parallel 4

# Тип данных для KV-cache (экономия памяти)
-ctk, --cache-type-k q8_0    # K cache: f32/f16/bf16/q8_0/q4_0/...
-ctv, --cache-type-v q8_0    # V cache: f32/f16/bf16/q8_0/q4_0/...

# Автоматическое переиспользование KV-cache
--cache-reuse N              # min chunk size для reuse (default: 0)

# Включить endpoints слотов (включено по умолчанию)
--slots
```

### Таблица размеров KV-cache (на 1 токен)

| Модель | ctx=4096 | ctx=8192 | ctx=16384 |
|--------|----------|----------|-----------|
| 7B (f16) | 8 МБ | 16 МБ | 32 МБ |
| 7B (q8_0) | 4 МБ | 8 МБ | 16 МБ |
| 7B (q4_0) | 2 МБ | 4 МБ | 8 МБ |
| 70B (f16) | 64 МБ | 128 МБ | 256 МБ |
| 70B (q8_0) | 32 МБ | 64 МБ | 128 МБ |

---

## 3. HTTP API Endpoints

### 3.1 GET `/slots` — Получить статус всех слотов

**Запрос:**
```bash
curl http://localhost:8080/slots
```

**Ответ:**
```json
{
    "slots": [
        {
            "id": 0,
            "state": "idle",
            "num_prompt_tokens": 1745,
            "num_infered_tokens": 512,
            "last_activity": 1234567890
        },
        {
            "id": 1,
            "state": "processing",
            ...
        }
    ]
}
```

---

### 3.2 POST `/slots/{id_slot}?action=save` — Сохранить KV-cache

**Запрос:**
```bash
curl -X POST "http://localhost:8080/slots/0?action=save" \
  -H "Content-Type: application/json" \
  -d '{"filename": "document_context.bin"}'
```

**Параметры:**
| Параметр | Тип | Описание |
|----------|-----|----------|
| `filename` | string | Имя файла для сохранения (обязательно) |

**Ответ (успех):**
```json
{
    "id_slot": 0,
    "filename": "document_context.bin",
    "n_saved": 1745,
    "n_written": 14309796,
    "timings": {
        "prompt_n": 1745,
        "prompt_ms": 245.5
    }
}
```

**Ответ (ошибка):**
```json
{
    "error": {
        "code": 400,
        "message": "Slot is busy",
        "type": "invalid_request_error"
    }
}
```

---

### 3.3 POST `/slots/{id_slot}?action=restore` — Восстановить KV-cache

**Запрос:**
```bash
curl -X POST "http://localhost:8080/slots/0?action=restore" \
  -H "Content-Type: application/json" \
  -d '{"filename": "document_context.bin"}'
```

**Параметры:**
| Параметр | Тип | Описание |
|----------|-----|----------|
| `filename` | string | Имя файла для восстановления (обязательно) |

**Ответ (успех):**
```json
{
    "id_slot": 0,
    "filename": "document_context.bin",
    "n_restored": 1745,
    "n_read": 14309796,
    "timings": {
        "prompt_n": 1745,
        "prompt_ms": 189.2
    }
}
```

---

### 3.4 POST `/slots/{id_slot}?action=reset` — Сбросить слот

**Запрос:**
```bash
curl -X POST "http://localhost:8080/slots/0?action=reset"
```

---

### 3.5 POST `/slots/{id_slot}?action=erase` — Удалить KV-cache слота

**Запрос:**
```bash
curl -X POST "http://localhost:8080/slots/0?action=erase"
```

---

## 4. Изменения в коде llama-gui

### 4.1 Обновление настроек сервера

**Файл:** `include/core/settings_server_runtime.h`

```cpp
#pragma once

#include <string>

namespace llama_gui {
namespace core {

struct ServerRuntimeSettings {
    // ... существующие поля ...
    
    // === KV-cache persistence ===
    std::string slot_save_path = "";        // Путь для сохранения KV-cache
    int n_parallel = 4;                     // Количество слотов
    std::string cache_type_k = "q8_0";      // Тип K-cache (f32/f16/bf16/q8_0/q4_0)
    std::string cache_type_v = "q8_0";      // Тип V-cache
    int cache_reuse = 0;                    // Min chunk size для reuse
    bool enable_slots_endpoint = true;      // Включить /slots endpoint
    
    // Сериализация
    void to_json(nlohmann::json& j) const;
    static void from_json(const nlohmann::json& j, ServerRuntimeSettings& s);
};

} // namespace core
} // namespace llama_gui
```

---

### 4.2 Обновление LlamaInterface

**Файл:** `include/core/llama_interface.h`

```cpp
// Добавить в класс LlamaInterface:

/**
 * @brief Получить статус всех слотов
 * @return JSON со статусом слотов
 */
json get_slots_status() const;

/**
 * @brief Сохранить KV-cache указанного слота в файл
 * @param slot_id ID слота (0..n_parallel-1)
 * @param filename Имя файла для сохранения
 * @return true если успешно
 */
bool save_slot_kv_cache(int slot_id, const std::string& filename);

/**
 * @brief Восстановить KV-cache указанного слота из файла
 * @param slot_id ID слота (0..n_parallel-1)
 * @param filename Имя файла для восстановления
 * @return true если успешно
 */
bool restore_slot_kv_cache(int slot_id, const std::string& filename);

/**
 * @brief Сбросить слот
 * @param slot_id ID слота
 * @return true если успешно
 */
bool reset_slot(int slot_id);

/**
 * @brief Удалить KV-cache слота
 * @param slot_id ID слота
 * @return true если успешно
 */
bool erase_slot(int slot_id);

/**
 * @brief Структура результата операции с слотом
 */
struct SlotOperationResult {
    bool success;
    int slot_id;
    std::string filename;
    int n_tokens;           // n_saved / n_restored
    int n_bytes;            // n_written / n_read
    double processing_ms;
    std::string error_message;
};

/**
 * @brief Сохранить KV-cache с подробным результатом
 * @return SlotOperationResult с деталями операции
 */
SlotOperationResult save_slot_kv_cache_detailed(int slot_id, const std::string& filename);

/**
 * @brief Восстановить KV-cache с подробным результатом
 * @return SlotOperationResult с деталями операции
 */
SlotOperationResult restore_slot_kv_cache_detailed(int slot_id, const std::string& filename);
```

---

### 4.3 Реализация в llama_interface_http.cpp

**Файл:** `src/core/llama_interface_http.cpp`

```cpp
#include "../include/core/llama_interface.h"
#include "../include/core/llama_interface_impl.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace llama_gui {
namespace core {

// === GET /slots ===
json LlamaInterface::get_slots_status() const {
    try {
        std::string response = make_http_request("/slots", "GET", json{});
        return json::parse(response);
    } catch (const std::exception& e) {
        last_error_ = std::string("Failed to get slots status: ") + e.what();
        return json{{"error", last_error_}};
    }
}

// === POST /slots/{id_slot}?action=save ===
bool LlamaInterface::save_slot_kv_cache(int slot_id, const std::string& filename) {
    auto result = save_slot_kv_cache_detailed(slot_id, filename);
    return result.success;
}

LlamaInterface::SlotOperationResult LlamaInterface::save_slot_kv_cache_detailed(
    int slot_id, const std::string& filename) 
{
    SlotOperationResult result;
    result.slot_id = slot_id;
    result.filename = filename;
    result.success = false;
    
    try {
        json request_data = {{"filename", filename}};
        std::string endpoint = "/slots/" + std::to_string(slot_id) + "?action=save";
        
        std::string response_str = make_http_request(endpoint, "POST", request_data);
        json response = json::parse(response_str);
        
        if (response.contains("error")) {
            result.error_message = response["error"].dump();
            return result;
        }
        
        result.success = true;
        result.n_tokens = response.value("n_saved", 0);
        result.n_bytes = response.value("n_written", 0);
        
        if (response.contains("timings")) {
            result.processing_ms = response["timings"].value("prompt_ms", 0.0);
        }
        
    } catch (const std::exception& e) {
        result.error_message = std::string("Exception: ") + e.what();
    }
    
    return result;
}

// === POST /slots/{id_slot}?action=restore ===
bool LlamaInterface::restore_slot_kv_cache(int slot_id, const std::string& filename) {
    auto result = restore_slot_kv_cache_detailed(slot_id, filename);
    return result.success;
}

LlamaInterface::SlotOperationResult LlamaInterface::restore_slot_kv_cache_detailed(
    int slot_id, const std::string& filename) 
{
    SlotOperationResult result;
    result.slot_id = slot_id;
    result.filename = filename;
    result.success = false;
    
    try {
        json request_data = {{"filename", filename}};
        std::string endpoint = "/slots/" + std::to_string(slot_id) + "?action=restore";
        
        std::string response_str = make_http_request(endpoint, "POST", request_data);
        json response = json::parse(response_str);
        
        if (response.contains("error")) {
            result.error_message = response["error"].dump();
            return result;
        }
        
        result.success = true;
        result.n_tokens = response.value("n_restored", 0);
        result.n_bytes = response.value("n_read", 0);
        
        if (response.contains("timings")) {
            result.processing_ms = response["timings"].value("prompt_ms", 0.0);
        }
        
    } catch (const std::exception& e) {
        result.error_message = std::string("Exception: ") + e.what();
    }
    
    return result;
}

// === POST /slots/{id_slot}?action=reset ===
bool LlamaInterface::reset_slot(int slot_id) {
    try {
        std::string endpoint = "/slots/" + std::to_string(slot_id) + "?action=reset";
        make_http_request(endpoint, "POST", json{});
        return true;
    } catch (const std::exception& e) {
        last_error_ = std::string("Failed to reset slot: ") + e.what();
        return false;
    }
}

// === POST /slots/{id_slot}?action=erase ===
bool LlamaInterface::erase_slot(int slot_id) {
    try {
        std::string endpoint = "/slots/" + std::to_string(slot_id) + "?action=erase";
        make_http_request(endpoint, "POST", json{});
        return true;
    } catch (const std::exception& e) {
        last_error_ = std::string("Failed to erase slot: ") + e.what();
        return false;
    }
}

} // namespace core
} // namespace llama_gui
```

---

### 4.4 Обновление RagManager

**Файл:** `include/core/rag_manager.h`

```cpp
// Добавить в класс RagManager:

// === KV-cache persistence для документов ===

/**
 * @brief Сохранить KV-cache для обработанного документа
 * @param doc_id Уникальный идентификатор документа
 * @param slot_id ID слота, в котором загружен документ
 * @return true если успешно
 */
bool save_document_kv_cache(const std::string& doc_id, int slot_id);

/**
 * @brief Загрузить KV-cache для документа в слот
 * @param doc_id Уникальный идентификатор документа
 * @param slot_id ID слота для загрузки
 * @return true если успешно
 */
bool load_document_kv_cache(const std::string& doc_id, int slot_id);

/**
 * @brief Проверить наличие сохранённого KV-cache для документа
 * @param doc_id Уникальный идентификатор документа
 * @return true если файл существует
 */
bool has_document_kv_cache(const std::string& doc_id) const;

/**
 * @brief Удалить сохранённый KV-cache документа
 * @param doc_id Уникальный идентификатор документа
 * @return true если успешно
 */
bool delete_document_kv_cache(const std::string& doc_id);

/**
 * @brief Получить путь к файлу KV-cache для документа
 * @param doc_id Уникальный идентификатор документа
 * @return Полный путь к файлу
 */
std::string get_document_kv_cache_path(const std::string& doc_id) const;

/**
 * @brief Структура информации о документе в RAG
 */
struct RagDocumentInfo {
    std::string doc_id;
    std::string file_path;
    std::string file_hash;        // Для проверки изменений
    size_t n_tokens;              // Количество токенов в документе
    time_t last_processed;        // Время последней обработки
    bool kv_cache_available;      // Есть ли сохранённый KV-cache
    std::string kv_cache_path;    // Путь к файлу KV-cache
};

/**
 * @brief Получить информацию о документе
 * @param doc_id Уникальный идентификатор документа
 * @return Информация о документе
 */
RagDocumentInfo get_document_info(const std::string& doc_id) const;

/**
 * @brief Очистить все сохранённые KV-cache
 * @param older_than_seconds Удалить файлы старше указанного времени (0 = все)
 * @return Количество удалённых файлов
 */
int cleanup_old_kv_caches(int older_than_seconds = 0);
```

---

### 4.5 Реализация в rag_manager_core.cpp

**Файл:** `src/core/rag_manager_core.cpp`

```cpp
#include "../include/core/rag_manager.h"
#include "../include/core/embedding_generator.h"
#include "../include/core/document_parser.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace llama_gui {
namespace core {

// === Вспомогательные функции ===

std::string RagManager::get_document_kv_cache_path(const std::string& doc_id) const {
    // Используем hash от doc_id для имени файла
    std::string hash = std::to_string(std::hash<std::string>{}(doc_id));
    return kv_cache_directory_ + "/" + hash + ".bin";
}

bool RagManager::has_document_kv_cache(const std::string& doc_id) const {
    std::string path = get_document_kv_cache_path(doc_id);
    std::ifstream file(path);
    return file.good();
}

bool RagManager::save_document_kv_cache(const std::string& doc_id, int slot_id) {
    std::string cache_path = get_document_kv_cache_path(doc_id);
    
    // Создаём директорию если не существует
    fs::path dir = fs::path(cache_path).parent_path();
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
    }
    
    // Извлекаем только имя файла для API
    std::string filename = fs::path(cache_path).filename().string();
    
    auto result = llama_interface_->save_slot_kv_cache_detailed(slot_id, filename);
    
    if (result.success) {
        std::cout << "[RAG] KV-cache saved for document " << doc_id 
                  << " (" << result.n_tokens << " tokens, " 
                  << result.n_bytes << " bytes)" << std::endl;
        return true;
    } else {
        std::cerr << "[RAG] Failed to save KV-cache for document " << doc_id 
                  << ": " << result.error_message << std::endl;
        return false;
    }
}

bool RagManager::load_document_kv_cache(const std::string& doc_id, int slot_id) {
    if (!has_document_kv_cache(doc_id)) {
        std::cerr << "[RAG] No KV-cache found for document " << doc_id << std::endl;
        return false;
    }
    
    std::string cache_path = get_document_kv_cache_path(doc_id);
    std::string filename = fs::path(cache_path).filename().string();
    
    auto result = llama_interface_->restore_slot_kv_cache_detailed(slot_id, filename);
    
    if (result.success) {
        std::cout << "[RAG] KV-cache loaded for document " << doc_id 
                  << " (" << result.n_tokens << " tokens, " 
                  << result.n_bytes << " bytes, " 
                  << result.processing_ms << " ms)" << std::endl;
        return true;
    } else {
        std::cerr << "[RAG] Failed to load KV-cache for document " << doc_id 
                  << ": " << result.error_message << std::endl;
        return false;
    }
}

bool RagManager::delete_document_kv_cache(const std::string& doc_id) {
    std::string path = get_document_kv_cache_path(doc_id);
    
    try {
        if (fs::exists(path)) {
            fs::remove(path);
            std::cout << "[RAG] Deleted KV-cache for document " << doc_id << std::endl;
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[RAG] Error deleting KV-cache: " << e.what() << std::endl;
        return false;
    }
}

int RagManager::cleanup_old_kv_caches(int older_than_seconds) {
    int deleted_count = 0;
    std::string cache_dir = kv_cache_directory_;
    
    if (!fs::exists(cache_dir)) {
        return 0;
    }
    
    auto now = std::time(nullptr);
    
    for (const auto& entry : fs::directory_iterator(cache_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".bin") {
            auto file_time = fs::last_write_time(entry);
            auto file_age = std::difftime(now, std::time(file_time));
            
            if (older_than_seconds == 0 || file_age > older_than_seconds) {
                try {
                    fs::remove(entry.path());
                    deleted_count++;
                } catch (const std::exception& e) {
                    std::cerr << "[RAG] Error deleting file " << entry.path() 
                              << ": " << e.what() << std::endl;
                }
            }
        }
    }
    
    std::cout << "[RAG] Cleaned up " << deleted_count << " KV-cache files" << std::endl;
    return deleted_count;
}

RagManager::RagDocumentInfo RagManager::get_document_info(const std::string& doc_id) const {
    RagDocumentInfo info;
    info.doc_id = doc_id;
    info.kv_cache_available = has_document_kv_cache(doc_id);
    info.kv_cache_path = get_document_kv_cache_path(doc_id);
    
    // Здесь можно добавить получение дополнительной информации
    // из внутреннего хранилища документов
    
    return info;
}

} // namespace core
} // namespace llama_gui
```

---

### 4.6 Обновление UI для управления KV-cache

**Файл:** `src/ui/settings_dialog_context.cpp` (существующий)

```cpp
// Добавить отображение информации о KV-cache:

void SettingsDialogContext::render_kv_cache_section() {
    ImGui::Separator();
    ImGui::Text("KV-Cache Persistence");
    ImGui::Spacing();
    
    // Путь для сохранения
    char path_buf[512];
    std::strncpy(path_buf, settings_.server_runtime.slot_save_path.c_str(), sizeof(path_buf));
    if (ImGui::InputText("Slot Save Path", path_buf, sizeof(path_buf))) {
        settings_.server_runtime.slot_save_path = path_buf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        // Открыть диалог выбора директории
    }
    
    // Количество слотов
    int n_parallel = settings_.server_runtime.n_parallel;
    if (ImGui::SliderInt("Parallel Slots", &n_parallel, 1, 16)) {
        settings_.server_runtime.n_parallel = n_parallel;
    }
    
    // Тип квантования KV-cache
    const char* cache_types[] = {"f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl"};
    int current_k = get_cache_type_index(settings_.server_runtime.cache_type_k);
    if (ImGui::Combo("K-Cache Type", &current_k, cache_types, IM_ARRAYSIZE(cache_types))) {
        settings_.server_runtime.cache_type_k = cache_types[current_k];
    }
    
    int current_v = get_cache_type_index(settings_.server_runtime.cache_type_v);
    if (ImGui::Combo("V-Cache Type", &current_v, cache_types, IM_ARRAYSIZE(cache_types))) {
        settings_.server_runtime.cache_type_v = cache_types[current_v];
    }
    
    // Cache reuse
    int cache_reuse = settings_.server_runtime.cache_reuse;
    if (ImGui::SliderInt("Cache Reuse (min tokens)", &cache_reuse, 0, 4096)) {
        settings_.server_runtime.cache_reuse = cache_reuse;
    }
    
    // Информация об использовании
    ImGui::Separator();
    ImGui::Text("KV-Cache Storage");
    
    size_t total_size = get_kv_cache_storage_size();
    int file_count = get_kv_cache_file_count();
    
    ImGui::BulletText("Stored documents: %d", file_count);
    ImGui::BulletText("Total size: %.2f MB", total_size / 1024.0 / 1024.0);
    
    ImGui::Spacing();
    if (ImGui::Button("Clean Up Old Caches")) {
        cleanup_old_kv_caches(7 * 24 * 3600); // Удалить старше 7 дней
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear All Caches")) {
        cleanup_old_kv_caches(0); // Удалить все
    }
}

int SettingsDialogContext::get_cache_type_index(const std::string& type) {
    static const std::vector<std::string> types = {"f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl"};
    for (size_t i = 0; i < types.size(); i++) {
        if (types[i] == type) return static_cast<int>(i);
    }
    return 3; // default q8_0
}
```

---

## 5. Сценарии использования

### 5.1 RAG с персистентным KV-cache

```cpp
// 1. Обработка нового документа
void RagManager::process_document(const std::string& file_path) {
    std::string doc_id = generate_doc_id(file_path);
    std::string doc_hash = compute_file_hash(file_path);
    
    // Проверяем наличие KV-cache
    if (has_document_kv_cache(doc_id)) {
        std::cout << "[RAG] Document already processed, KV-cache available" << std::endl;
        return;
    }
    
    // Выбираем свободный слот
    int slot_id = find_free_slot();
    
    // Загружаем документ в слот
    std::string content = read_document(file_path);
    send_to_slot(slot_id, content);
    
    // Сохраняем KV-cache
    save_document_kv_cache(doc_id, slot_id);
    
    // Освобождаем слот
    reset_slot(slot_id);
}

// 2. Поиск с использованием KV-cache
std::string RagManager::query(const std::string& user_query) {
    // FAISS поиск релевантных документов
    auto relevant_docs = search_documents(user_query, k=5);
    
    // Выбираем слот
    int slot_id = find_free_slot();
    
    // Для первого документа восстанавливаем KV-cache
    if (!relevant_docs.empty() && has_document_kv_cache(relevant_docs[0].id)) {
        load_document_kv_cache(relevant_docs[0].id, slot_id);
        
        // Добавляем только запрос (KV-cache уже содержит документ)
        send_query_to_slot(slot_id, user_query);
    } else {
        // Стандартный путь: отправляем документ + запрос
        send_full_context_to_slot(slot_id, relevant_docs, user_query);
    }
    
    // Получаем ответ
    std::string response = get_completion(slot_id);
    
    // Освобождаем слот
    reset_slot(slot_id);
    
    return response;
}
```

### 5.2 Диалог с сохранением контекста

```cpp
// Сохранение контекста диалога между сессиями
void MainWindow::save_conversation_kv_cache(const std::string& conversation_id) {
    int slot_id = 0; // Основной слот для диалога
    std::string filename = "conversation_" + conversation_id + ".bin";
    
    llama_interface_.save_slot_kv_cache(slot_id, filename);
}

void MainWindow::load_conversation_kv_cache(const std::string& conversation_id) {
    int slot_id = 0;
    std::string filename = "conversation_" + conversation_id + ".bin";
    
    if (llama_interface_.restore_slot_kv_cache(slot_id, filename)) {
        std::cout << "Conversation context restored!" << std::endl;
    }
}
```

---

## 6. Оптимизации и лучшие практики

### 6.1 Выбор типа квантования KV-cache

| Тип | Размер | Точность | Рекомендация |
|-----|--------|----------|--------------|
| f16 | 100% | 100% | Только для отладки |
| q8_0 | 50% | ~99.9% | **Рекомендуется по умолчанию** |
| q4_0 | 25% | ~99% | Для ограниченной памяти |
| q4_1 | 25% | ~99.5% | Компромиссный вариант |

### 6.2 Управление памятью

```cpp
// Рекомендуемые настройки для разных сценариев

// Desktop с 16GB RAM:
cache_type_k = "q8_0"
cache_type_v = "q8_0"
n_parallel = 4
cache_reuse = 256

// Desktop с 32GB+ RAM:
cache_type_k = "f16"
cache_type_v = "f16"
n_parallel = 8
cache_reuse = 512

// Ограниченная память (8GB):
cache_type_k = "q4_0"
cache_type_v = "q4_0"
n_parallel = 2
cache_reuse = 128
```

### 6.3 Инвалидация KV-cache

```cpp
// Проверяем изменения в документе перед использованием KV-cache
bool RagManager::is_kv_cache_valid(const std::string& doc_id) {
    std::string current_hash = compute_file_hash(get_document_path(doc_id));
    std::string stored_hash = get_stored_document_hash(doc_id);
    
    return current_hash == stored_hash;
}

// При обновлении документа удаляем старый KV-cache
void RagManager::on_document_updated(const std::string& doc_id) {
    delete_document_kv_cache(doc_id);
    // Помечаем для повторной обработки
    mark_for_reprocessing(doc_id);
}
```

---

## 7. Тестирование

### 7.1 Unit тесты

```cpp
// tests/test_kv_cache_persistence.cpp

TEST(KVCachePersistence, SaveAndRestore) {
    auto llama_interface = std::make_unique<LlamaInterface>("http://localhost:8080");
    
    // Отправляем текст в слот
    std::string test_text = "This is a test document for KV-cache persistence.";
    llama_interface->send_to_slot(0, test_text);
    
    // Сохраняем KV-cache
    auto save_result = llama_interface->save_slot_kv_cache_detailed(0, "test.bin");
    ASSERT_TRUE(save_result.success);
    ASSERT_GT(save_result.n_tokens, 0);
    
    // Сбрасываем слот
    llama_interface->reset_slot(0);
    
    // Восстанавливаем KV-cache
    auto restore_result = llama_interface->restore_slot_kv_cache_detailed(0, "test.bin");
    ASSERT_TRUE(restore_result.success);
    ASSERT_EQ(restore_result.n_tokens, save_result.n_tokens);
    
    // Очищаем
    std::remove("test.bin");
}

TEST(KVCachePersistence, RagManagerIntegration) {
    RagManager rag_manager("models/embedding.gguf");
    
    // Обрабатываем документ
    std::string doc_path = "test_docs/sample.txt";
    rag_manager.process_document(doc_path);
    
    // Проверяем наличие KV-cache
    std::string doc_id = rag_manager.get_doc_id(doc_path);
    ASSERT_TRUE(rag_manager.has_document_kv_cache(doc_id));
    
    // Загружаем KV-cache
    ASSERT_TRUE(rag_manager.load_document_kv_cache(doc_id, 0));
    
    // Очищаем
    rag_manager.delete_document_kv_cache(doc_id);
}
```

### 7.2 Интеграционные тесты

```bash
# Запуск сервера с поддержкой KV-cache persistence
./llama-server \
    -m models/llama-7b.gguf \
    --slot-save-path /tmp/kv-cache \
    -np 4 \
    -ctk q8_0 -ctv q8_0 \
    --cache-reuse 256 \
    --port 8080

# Тестирование через curl
# 1. Отправляем документ
curl -X POST http://localhost:8080/completion \
    -H "Content-Type: application/json" \
    -d '{"prompt": "Document content here...", "n_predict": 0}'

# 2. Сохраняем KV-cache
curl -X POST "http://localhost:8080/slots/0?action=save" \
    -H "Content-Type: application/json" \
    -d '{"filename": "doc_test.bin"}'

# 3. Восстанавливаем KV-cache
curl -X POST "http://localhost:8080/slots/0?action=restore" \
    -H "Content-Type: application/json" \
    -d '{"filename": "doc_test.bin"}'

# 4. Проверяем статус
curl http://localhost:8080/slots
```

---

## 8. Диагностика и отладка

### 8.1 Логирование

```cpp
// Включаем подробное логирование операций с KV-cache
#define KV_CACHE_DEBUG 1

#if KV_CACHE_DEBUG
    #define KV_LOG(msg) std::cout << "[KV-CACHE] " << msg << std::endl
#else
    #define KV_LOG(msg)
#endif

// Пример использования
KV_LOG("Saving KV-cache for slot " << slot_id << " to " << filename);
```

### 8.2 Метрики для мониторинга

```cpp
struct KVCacheMetrics {
    size_t total_saves;
    size_t total_restores;
    size_t total_bytes_written;
    size_t total_bytes_read;
    double avg_save_time_ms;
    double avg_restore_time_ms;
    size_t cache_hits;
    size_t cache_misses;
    
    float hit_rate() const {
        size_t total = cache_hits + cache_misses;
        return total > 0 ? static_cast<float>(cache_hits) / total : 0.0f;
    }
};
```

### 8.3 Типичные проблемы и решения

| Проблема | Причина | Решение |
|----------|---------|---------|
| Slot is busy | Слот занят обработкой | Использовать `reset_slot()` или ждать |
| File not found | Файл KV-cache удалён | Проверить `has_document_kv_cache()` перед загрузкой |
| Hash mismatch | Документ изменён | Инвалидировать KV-cache при изменении |
| Out of memory | Слишком много KV-cache | Очистить старые файлы (`cleanup_old_kv_caches`) |
| Slow restore | Большой файл KV-cache | Использовать квантование (q8_0/q4_0) |

---

## 9. Оценка производительности

### 9.1 Ожидаемое ускорение

| Сценарий | Без KV-cache | С KV-cache | Ускорение |
|----------|--------------|------------|-----------|
| Первый запрос к документу (10K токенов) | 5-10 сек | 5-10 сек | 1x (без изменений) |
| Повторный запрос к документу | 5-10 сек | 50-200 мс | **25-100x** |
| Запрос к другому документу | 5-10 сек | 5-10 сек | 1x |
| Диалог с контекстом (1K токенов) | 1-2 сек | 100-300 мс | **5-10x** |

### 9.2 Использование памяти

```
7B модель, ctx=4096, q8_0:
- Один документ (10K токенов): ~80 МБ
- 10 документов: ~800 МБ
- 100 документов: ~8 ГБ

Рекомендация: Хранить в памяти только активные документы,
остальные удалять и восстанавливать по требованию.
```

---

## 10. Чеклист внедрения

- [ ] Обновить `settings_server_runtime.h` с параметрами KV-cache
- [ ] Добавить методы в `llama_interface.h` для работы с слотами
- [ ] Реализовать методы в `llama_interface_http.cpp`
- [ ] Обновить `rag_manager.h` с методами сохранения/загрузки
- [ ] Реализовать методы в `rag_manager_core.cpp`
- [ ] Добавить UI для управления KV-cache в `settings_dialog_context.cpp`
- [ ] Добавить логирование операций с KV-cache
- [ ] Написать unit тесты
- [ ] Провести интеграционное тестирование
- [ ] Обновить документацию пользователя
- [ ] Протестировать на различных моделях (7B, 70B)
- [ ] Оптимизировать настройки для разных конфигураций RAM

---

## 11. Дополнительные ресурсы

- [llama.cpp server README](https://github.com/ggml-org/llama.cpp/tree/master/tools/server)
- [llama.h API документация](https://github.com/ggml-org/llama.cpp/blob/master/include/llama.h)
- [Issue #9291: Server changelog](https://github.com/ggml-org/llama.cpp/issues/9291)
- [PR #15293: Context checkpoints](https://github.com/ggml-org/llama.cpp/pull/15293)
- [PR #16391: Cache RAM limit](https://github.com/ggml-org/llama.cpp/pull/16391)

---

## 12. Контакты и поддержка

Вопросы и предложения по улучшению этого документа направляйте в:
- GitHub Issues проекта llama-gui
- Discord канал llama.cpp

---

*Документ создан: 2026-03-07*
*Версия llama.cpp: b7472*
*Версия документа: 1.0*

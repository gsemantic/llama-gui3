#pragma once

/**
 * @file rag_agent.h
 * @brief RAG-агент для поиска по документам
 * 
 * Этот агент является обёрткой вокруг существующего RagManager
 * и предоставляет унифицированный интерфейс для поиска контекста.
 */

#include <agents/agents.h>
#include <string>
#include <vector>
#include <memory>

// Forward declaration существующего RagManager
namespace core {
    class RagManager;
}

namespace agents {

/**
 * @brief Результат поиска RAG
 */
struct RagSearchResult {
    std::string content;
    std::string document_path;
    int chunk_index;
    float similarity_score;
};

/**
 * @brief Агент для RAG-поиска по документам
 * 
 * Интегрируется с существующим RagManager проекта.
 * Поддерживает:
 * - Поиск по внешним документам
 * - Семантический поиск через эмбеддинги
 * - Кэширование результатов
 */
class RagAgent : public IAgent {
public:
    RagAgent();
    ~RagAgent() override;

    /**
     * @brief Имя агента
     */
    const char* name() const override;

    /**
     * @brief Описание агента
     */
    const char* description() const override;

    /**
     * @brief Версия агента
     */
    const char* version() const override;

    /**
     * @brief Инициализация агента
     * @param context Контекст выполнения
     * @return true если успешно
     */
    bool initialize(AgentContext* context) override;

    /**
     * @brief Выполнение запроса
     * 
     * Поддерживаемые действия:
     * - "search" - поиск по документам
     * - "add_document" - добавление документа
     * - "list_documents" - список документов
     * - "clear" - очистка кэша
     * - "stats" - статистика
     * 
     * @param request Запрос
     * @return Результат выполнения
     */
    AgentResult execute(const AgentRequest& request) override;

    /**
     * @brief Остановка агента
     */
    void shutdown() override;

    /**
     * @brief Возможности агента
     */
    AgentCapability capabilities() const override;

    /**
     * @brief Проверка готовности
     */
    bool is_ready() const override;

    /**
     * @brief Установка RagManager
     * @param manager Указатель на RagManager
     */
    void set_rag_manager(core::RagManager* manager);

private:
    /**
     * @brief Обработка действия "search"
     */
    AgentResult handle_search(const AgentRequest& request);

    /**
     * @brief Обработка действия "add_document"
     */
    AgentResult handle_add_document(const AgentRequest& request);

    /**
     * @brief Обработка действия "list_documents"
     */
    AgentResult handle_list_documents(const AgentRequest& request);

    /**
     * @brief Обработка действия "clear"
     */
    AgentResult handle_clear(const AgentRequest& request);

    /**
     * @brief Обработка действия "stats"
     */
    AgentResult handle_stats(const AgentRequest& request);

    AgentContext* context_ = nullptr;
    core::RagManager* rag_manager_ = nullptr;
    bool initialized_ = false;
    
    // Настройки
    int max_chunks_ = 10;
    float similarity_threshold_ = 0.7f;
    bool cache_enabled_ = true;
};

} // namespace agents

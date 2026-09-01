#pragma once

/**
 * @file file_agent.h
 * @brief Агент для работы с файловой системой
 * 
 * Предоставляет безопасный доступ к файлам с проверкой разрешений.
 */

#include <agents/agents.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <memory>

namespace agents {

/**
 * @brief Агент для работы с файлами
 * 
 * Поддерживаемые действия:
 * - read - чтение файла
 * - write - запись файла
 * - append - добавление в файл
 * - delete - удаление файла
 * - exists - проверка существования
 * - list - список файлов в директории
 * - copy - копирование файла
 * - move - перемещение файла
 * - info - информация о файле
 */
class FileAgent : public IAgent {
public:
    FileAgent();
    ~FileAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(AgentContext* context) override;
    AgentResult execute(const AgentRequest& request) override;
    void shutdown() override;
    AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    // Обработчики действий
    AgentResult handle_read(const AgentRequest& request);
    AgentResult handle_write(const AgentRequest& request);
    AgentResult handle_append(const AgentRequest& request);
    AgentResult handle_delete(const AgentRequest& request);
    AgentResult handle_exists(const AgentRequest& request);
    AgentResult handle_list(const AgentRequest& request);
    AgentResult handle_copy(const AgentRequest& request);
    AgentResult handle_move(const AgentRequest& request);
    AgentResult handle_info(const AgentRequest& request);

    /**
     * @brief Проверка расширения файла
     */
    bool is_extension_allowed(const std::string& path) const;

    /**
     * @brief Проверка размера файла
     */
    bool is_file_size_allowed(const std::string& path) const;

    /**
     * @brief Нормализация пути
     */
    std::string normalize_path(const std::string& path) const;

    AgentContext* context_ = nullptr;
    bool initialized_ = false;

    // Настройки
    std::unordered_set<std::string> allowed_extensions_;
    size_t max_file_size_mb_ = 10;
    std::string base_dir_;  // Базовая директория для операций
};

} // namespace agents

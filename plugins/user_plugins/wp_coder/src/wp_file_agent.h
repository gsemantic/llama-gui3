#pragma once

#include <agents/agents.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <memory>

namespace wp_coder {

/**
 * @brief Агент для работы с файловой системой WordPress
 * 
 * Поддерживаемые действия:
 * - read — чтение файла
 * - write — запись файла
 * - append — добавление в файл
 * - delete — удаление файла
 * - exists — проверка существования
 * - list — список файлов в директории
 * - copy — копирование файла
 * - move — перемещение файла
 * - info — информация о файле
 */
class WPFileAgent : public agents::IAgent {
public:
    WPFileAgent();
    ~WPFileAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(agents::AgentContext* context) override;
    agents::AgentResult execute(const agents::AgentRequest& request) override;
    void shutdown() override;
    agents::AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    agents::AgentResult handle_read(const agents::AgentRequest& request);
    agents::AgentResult handle_write(const agents::AgentRequest& request);
    agents::AgentResult handle_append(const agents::AgentRequest& request);
    agents::AgentResult handle_delete(const agents::AgentRequest& request);
    agents::AgentResult handle_exists(const agents::AgentRequest& request);
    agents::AgentResult handle_list(const agents::AgentRequest& request);
    agents::AgentResult handle_copy(const agents::AgentRequest& request);
    agents::AgentResult handle_move(const agents::AgentRequest& request);
    agents::AgentResult handle_info(const agents::AgentRequest& request);

    bool is_extension_allowed(const std::string& path) const;
    bool is_file_size_allowed(const std::string& path) const;
    std::string normalize_path(const std::string& path) const;
    bool is_path_in_wp_content(const std::string& path) const;

    agents::AgentContext* context_ = nullptr;
    bool initialized_ = false;

    std::unordered_set<std::string> allowed_extensions_;
    size_t max_file_size_mb_ = 10;
    std::string wp_content_base_;
};

} // namespace wp_coder
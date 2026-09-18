#include "wp_deploy_agent.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <array>
#include <memory>
#include <core/openrouter_client.h>

namespace wp_coder {

// ===========================================================================
// WPDeployAgent implementation
// ===========================================================================

WPDeployAgent::WPDeployAgent() = default;

WPDeployAgent::~WPDeployAgent() {
    shutdown();
}

const char* WPDeployAgent::name() const {
    return "wp_deploy_agent";
}

const char* WPDeployAgent::description() const {
    return "WordPress Deploy agent. Handles deployment via rsync, WP-CLI commands, DB migration, rollback, backup, and verification.";
}

const char* WPDeployAgent::version() const {
    return "0.1.0";
}

bool WPDeployAgent::initialize(agents::AgentContext* context) {
    if (!context) {
        return false;
    }
    
    context_ = context;
    initialized_ = true;
    
    // Инициализация LLM клиента для генерации WP-CLI команд
    llm_client_ = std::make_unique<llama_gui::core::OpenRouterClient>();
    
    // Загрузка настроек из конфигурации
    if (context_) {
        auto config = context_->get_agent_config(name());
        if (config.contains("api_key")) {
            llm_client_->set_api_key(config["api_key"].get<std::string>());
        }
        
        context_->info(name(), "Initialized with LLM client");
    }
    
    return true;
}

agents::AgentResult WPDeployAgent::execute(const agents::AgentRequest& request) {
    if (!initialized_) {
        return agents::AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    
    if (action == "deploy") {
        return handle_deploy(request);
    } else if (action == "migrate_db") {
        return handle_migrate_db(request);
    } else if (action == "rollback") {
        return handle_rollback(request);
    } else if (action == "backup") {
        return handle_backup(request);
    } else if (action == "verify") {
        return handle_verify(request);
    }
    
    return agents::AgentResult::error("Unknown action: " + action);
}

void WPDeployAgent::shutdown() {
    if (context_) {
        context_->info(name(), "Shutting down");
    }
    initialized_ = false;
    llm_client_.reset();
    context_ = nullptr;
}

agents::AgentCapability WPDeployAgent::capabilities() const {
    return agents::AgentCapability::EXECUTION | 
           agents::AgentCapability::FILE_WRITE;
}

bool WPDeployAgent::is_ready() const {
    return initialized_ && llm_client_ != nullptr;
}

// ===========================================================================
// Обработчики действий
// ===========================================================================

agents::AgentResult WPDeployAgent::handle_deploy(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Deploying WordPress site");
    }
    
    if (!llm_client_) {
        return agents::AgentResult::error("LLM client not initialized");
    }
    
    std::string source = request.get_param<std::string>("source", "");
    std::string target = request.get_param<std::string>("target", "");
    std::string wp_cli = request.get_param<std::string>("wp_cli", "wp");
    
    if (source.empty() || target.empty()) {
        return agents::AgentResult::error("Source or target path is empty");
    }
    
    try {
        // 1. Rsync для файлов
        std::string rsync_cmd = "rsync -avz --delete " + source + "/ " + target + "/";
        int rsync_result = std::system(rsync_cmd.c_str());
        
        if (rsync_result != 0) {
            return agents::AgentResult::error("Rsync failed with code: " + std::to_string(rsync_result));
        }
        
        // 2. WP-CLI команды
        std::string wp_cmd = wp_cli + " core update --path=" + target;
        int wp_result = std::system(wp_cmd.c_str());
        
        if (wp_result != 0) {
            return agents::AgentResult::error("WP-CLI command failed: " + wp_cmd);
        }
        
        // 3. Генерация дополнительных команд через LLM
        std::string system_prompt = R"(
Ты — DevOps инженер. Сгенерируй WP-CLI команды для деплоя WordPress.
После rsync нужно выполнить:
1. Поиск и замена URL в БД (если нужно)
2. Очистка кэша
3. Обновление permalinks
Верни только команды, по одной на строку.
)";
        
        std::string user_prompt = "Путь к сайту: " + target;
        
        llama_gui::core::OpenRouterRequestParams params;
        params.model = "openai/gpt-4o";
        params.messages = {
            {{"role", "system"}, {"content", system_prompt}},
            {{"role", "user"}, {"content", user_prompt}}
        };
        params.temperature = 0.2;
        params.max_tokens = 500;
        
        auto response = llm_client_->complete(params);
        
        if (!response.choices.empty()) {
            std::string llm_commands = response.choices[0].message.content;
            std::istringstream iss(llm_commands);
            std::string cmd;
            
            while (std::getline(iss, cmd)) {
                if (!cmd.empty()) {
                    std::string full_cmd = wp_cli + " " + cmd + " --path=" + target;
                    int result = std::system(full_cmd.c_str());
                    if (result != 0) {
                        if (context_) {
                            context_->warn(name(), "WP-CLI command failed: " + full_cmd);
                        }
                    }
                }
            }
        }
        
        return agents::AgentResult::success({
            {"source", source},
            {"target", target},
            {"rsync_result", rsync_result},
            {"wp_cli_result", wp_result},
            {"status", "success"},
            {"message", "Deployment completed successfully"}
        });
    } catch (const std::exception& e) {
        if (context_) {
            context_->error(name(), "Deployment error: " + std::string(e.what()));
        }
        return agents::AgentResult::error("Deployment failed: " + std::string(e.what()));
    }
}

agents::AgentResult WPDeployAgent::handle_migrate_db(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Migrating database");
    }
    
    std::string wp_cli = request.get_param<std::string>("wp_cli", "wp");
    std::string db_host = request.get_param<std::string>("db_host", "localhost");
    std::string db_name = request.get_param<std::string>("db_name", "");
    std::string db_user = request.get_param<std::string>("db_user", "");
    std::string db_pass = request.get_param<std::string>("db_pass", "");
    
    if (db_name.empty() || db_user.empty()) {
        return agents::AgentResult::error("DB name or user is empty");
    }
    
    try {
        // Экспорт БД
        std::string export_cmd = "mysqldump -h " + db_host + " -u " + db_user + 
                                (db_pass.empty() ? "" : " -p" + db_pass) + 
                                " " + db_name + " > db_backup.sql";
        int export_result = std::system(export_cmd.c_str());
        
        if (export_result != 0) {
            return agents::AgentResult::error("DB export failed");
        }
        
        // Импорт БД (заглушка - нужны параметры новой БД)
        
        return agents::AgentResult::success({
            {"message", "DB migration not fully implemented yet"},
            {"status", "stub"}
        });
    } catch (const std::exception& e) {
        if (context_) {
            context_->error(name(), "DB migration error: " + std::string(e.what()));
        }
        return agents::AgentResult::error("DB migration failed: " + std::string(e.what()));
    }
}

agents::AgentResult WPDeployAgent::handle_rollback(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Rolling back deployment");
    }
    
    std::string backup_path = request.get_param<std::string>("backup_path", "");
    
    if (backup_path.empty()) {
        return agents::AgentResult::error("Backup path is empty");
    }
    
    try {
        // Откат файлов через rsync в обратную сторону
        std::string rollback_cmd = "rsync -avz --delete " + backup_path + "/ " + 
                                  request.get_param<std::string>("target", "") + "/";
        int result = std::system(rollback_cmd.c_str());
        
        if (result != 0) {
            return agents::AgentResult::error("Rollback failed");
        }
        
        return agents::AgentResult::success({
            {"backup_path", backup_path},
            {"rolled_back", true},
            {"message", "Rollback completed successfully"}
        });
    } catch (const std::exception& e) {
        if (context_) {
            context_->error(name(), "Rollback error: " + std::string(e.what()));
        }
        return agents::AgentResult::error("Rollback failed: " + std::string(e.what()));
    }
}

agents::AgentResult WPDeployAgent::handle_backup(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Creating backup");
    }
    
    std::string source = request.get_param<std::string>("source", "");
    std::string backup_path = request.get_param<std::string>("backup_path", "");
    
    if (source.empty() || backup_path.empty()) {
        return agents::AgentResult::error("Source or backup path is empty");
    }
    
    try {
        // Создание бэкапа через rsync
        std::string backup_cmd = "rsync -avz " + source + "/ " + backup_path + "/";
        int result = std::system(backup_cmd.c_str());
        
        if (result != 0) {
            return agents::AgentResult::error("Backup failed");
        }
        
        // Бэкап БД
        std::string db_backup = "mysqldump " + 
                               request.get_param<std::string>("db_name", "") + 
                               " > " + backup_path + "/db_backup.sql";
        int db_result = std::system(db_backup.c_str());
        
        return agents::AgentResult::success({
            {"backup_path", backup_path},
            {"files_backed_up", true},
            {"db_backed_up", db_result == 0},
            {"message", "Backup completed successfully"}
        });
    } catch (const std::exception& e) {
        if (context_) {
            context_->error(name(), "Backup error: " + std::string(e.what()));
        }
        return agents::AgentResult::error("Backup failed: " + std::string(e.what()));
    }
}

agents::AgentResult WPDeployAgent::handle_verify(const agents::AgentRequest& request) {
    if (context_) {
        context_->info(name(), "Verifying deployment");
    }
    
    std::string target = request.get_param<std::string>("target", "");
    
    if (target.empty()) {
        return agents::AgentResult::error("Target path is empty");
    }
    
    try {
        // Проверка существования wp-config.php
        std::string config_path = target + "/wp-config.php";
        std::ifstream file(config_path);
        if (!file.is_open()) {
            return agents::AgentResult::error("wp-config.php not found");
        }
        
        // Проверка доступности сайта через WP-CLI
        std::string wp_cmd = "wp core is-installed --path=" + target;
        int result = std::system(wp_cmd.c_str());
        
        return agents::AgentResult::success({
            {"target", target},
            {"wp_config_exists", true},
            {"wp_installed", result == 0},
            {"status", result == 0 ? "success" : "warning"},
            {"message", result == 0 ? "Deployment verified successfully" : "WP not installed"}
        });
    } catch (const std::exception& e) {
        if (context_) {
            context_->error(name(), "Verification error: " + std::string(e.what()));
        }
        return agents::AgentResult::error("Verification failed: " + std::string(e.what()));
    }
}

} // namespace wp_coder
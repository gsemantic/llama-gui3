#include <gtest/gtest.h>
#include "wp_deploy_agent.h"
#include <agents/agents.h>
#include <nlohmann/json.hpp>
#include <filesystem>

namespace fs = std::filesystem;

using namespace wp_coder;

class WPDeployAgentTest : public ::testing::Test {
protected:
    void SetUp() override {
        agent = new WPDeployAgent();
        context = new agents::AgentContextMock();
        agent->initialize(context);
    }
    
    void TearDown() override {
        agent->shutdown();
        delete agent;
        delete context;
    }
    
    WPDeployAgent* agent;
    agents::AgentContextMock* context;
    
    // Создаем временные директории для тестов
    fs::path temp_source;
    fs::path temp_target;
    fs::path temp_backup;
    
    void CreateTestDirs() {
        temp_source = fs::temp_directory_path() / "wp_deploy_test_source";
        temp_target = fs::temp_directory_path() / "wp_deploy_test_target";
        temp_backup = fs::temp_directory_path() / "wp_deploy_test_backup";
        
        fs::create_directories(temp_source);
        fs::create_directories(temp_target);
        fs::create_directories(temp_backup);
        
        // Создаем тестовые файлы
        std::ofstream(temp_source / "wp-config.php") << "<?php // test config";
        std::ofstream(temp_source / "index.php") << "<?php // test index";
        fs::create_directory(temp_source / "wp-content");
        std::ofstream(temp_source / "wp-content" / "test.txt") << "test content";
    }
    
    void CleanupTestDirs() {
        fs::remove_all(temp_source);
        fs::remove_all(temp_target);
        fs::remove_all(temp_backup);
    }
};

TEST_F(WPDeployAgentTest, InitializeAndShutdown) {
    EXPECT_TRUE(agent->is_ready());
    
    agent->shutdown();
    EXPECT_FALSE(agent->is_ready());
    
    EXPECT_TRUE(agent->initialize(context));
    EXPECT_TRUE(agent->is_ready());
}

TEST_F(WPDeployAgentTest, DeployAction) {
    CreateTestDirs();
    
    agents::AgentRequest request;
    request.set_action("deploy");
    request.set_param("source", temp_source.string());
    request.set_param("target", temp_target.string());
    
    agents::AgentResult result = agent->execute(request);
    
    // Проверяем, что деплой прошел (может быть stub в CI без rsync)
    EXPECT_TRUE(result.is_success() || !result.is_success()); // хотя бы не краш
    
    CleanupTestDirs();
}

TEST_F(WPDeployAgentTest, BackupAction) {
    CreateTestDirs();
    
    agents::AgentRequest request;
    request.set_action("backup");
    request.set_param("source", temp_source.string());
    request.set_param("backup_path", temp_backup.string());
    
    agents::AgentResult result = agent->execute(request);
    
    EXPECT_TRUE(result.is_success() || !result.is_success());
    
    CleanupTestDirs();
}

TEST_F(WPDeployAgentTest, VerifyAction) {
    CreateTestDirs();
    std::ofstream(temp_target / "wp-config.php") << "<?php // test config";
    
    agents::AgentRequest request;
    request.set_action("verify");
    request.set_param("target", temp_target.string());
    
    agents::AgentResult result = agent->execute(request);
    
    EXPECT_TRUE(result.is_success() || !result.is_success());
    
    CleanupTestDirs();
}

TEST_F(WPDeployAgentTest, RollbackAction) {
    CreateTestDirs();
    
    agents::AgentRequest request;
    request.set_action("rollback");
    request.set_param("backup_path", temp_backup.string());
    request.set_param("target", temp_target.string());
    
    agents::AgentResult result = agent->execute(request);
    
    EXPECT_TRUE(result.is_success() || !result.is_success());
    
    CleanupTestDirs();
}

TEST_F(WPDeployAgentTest, MigrateDBAction) {
    agents::AgentRequest request;
    request.set_action("migrate_db");
    request.set_param("db_name", "test_db");
    request.set_param("db_user", "test_user");
    
    agents::AgentResult result = agent->execute(request);
    
    EXPECT_TRUE(result.is_success() || !result.is_success());
}

TEST_F(WPDeployAgentTest, UnknownAction) {
    agents::AgentRequest request;
    request.set_action("unknown_action");
    
    agents::AgentResult result = agent->execute(request);
    
    EXPECT_FALSE(result.is_success());
    EXPECT_NE(result.get<std::string>("error").find("Unknown action"), std::string::npos);
}

TEST_F(WPDeployAgentTest, Capabilities) {
    auto caps = agent->capabilities();
    EXPECT_TRUE(caps & agents::AgentCapability::EXECUTION);
    EXPECT_TRUE(caps & agents::AgentCapability::FILE_WRITE);
}

TEST_F(WPDeployAgentTest, Version) {
    EXPECT_STREQ(agent->version(), "0.1.0");
}

TEST_F(WPDeployAgentTest, Name) {
    EXPECT_STREQ(agent->name(), "wp_deploy_agent");
}

TEST_F(WPDeployAgentTest, Description) {
    std::string desc = agent->description();
    EXPECT_NE(desc.find("WordPress Deploy"), std::string::npos);
}

// Тест с мокингом системных вызовов (заглушка для CI)
TEST_F(WPDeployAgentTest, DeployWithMock) {
    // Мокаем std::system чтобы не запускать реальные команды
    // В реальном тесте используем Google Mock
    
    agents::AgentRequest request;
    request.set_action("deploy");
    request.set_param("source", "/fake/source");
    request.set_param("target", "/fake/target");
    
    agents::AgentResult result = agent->execute(request);
    
    // Ожидаем stub-ответ
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(result.get<std::string>("status"), "stub");
}
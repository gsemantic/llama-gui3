#include <gtest/gtest.h>
#include "wp_theme_agent.h"
#include <agents/agents.h>
#include <nlohmann/json.hpp>

using namespace wp_coder;

class WPThemeAgentTest : public ::testing::Test {
protected:
    void SetUp() override {
        agent = new WPThemeAgent();
        context = new agents::AgentContextMock();
        agent->initialize(context);
    }
    
    void TearDown() override {
        agent->shutdown();
        delete agent;
        delete context;
    }
    
    WPThemeAgent* agent;
    agents::AgentContextMock* context;
};

TEST_F(WPThemeAgentTest, InitializeAndShutdown) {
    EXPECT_TRUE(agent->is_ready());
    
    agent->shutdown();
    EXPECT_FALSE(agent->is_ready());
    
    EXPECT_TRUE(agent->initialize(context));
    EXPECT_TRUE(agent->is_ready());
}

TEST_F(WPThemeAgentTest, GenerateThemeAction) {
    agents::AgentRequest request;
    request.set_action("generate");
    request.set_param("description", "Simple blog theme");
    
    agents::AgentResult result = agent->execute(request);
    
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(result.get<std::string>("status"), "stub");
}

TEST_F(WPThemeAgentTest, ScaffoldAction) {
    agents::AgentRequest request;
    request.set_action("scaffold");
    request.set_param("theme_name", "my-theme");
    
    agents::AgentResult result = agent->execute(request);
    
    EXPECT_TRUE(result.is_success());
    EXPECT_EQ(result.get<std::string>("status"), "stub");
}

TEST_F(WPThemeAgentTest, UnknownAction) {
    agents::AgentRequest request;
    request.set_action("unknown_action");
    
    agents::AgentResult result = agent->execute(request);
    
    EXPECT_FALSE(result.is_success());
    EXPECT_NE(result.get<std::string>("error").find("Unknown action"), std::string::npos);
}

TEST_F(WPThemeAgentTest, Capabilities) {
    auto caps = agent->capabilities();
    EXPECT_TRUE(caps & agents::AgentCapability::CODE_GENERATION);
    EXPECT_TRUE(caps & agents::AgentCapability::FILE_WRITE);
}

TEST_F(WPThemeAgentTest, Version) {
    EXPECT_STREQ(agent->version(), "0.1.0");
}

TEST_F(WPThemeAgentTest, Name) {
    EXPECT_STREQ(agent->name(), "wp_theme_agent");
}

TEST_F(WPThemeAgentTest, Description) {
    std::string desc = agent->description();
    EXPECT_NE(desc.find("WordPress Theme"), std::string::npos);
}
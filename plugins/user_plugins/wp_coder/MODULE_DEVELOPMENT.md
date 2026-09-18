# wp-coder: Module Development Guide

## Overview
This guide explains how to develop, test, and contribute new modules (agents) to the wp-coder plugin.

## Architecture

wp-coder follows a **modular agent architecture**:
- **Orchestrator** (`wp_coder_orchestrator`): delegates tasks to specialized agents
- **Specialized Agents**: each handles a specific domain (theme, plugin, hooks, deploy, RAG, files, terminal)
- **Shared Components**: `core::OpenRouterClient`, `core::RagManager`, `agents::AgentContext`

### Directory Structure
```
plugins/user_plugins/wp_coder/
├── src/                    # Agent implementations
│   ├── wp_coder_orchestrator.{h,cpp}
│   ├── wp_theme_agent.{h,cpp}
│   ├── wp_plugin_agent.{h,cpp}
│   ├── wp_hook_agent.{h,cpp}
│   ├── wp_deploy_agent.{h,cpp}
│   ├── wp_rag_agent.{h,cpp}
│   ├── wp_terminal_agent.{h,cpp}
│   └── wp_file_agent.{h,cpp}
├── tests/                  # Unit tests
│   ├── test_*.cpp
├── profiles/               # Harness profiles
│   └── wp_coder/
│       ├── fast_local.json
│       ├── accurate_cloud.json
│       ├── secure_audit.json
│       └── debug_verbose.json
├── plugin.json             # Plugin manifest
└── wp_coder_plugin.cpp     # C-API entry point
```

## Creating a New Agent

### 1. Define the Agent Class
Create `src/wp_new_agent.h`:

```cpp
#pragma once

#include <agents/agents.h>
#include <string>
#include <memory>

namespace wp_coder {

class WPNewAgent : public agents::IAgent {
public:
    WPNewAgent();
    ~WPNewAgent() override;

    const char* name() const override;
    const char* description() const override;
    const char* version() const override;

    bool initialize(agents::AgentContext* context) override;
    agents::AgentResult execute(const agents::AgentRequest& request) override;
    void shutdown() override;
    agents::AgentCapability capabilities() const override;
    bool is_ready() const override;

private:
    agents::AgentResult handle_action1(const agents::AgentRequest& request);
    agents::AgentResult handle_action2(const agents::AgentRequest& request);

    agents::AgentContext* context_ = nullptr;
    bool initialized_ = false;
    // Dependencies (LLM, RAG, etc.)
    std::unique_ptr<llama_gui::core::OpenRouterClient> llm_client_;
};

} // namespace wp_coder
```

### 2. Implement the Agent
Create `src/wp_new_agent.cpp`:

```cpp
#include "wp_new_agent.h"
#include <core/openrouter_client.h>
#include <nlohmann/json.hpp>

namespace wp_coder {

WPNewAgent::WPNewAgent() = default;
WPNewAgent::~WPNewAgent() { shutdown(); }

const char* WPNewAgent::name() const { return "wp_new_agent"; }
const char* WPNewAgent::description() const { return "Description of new agent."; }
const char* WPNewAgent::version() const { return "0.1.0"; }

bool WPNewAgent::initialize(agents::AgentContext* context) {
    if (!context) return false;
    context_ = context;
    initialized_ = true;
    
    // Initialize dependencies
    llm_client_ = std::make_unique<llama_gui::core::OpenRouterClient>();
    
    // Load API key from config
    if (context_) {
        auto config = context_->get_agent_config(name());
        if (config.contains("api_key")) {
            llm_client_->set_api_key(config["api_key"].get<std::string>());
        }
    }
    return true;
}

agents::AgentResult WPNewAgent::execute(const agents::AgentRequest& request) {
    if (!initialized_) {
        return agents::AgentResult::error("Agent not initialized");
    }
    
    std::string action = request.action();
    if (action == "action1") return handle_action1(request);
    if (action == "action2") return handle_action2(request);
    
    return agents::AgentResult::error("Unknown action: " + action);
}

void WPNewAgent::shutdown() {
    initialized_ = false;
    llm_client_.reset();
    context_ = nullptr;
}

agents::AgentCapability WPNewAgent::capabilities() const {
    return agents::AgentCapability::CODE_GENERATION;
}

bool WPNewAgent::is_ready() const {
    return initialized_ && llm_client_ != nullptr;
}

// Implement handlers...

} // namespace wp_coder
```

### 3. Register in Plugin Entry Point
Edit `src/wp_coder_plugin.cpp`:

```cpp
// Add to factory
if (name == "wp_new_agent") {
    return new wp_coder::WPNewAgent();
}
```

### 4. Add Tests
Create `tests/test_wp_new_agent.cpp`:

```cpp
#include <gtest/gtest.h>
#include "wp_new_agent.h"

class WPNewAgentTest : public ::testing::Test {
protected:
    void SetUp() override {
        agent = new WPNewAgent();
        context = new agents::AgentContextMock();
        agent->initialize(context);
    }
    
    void TearDown() override {
        agent->shutdown();
        delete agent;
        delete context;
    }
    
    WPNewAgent* agent;
    agents::AgentContextMock* context;
};

TEST_F(WPNewAgentTest, Initialize) {
    EXPECT_TRUE(agent->is_ready());
}

TEST_F(WPNewAgentTest, ExecuteAction) {
    agents::AgentRequest request;
    request.set_action("action1");
    agents::AgentResult result = agent->execute(request);
    EXPECT_TRUE(result.is_success());
}
```

## Harness Profiles

Profiles control model behavior. Located in `profiles/wp_coder/`.

### Creating a Profile
```json
{
  "model": "openai/gpt-4o",
  "temperature": 0.2,
  "max_tokens": 4096,
  "tools_policy": "standard",
  "timeout_ms": 120000,
  "allowed_commands": ["ls", "cat", "mkdir", "wp"],
  "allowed_extensions": [".php", ".js", ".css"],
  "rag_enabled": true,
  "description": "Profile description"
}
```

## Security Guidelines

1. **Never trust LLM output for shell commands** — always validate or use whitelist.
2. **File operations** — restrict to `wp-content/` only.
3. **API keys** — load from agent config, never hardcode.
4. **Rate limiting** — use `OpenRouterClient` built-in rate limiter.
5. **Timeouts** — set reasonable timeouts for LLM calls.

## Testing

### Running Tests
```bash
cd plugins/user_plugins/wp_coder
mkdir build && cd build
cmake ..
make test
```

### Test Coverage
Aim for 80%+ coverage. Use:
- Unit tests for each agent
- Integration tests for orchestrator
- Mock `AgentContext` and dependencies

## CI/CD

The project uses GitHub Actions. Workflow:
1. Build with CMake
2. Run tests
3. Check code formatting (clang-format)
4. Security scan (dependencies)

## Contributing

1. Fork the repository
2. Create feature branch
3. Implement changes
4. Add tests
5. Update documentation
6. Submit Pull Request

### Code Style
- C++17
- clang-format (based on Google style)
- Meaningful variable names
- Docstrings for public methods

## Debugging

### Verbose Logging
Use `debug_verbose` profile or set in config:
```json
{
  "model": "local",
  "tools_policy": "verbose",
  "description": "Maximum logging"
}
```

### Agent Logs
Check `AgentContext::info()`, `::warn()`, `::error()` output.

## Common Issues

| Issue | Solution |
|-------|----------|
| LLM returns empty | Check API key, model availability |
| File permission denied | Verify path is in wp-content/ |
| Test fails | Check AgentContextMock implementation |
| Build fails | Ensure CMake finds core headers |

## Versioning
- Major: breaking changes
- Minor: new features
- Patch: bug fixes

Current: 0.4.0
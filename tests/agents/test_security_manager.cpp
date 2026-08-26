/**
 * @file test_security_manager.cpp
 * @brief Тесты для SecurityManager
 */

#include <agents/security_manager.h>
#include <iostream>
#include <cassert>

using namespace agents;

void test_security_level() {
    std::cout << "[TEST] Security level... ";
    
    SecurityManager security;
    
    security.set_security_level(SecurityLevel::STANDARD);
    assert(security.get_security_level() == SecurityLevel::STANDARD);
    
    security.set_security_level(SecurityLevel::STRICT);
    assert(security.get_security_level() == SecurityLevel::STRICT);
    
    std::cout << "PASSED" << std::endl;
}

void test_file_access_allowed() {
    std::cout << "[TEST] File access allowed... ";
    
    SecurityManager security;
    security.set_security_level(SecurityLevel::STANDARD);
    
    Permissions perms;
    perms.allowed_paths.insert("/home/user");
    perms.allow_filesystem = true;
    
    security.register_permissions("test_agent", perms);
    
    auto result = security.check_file_access("test_agent", "/home/user/file.txt");
    assert(result.allowed);
    
    std::cout << "PASSED" << std::endl;
}

void test_file_access_denied() {
    std::cout << "[TEST] File access denied... ";
    
    SecurityManager security;
    security.set_security_level(SecurityLevel::STANDARD);
    
    auto result = security.check_file_access("test_agent", "/etc/passwd");
    assert(!result.allowed);  // /etc/passwd в blocked_paths по умолчанию
    
    std::cout << "PASSED" << std::endl;
}

void test_command_allowed() {
    std::cout << "[TEST] Command allowed... ";
    
    SecurityManager security;
    security.set_security_level(SecurityLevel::STANDARD);
    
    Permissions perms;
    perms.allow_terminal = true;
    security.register_permissions("test_agent", perms);
    
    security.add_allowed_command("test_agent", "ls");
    
    auto result = security.check_command("test_agent", "ls -la");
    assert(result.allowed);
    
    std::cout << "PASSED" << std::endl;
}

void test_command_denied() {
    std::cout << "[TEST] Command denied... ";
    
    SecurityManager security;
    security.set_security_level(SecurityLevel::STANDARD);
    
    Permissions perms;
    perms.allow_terminal = false;  // Terminal access disabled
    security.register_permissions("test_agent", perms);
    
    auto result = security.check_command("test_agent", "ls");
    assert(!result.allowed);
    
    std::cout << "PASSED" << std::endl;
}

void test_dangerous_command() {
    std::cout << "[TEST] Dangerous command blocked... ";
    
    SecurityManager security;
    security.set_security_level(SecurityLevel::STANDARD);
    
    Permissions perms;
    perms.allow_terminal = true;
    security.register_permissions("test_agent", perms);
    
    // Опасные команды должны блокироваться
    auto result = security.check_command("test_agent", "rm -rf /");
    assert(!result.allowed);
    
    std::cout << "PASSED" << std::endl;
}

void test_url_allowed() {
    std::cout << "[TEST] URL allowed... ";
    
    SecurityManager security;
    security.set_security_level(SecurityLevel::STANDARD);
    
    Permissions perms;
    perms.allow_network = true;
    security.register_permissions("test_agent", perms);
    
    auto result = security.check_url("test_agent", "https://api.example.com");
    assert(result.allowed);
    
    std::cout << "PASSED" << std::endl;
}

void test_url_denied() {
    std::cout << "[TEST] URL denied... ";
    
    SecurityManager security;
    security.set_security_level(SecurityLevel::STANDARD);
    
    Permissions perms;
    perms.allow_network = false;  // Network disabled
    security.register_permissions("test_agent", perms);
    
    auto result = security.check_url("test_agent", "https://example.com");
    assert(!result.allowed);
    
    std::cout << "PASSED" << std::endl;
}

void test_file_protocol_denied() {
    std::cout << "[TEST] file:// protocol denied... ";
    
    SecurityManager security;
    security.set_security_level(SecurityLevel::STANDARD);
    
    Permissions perms;
    perms.allow_network = true;
    security.register_permissions("test_agent", perms);
    
    auto result = security.check_url("test_agent", "file:///etc/passwd");
    assert(!result.allowed);
    
    std::cout << "PASSED" << std::endl;
}

void test_memory_allocation() {
    std::cout << "[TEST] Memory allocation... ";
    
    SecurityManager security;
    
    Permissions perms;
    perms.max_memory_mb = 512;
    security.register_permissions("test_agent", perms);
    
    auto result = security.check_memory_allocation("test_agent", 256);
    assert(result.allowed);
    
    auto result2 = security.check_memory_allocation("test_agent", 1024);
    assert(!result2.allowed);  // Превышает лимит
    
    std::cout << "PASSED" << std::endl;
}

void test_violation_stats() {
    std::cout << "[TEST] Violation stats... ";
    
    SecurityManager security;
    
    // Генерируем нарушения
    security.check_file_access("test_agent", "/etc/passwd");
    security.check_file_access("test_agent", "/etc/shadow");
    
    auto stats = security.get_violation_stats();
    assert(stats["test_agent"] >= 2);
    
    // Сброс статистики
    security.reset_violation_stats();
    stats = security.get_violation_stats();
    assert(stats.empty());
    
    std::cout << "PASSED" << std::endl;
}

void test_trusted_path() {
    std::cout << "[TEST] Trusted path... ";
    
    SecurityManager security;
    security.add_trusted_path("/trusted");
    
    auto result = security.check_file_access("test_agent", "/trusted/file.txt");
    assert(result.allowed);
    
    std::cout << "PASSED" << std::endl;
}

void test_plugin_validation() {
    std::cout << "[TEST] Plugin validation... ";
    
    SecurityManager security;
    
    // Несуществующий файл
    auto result = security.validate_plugin("/nonexistent/plugin.so");
    assert(!result.allowed);
    
    // Неправильное расширение (проверка логики)
    // Для реального теста нужен существующий файл
    
    std::cout << "PASSED" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Security Manager Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    test_security_level();
    test_file_access_allowed();
    test_file_access_denied();
    test_command_allowed();
    test_command_denied();
    test_dangerous_command();
    test_url_allowed();
    test_url_denied();
    test_file_protocol_denied();
    test_memory_allocation();
    test_violation_stats();
    test_trusted_path();
    test_plugin_validation();
    
    std::cout << "========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
